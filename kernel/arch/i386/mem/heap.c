#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

heap_t kernel_heap;

#define CANARY_OVERHEAD (sizeof(uint32_t) * 2)

static inline uint32_t *get_head_canary(heap_block_t *block) {
    return (uint32_t *)((uint8_t *)block + sizeof(heap_block_t));
}

static inline uint32_t *get_tail_canary(heap_block_t *block) {
    return (uint32_t *)((uint8_t *)block + sizeof(heap_block_t) + 
                        sizeof(uint32_t) + block->alloc_size);
}

static inline void *get_user_ptr(heap_block_t *block) {
    return (void *)((uint8_t *)block + sizeof(heap_block_t) + sizeof(uint32_t));
}

static inline heap_block_t *get_block_from_ptr(void *ptr) {
    return (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t) - sizeof(uint32_t));
}

static void set_canaries(heap_block_t *block) {
    *get_head_canary(block) = HEAP_CANARY_HEAD;
    *get_tail_canary(block) = HEAP_CANARY_TAIL;
}

int heap_check_canaries(void *ptr) {
    if (!ptr) return -1;
    
    heap_block_t *block = get_block_from_ptr(ptr);
    
    if (block->magic != HEAP_MAGIC) {
        printf("heap_check_canaries: bad magic 0x%08X at block 0x%08X\n", 
               block->magic, (uint32_t)block);
        return -1;
    }
    
    uint32_t head = *get_head_canary(block);
    uint32_t tail = *get_tail_canary(block);
    
    if (head != HEAP_CANARY_HEAD) {
        printf("HEAP CORRUPTION: Head canary corrupted at 0x%08X (expected 0x%08X, got 0x%08X)\n",
               (uint32_t)ptr, HEAP_CANARY_HEAD, head);
        return -1;
    }
    
    if (tail != HEAP_CANARY_TAIL) {
        printf("HEAP CORRUPTION: Tail canary corrupted at 0x%08X (expected 0x%08X, got 0x%08X) - buffer overflow!\n",
               (uint32_t)ptr, HEAP_CANARY_TAIL, tail);
        printf("  Block size: %u, alloc_size: %u\n", block->size, block->alloc_size);
        return -1;
    }
    
    return 0;
}

int heap_validate_ptr(void *ptr) {
    if (!ptr) return -1;
    
    heap_block_t *block = get_block_from_ptr(ptr);
    
    uint32_t block_addr = (uint32_t)block;
    if (block_addr < kernel_heap.start_addr || 
        block_addr >= kernel_heap.end_addr) {
        printf("heap_validate_ptr: block 0x%08X outside heap bounds\n", block_addr);
        return -1;
    }
    
    if (block->magic != HEAP_MAGIC) {
        printf("heap_validate_ptr: bad magic 0x%08X\n", block->magic);
        return -1;
    }
    
    if (block->is_free) {
        printf("heap_validate_ptr: block is free (use-after-free?)\n");
        return -1;
    }
    
    return heap_check_canaries(ptr);
}

static void poison_memory(void *ptr, size_t size, uint8_t pattern) {
    memset(ptr, pattern, size);
}

static heap_block_t *find_best_fit(size_t size) {
    heap_block_t *best = NULL;
    heap_block_t *current = kernel_heap.free_list;

    while (current) {
        if (current->magic != HEAP_MAGIC) {
            printf("Heap corruption detected at 0x%08X\n", (uint32_t)current);
            heap_verify();
            dump_map_debug();
            return NULL;
        }

        if (current->is_free && current->size >= size) {
            if (!best || current->size < best->size) {
                best = current;
                if (current->size == size) break;
            }
        }
        current = current->next;
    }

    return best;
}

static void split_block(heap_block_t *block, size_t size) {
    if (block->size < size + sizeof(heap_block_t) + HEAP_MIN_BLOCK) return;

    size_t remaining = block->size - size - sizeof(heap_block_t);

    if (remaining >= HEAP_MIN_BLOCK) {
        heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + sizeof(heap_block_t) + size);
        new_block->magic = HEAP_MAGIC;
        new_block->size = remaining;
        new_block->alloc_size = 0;
        new_block->flags = 0;
        new_block->is_free = 1;
        new_block->next = block->next;
        new_block->prev = block;

        if (block->next) {
            block->next->prev = new_block;
        }
        block->next = new_block;
        block->size = size;
    }
}

static void coalesce_free_blocks(heap_block_t *block) {
    if (block->next && block->next->is_free) {
        heap_block_t *next = block->next;
        uint32_t block_end = (uint32_t)block + sizeof(heap_block_t) + block->size;
        if (block_end == (uint32_t)next) {
            block->size += sizeof(heap_block_t) + next->size;
            block->next = next->next;
            if (block->next) {
                block->next->prev = block;
            }
            next->magic = 0xDEAD0001;
        } else {
            DPRINTF3("coalesce: skip non-adjacent next (block_end=0x%08X next=0x%08X)\n", block_end, (uint32_t)next);
        }
    }

    if (block->prev && block->prev->is_free) {
        heap_block_t *prev = block->prev;
        uint32_t prev_end = (uint32_t)prev + sizeof(heap_block_t) + prev->size;
        if (prev_end == (uint32_t)block) {
            prev->size += sizeof(heap_block_t) + block->size;
            prev->next = block->next;
            if (block->next) {
                block->next->prev = prev;
            }
            block->magic = 0xDEAD0002;
        } else {
            DPRINTF3("coalesce: skip non-adjacent prev (prev_end=0x%08X block=0x%08X)\n", prev_end, (uint32_t)block);
        }
    }
}

extern int heap_map_page(uint32_t virt_addr);

static int heap_expand(uint32_t new_end) {
    uint32_t addr;
    
    DPRINTF4("heap_expand: request new_end=0x"); DEBUG_HEX4(new_end); DPRINTF4("\n");
    if (new_end > kernel_heap.max_addr) {
        new_end = kernel_heap.max_addr;
    }

    new_end = (new_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_end <= kernel_heap.end_addr) return 0;

    for (addr = kernel_heap.end_addr; addr < new_end; addr += PAGE_SIZE) {
        if (heap_map_page(addr) < 0) {
            printf("heap_expand: Failed to map page at 0x%08X\n", addr);
            return -1;
        }
    }

    kernel_heap.end_addr = new_end;
    kernel_heap.total_size = new_end - kernel_heap.start_addr;
    DPRINTF("heap_expand: new end=0x%08X\n", kernel_heap.end_addr);
    return 0;
}

void heap_init(void) {
    uint32_t *pd;
    uint32_t pd_idx;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt;
    heap_block_t *initial_block;
    
    kernel_heap.start_addr = HEAP_START;
    kernel_heap.end_addr = HEAP_START;
    kernel_heap.max_addr = HEAP_START + HEAP_MAX_SIZE;
    kernel_heap.free_list = NULL;
    kernel_heap.total_size = 0;
    kernel_heap.used_size = 0;

    heap_expand(HEAP_START + HEAP_INITIAL_SIZE);

    extern uint32_t pae_enabled;
    if (!pae_enabled) {
        pd = (uint32_t *)0xFFFFF000;
        pd_idx = HEAP_START >> 22;
        if (!(pd[pd_idx] & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("heap_init: Failed to allocate low page for heap PDE!\n");
            } else {
                pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | 3;
                pt_addr = 0xFFC00000 + (pd_idx << 12);
                __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
                pt = (uint32_t *)pt_addr;
                memset(pt, 0, PAGE_SIZE);
                printf("heap_init: PDE[%u] created for heap (PDE=0x%08X)\n", pd_idx, pd[pd_idx]);
            }
        }
    }

    initial_block = (heap_block_t *)HEAP_START;
    initial_block->magic = HEAP_MAGIC;
    initial_block->size = kernel_heap.total_size - sizeof(heap_block_t);
    initial_block->alloc_size = 0;
    initial_block->flags = 0;
    initial_block->is_free = 1;
    initial_block->next = NULL;
    initial_block->prev = NULL;

    kernel_heap.free_list = initial_block;

    printf("Heap initialized: 0x%08X - 0x%08X (%u KB) [with canary protection]\n",
           kernel_heap.start_addr, kernel_heap.end_addr,
           kernel_heap.total_size / 1024);
}

void *kmalloc(size_t size) {
    size_t orig_size;
    size_t total_size;
    heap_block_t *block;
    uint32_t old_end;
    uint32_t needed;
    uint32_t new_end;
    heap_block_t *adjacent;
    heap_block_t *iter;
    uint32_t iter_end;
    uint32_t added;
    heap_block_t *new_block;
    uint32_t new_block_size;
    heap_block_t *prev;
    heap_block_t *cur;
    void *ptr;
    
    if (size == 0) return NULL;
    
    if (size > SIZE_MAX - CANARY_OVERHEAD - 7) {
        printf("kmalloc: size overflow detected\n");
        return NULL;
    }

    orig_size = size;
    total_size = size + CANARY_OVERHEAD;
    total_size = (total_size + 7) & ~7;

    block = find_best_fit(total_size);

    if (!block) {
        old_end = kernel_heap.end_addr;
        needed = total_size + sizeof(heap_block_t) + PAGE_SIZE;
        new_end = old_end + needed;

        DPRINTF("kmalloc: expanding to new_end=0x%08X\n", new_end);
        if (new_end > kernel_heap.max_addr) {
            printf("kmalloc: Heap exhausted\n");
            return NULL;
        }

        if (heap_expand(new_end) < 0) {
            printf("kmalloc: heap_expand failed\n");
            return NULL;
        }

        if (kernel_heap.end_addr > kernel_heap.max_addr) {
            printf("kmalloc: After expand, end > max (0x%08X > 0x%08X)\n", kernel_heap.end_addr, kernel_heap.max_addr);
            return NULL;
        }

        adjacent = NULL;
        for (iter = kernel_heap.free_list; iter; iter = iter->next) {
            iter_end = (uint32_t)iter + sizeof(heap_block_t) + iter->size;
            if (iter->is_free && iter_end == old_end) {
                adjacent = iter;
                break;
            }
        }

        if (adjacent) {
            added = kernel_heap.end_addr - old_end;
            adjacent->size += added;
        } else {
            if (kernel_heap.end_addr <= old_end + sizeof(heap_block_t)) {
                printf("kmalloc: Expand produced no usable space\n");
                return NULL;
            }

            new_block = (heap_block_t *)old_end;
            new_block_size = kernel_heap.end_addr - old_end - sizeof(heap_block_t);
            DPRINTF("kmalloc: new_block at 0x"); if (debugMode) print_hex((uint32_t)new_block); DPRINTF("\n");
            new_block->magic = HEAP_MAGIC;
            new_block->size = new_block_size;
            new_block->alloc_size = 0;
            new_block->flags = 0;
            new_block->is_free = 1;
            new_block->next = NULL;
            new_block->prev = NULL;

            prev = NULL;
            cur = kernel_heap.free_list;
            while (cur && (uint32_t)cur < (uint32_t)new_block) {
                prev = cur;
                cur = cur->next;
            }
            if (!prev) {
                new_block->next = kernel_heap.free_list;
                if (kernel_heap.free_list) kernel_heap.free_list->prev = new_block;
                kernel_heap.free_list = new_block;
            } else {
                new_block->next = prev->next;
                new_block->prev = prev;
                if (prev->next) prev->next->prev = new_block;
                prev->next = new_block;
            }

            coalesce_free_blocks(new_block);
        }

        block = find_best_fit(total_size);
        if (!block) {
            printf("kmalloc: Failed after expand\n");
            return NULL;
        }
    }

    split_block(block, total_size);

    block->is_free = 0;
    block->alloc_size = orig_size;
    block->flags = 0;
    kernel_heap.used_size += block->size + sizeof(heap_block_t);

    set_canaries(block);
    
    ptr = get_user_ptr(block);

    DPRINTF4("kmalloc: alloc size=%u (total=%u) block=0x%08X ptr=0x%08X\n", 
             (uint32_t)orig_size, (uint32_t)total_size, (uint32_t)block, (uint32_t)ptr);

    if (((uintptr_t)ptr & 0x3) != 0) {
        printf("kmalloc: WARNING - returned pointer not 4-byte aligned: 0x%08X\n", (uint32_t)ptr);
        heap_verify();
    }

    return ptr;
}

void *ksafe_alloc(size_t size, uint32_t flags) {
    if (size == 0) return NULL;
    
    if (size > SIZE_MAX - CANARY_OVERHEAD - 7) {
        printf("ksafe_alloc: size overflow detected\n");
        return NULL;
    }
    
    void *ptr = kmalloc(size);
    if (!ptr) return NULL;
    
    heap_block_t *block = get_block_from_ptr(ptr);
    block->flags = flags;
    
    if (flags & KMALLOC_ZERO) {
        memset(ptr, 0, size);
    } else if (!(flags & KMALLOC_NO_POISON)) {
        #ifdef DEBUG
        poison_memory(ptr, size, HEAP_POISON_ALLOC);
        #endif
    }
    
    return ptr;
}

void *kcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    
    if (nmemb > SIZE_MAX / size) {
        printf("kcalloc: integer overflow detected (%u * %u)\n", 
               (uint32_t)nmemb, (uint32_t)size);
        return NULL;
    }
    
    size_t total = nmemb * size;
    return ksafe_alloc(total, KMALLOC_ZERO);
}

void *kmalloc_aligned(size_t size, uint32_t alignment) {
    if (size == 0) return NULL;
    if (alignment == 0) alignment = 1;

    if (size > SIZE_MAX - alignment - sizeof(void *)) {
        printf("kmalloc_aligned: size overflow\n");
        return NULL;
    }

    size_t alloc_size = size + alignment + sizeof(void *);
    void *ptr = kmalloc(alloc_size);
    if (!ptr) return NULL;

    uintptr_t addr = (uintptr_t)ptr + sizeof(void *);
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);

    ((void **)aligned)[-1] = ptr;

    return (void *)aligned;
}

void kfree(void *ptr) {
    if (!ptr) return;

    heap_block_t *block = get_block_from_ptr(ptr);

    DPRINTF5("kfree: ptr=0x%08X block=0x%08X magic=0x%08X size=%u\n",
             (uint32_t)ptr, (uint32_t)block, block->magic, block->size);

    if (block->magic != HEAP_MAGIC) {
        printf("kfree: Invalid pointer 0x%08X (bad magic 0x%08X)\n", (uint32_t)ptr, block->magic);
        heap_verify();
        return;
    }

    if (block->is_free) {
        printf("kfree: Double free detected at 0x%08X\n", (uint32_t)ptr);
        return;
    }
    
    if (heap_check_canaries(ptr) != 0) {
        printf("kfree: Memory corruption detected for ptr 0x%08X - refusing to free\n", (uint32_t)ptr);
        return;
    }
    
    if (block->flags & KMALLOC_SECURE) {
        memset(ptr, 0, block->alloc_size);
    } else if (!(block->flags & KMALLOC_NO_POISON)) {
        poison_memory(ptr, block->alloc_size, HEAP_POISON_FREED);
    }

    block->is_free = 1;
    kernel_heap.used_size -= block->size + sizeof(heap_block_t);

    coalesce_free_blocks(block);
}

void ksafe_free(void *ptr) {
    kfree(ptr); 
}

void kfree_secure(void *ptr) {
    if (!ptr) return;
    
    heap_block_t *block = get_block_from_ptr(ptr);
    
    if (block->magic != HEAP_MAGIC) {
        printf("kfree_secure: Invalid pointer 0x%08X\n", (uint32_t)ptr);
        return;
    }
    
    if (block->is_free) {
        printf("kfree_secure: Double free detected at 0x%08X\n", (uint32_t)ptr);
        return;
    }
    
    if (heap_check_canaries(ptr) != 0) {
        printf("kfree_secure: Memory corruption at 0x%08X\n", (uint32_t)ptr);
        return;
    }
    
    memset(ptr, 0, block->alloc_size);
    
    block->is_free = 1;
    kernel_heap.used_size -= block->size + sizeof(heap_block_t);
    
    coalesce_free_blocks(block);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    heap_block_t *block = get_block_from_ptr(ptr);

    if (block->magic != HEAP_MAGIC) {
        printf("krealloc: Invalid pointer\n");
        return NULL;
    }
    
    if (heap_check_canaries(ptr) != 0) {
        printf("krealloc: Memory corruption detected, refusing to reallocate\n");
        return NULL;
    }

    if (block->alloc_size >= new_size) {
        block->alloc_size = new_size;
        set_canaries(block);
        return ptr;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, block->alloc_size);
    kfree(ptr);

    return new_ptr;
}

void heap_dump(void) {
    printf("=== Heap Dump ===\n");
    printf("Start: 0x%08X  End: 0x%08X  Max: 0x%08X\n",
           kernel_heap.start_addr, kernel_heap.end_addr, kernel_heap.max_addr);
    printf("Total: %u KB  Used: %u KB  Free: %u KB\n",
           kernel_heap.total_size / 1024,
           kernel_heap.used_size / 1024,
           (kernel_heap.total_size - kernel_heap.used_size) / 1024);

    heap_block_t *block = kernel_heap.free_list;
    uint32_t count = 0;
    while (block && count < 20) {
        printf("Block %u: addr=0x%08X size=%u %s\n",
               count, (uint32_t)block, block->size,
               block->is_free ? "FREE" : "USED");
        block = block->next;
        count++;
    }
    printf("=================\n");
}

uint32_t heap_free_space(void) {
    uint32_t free = 0;
    heap_block_t *block = kernel_heap.free_list;
    while (block) {
        if (block->is_free) {
            free += block->size;
        }
        block = block->next;
    }
    return free;
}

uint32_t heap_block_size_for_ptr(void *ptr) {
    if (!ptr) return 0;
    heap_block_t *block = get_block_from_ptr(ptr);
    if (block->magic != HEAP_MAGIC) return 0;
    return block->alloc_size; 
}

static void dump_memory_around(uint32_t addr, uint32_t radius) {
    uint32_t start = (addr >= radius) ? (addr - radius) : 0;
    uint32_t end = addr + radius;
    if (start < kernel_heap.start_addr) start = kernel_heap.start_addr;
    if (end > kernel_heap.end_addr) end = kernel_heap.end_addr;

    printf(" dump mem around 0x%08X (0x%08X - 0x%08X):\n", addr, start, end);
    for (uint32_t a = start; a < end; a += 4) {
        printf("  0x%08X: 0x%08X\n", a, *(volatile uint32_t *)a);
    }
}

void heap_verify(void) {
    DPRINTF4("heap_verify: start: free_list=0x%08X\n", (uint32_t)kernel_heap.free_list);
    heap_block_t *block = kernel_heap.free_list;
    int i = 0;
    int corruption_found = 0;
    
    while (block) {
        DPRINTF4(" heap block %d: addr=0x%08X size=%u alloc_size=%u free=%u magic=0x%08X next=0x%08X prev=0x%08X\n",
               i, (uint32_t)block, block->size, block->alloc_size, block->is_free, block->magic,
               (uint32_t)block->next, (uint32_t)block->prev);

        uint32_t baddr = (uint32_t)block;
        if (baddr < kernel_heap.start_addr || baddr + sizeof(heap_block_t) > kernel_heap.end_addr) {
            printf("heap_verify: ERROR - block header out of heap bounds: 0x%08X\n", baddr);
            dump_memory_around(baddr, 64);
            break;
        }

        if (block->magic != HEAP_MAGIC) {
            printf("heap_verify: ERROR - invalid magic at 0x%08X (magic=0x%08X)\n", baddr, block->magic);
            dump_memory_around(baddr, 64);
            break;
        }

        uint32_t block_end = baddr + sizeof(heap_block_t) + block->size;
        if (block_end > kernel_heap.end_addr) {
            printf("heap_verify: ERROR - block extends past heap end: block_end=0x%08X heap_end=0x%08X\n", block_end, kernel_heap.end_addr);
            dump_memory_around(baddr, 64);
            break;
        }
        
        if (!block->is_free && block->alloc_size > 0) {
            uint32_t *head_canary = get_head_canary(block);
            uint32_t *tail_canary = get_tail_canary(block);
            
            if (*head_canary != HEAP_CANARY_HEAD) {
                printf("heap_verify: ERROR - head canary corrupted at block 0x%08X (got 0x%08X)\n", 
                       baddr, *head_canary);
                corruption_found++;
            }
            if (*tail_canary != HEAP_CANARY_TAIL) {
                printf("heap_verify: ERROR - tail canary corrupted at block 0x%08X (got 0x%08X) - BUFFER OVERFLOW!\n", 
                       baddr, *tail_canary);
                corruption_found++;
            }
        }

        if (block->next) {
            uint32_t naddr = (uint32_t)block->next;
            if (naddr <= baddr) {
                printf("heap_verify: ERROR - next pointer not ascending (0x%08X -> 0x%08X)\n", baddr, naddr);
                dump_memory_around(baddr, 64);
                break;
            }
            if (block->next->prev != block) {
                printf("heap_verify: ERROR - next->prev inconsistency at 0x%08X\n", naddr);
                dump_memory_around(naddr, 64);
                break;
            }
            if (naddr < block_end) {
                printf("heap_verify: ERROR - overlapping blocks 0x%08X and 0x%08X (block_end=0x%08X)\n", baddr, naddr, block_end);
                dump_memory_around(baddr, 64);
                break;
            }
        }

        block = block->next;
        i++;
        if (i > 100) { printf(" heap_verify: stopping early (>100)\n"); break; }
    }
    
    if (corruption_found > 0) {
        printf("heap_verify: FOUND %d CANARY CORRUPTIONS!\n", corruption_found);
    }
}

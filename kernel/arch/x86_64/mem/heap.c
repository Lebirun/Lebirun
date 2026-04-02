#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <kernel/spinlock.h>
#include <string.h>

heap_t kernel_heap;
static spinlock_t heap_lock = {0};
static volatile uint64_t heap_saved_eflags = 0;

static inline void heap_lock_acquire(void) {
    uint64_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli" ::: "memory");
    spin_lock(&heap_lock);
    heap_saved_eflags = eflags;
}

static inline void heap_lock_release(void) {
    uint64_t eflags = heap_saved_eflags;
    spin_unlock(&heap_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

#define CANARY_OVERHEAD (sizeof(uint64_t) * 2)
#define HEAP_USE_DEMAND_PAGING 1

#define EARLY_HEAP_SIZE (16 * 1024)
static uint8_t early_heap_buffer[EARLY_HEAP_SIZE] __attribute__((aligned(4096)));
static uint64_t early_heap_offset = 0;
static int main_heap_initialized = 0;

static void *early_kmalloc(size_t size) {
    void *ptr;

    size = (size + 15) & ~15;
    if (early_heap_offset + size > EARLY_HEAP_SIZE) return NULL;
    ptr = &early_heap_buffer[early_heap_offset];
    memset(ptr, 0, size);
    early_heap_offset += size;
    return ptr;
}

int is_early_heap_ptr(void *ptr) {
    uint64_t addr;

    if (!ptr) return 0;
    addr = (uint64_t)ptr;
    return (addr >= (uint64_t)early_heap_buffer &&
            addr < (uint64_t)early_heap_buffer + EARLY_HEAP_SIZE);
}

static inline uint64_t *get_head_canary(heap_block_t *block) {
    return (uint64_t *)((uint8_t *)block + sizeof(heap_block_t));
}

static inline uint64_t *get_tail_canary(heap_block_t *block) {
    return (uint64_t *)((uint8_t *)block + sizeof(heap_block_t) + 
                        sizeof(uint64_t) + block->alloc_size);
}

static inline void *get_user_ptr(heap_block_t *block) {
    return (void *)((uint8_t *)block + sizeof(heap_block_t) + sizeof(uint64_t));
}

static inline heap_block_t *get_block_from_ptr(void *ptr) {
    return (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t) - sizeof(uint64_t));
}

static void set_canaries(heap_block_t *block) {
    *get_head_canary(block) = HEAP_CANARY_HEAD;
    *get_tail_canary(block) = HEAP_CANARY_TAIL;
}

int heap_check_canaries(void *ptr) {
    heap_block_t *block;
    uint64_t head;
    uint64_t tail;
    
    if (!ptr) return -1;
    if (is_early_heap_ptr(ptr)) return 0;
    
    block = get_block_from_ptr(ptr);
    
    if (block->magic != HEAP_MAGIC) {
        printf("heap_check_canaries: bad magic 0x%08lX at block 0x%016lX\n", 
               block->magic, (uint64_t)block);
        return -1;
    }
    
    head = *get_head_canary(block);
    tail = *get_tail_canary(block);
    
    if (head != HEAP_CANARY_HEAD) {
        printf("HEAP CORRUPTION: Head canary corrupted at 0x%016lX (expected 0x%08lX, got 0x%08lX)\n",
               (uint64_t)ptr, (unsigned long)HEAP_CANARY_HEAD, head);
        return -1;
    }
    
    if (tail != HEAP_CANARY_TAIL) {
        printf("HEAP CORRUPTION: Tail canary corrupted at 0x%016lX (expected 0x%08lX, got 0x%08lX) - buffer overflow!\n",
               (uint64_t)ptr, (unsigned long)HEAP_CANARY_TAIL, tail);
        printf("  Block size: %u, alloc_size: %u\n", block->size, block->alloc_size);
        return -1;
    }
    
    return 0;
}

int heap_validate_ptr(void *ptr) {
    heap_block_t *block;
    uint64_t block_addr;
    
    if (!ptr) return -1;
    if (is_early_heap_ptr(ptr)) return 0;
    
    block = get_block_from_ptr(ptr);
    
    block_addr = (uint64_t)block;
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
    heap_block_t *best;
    heap_block_t *current;
    
    best = NULL;
    current = kernel_heap.free_list;

    while (current) {
        if (current->magic != HEAP_MAGIC) {
            printf("Heap corruption detected at 0x%08X\n", (uint64_t)current);
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
    size_t remaining;
    heap_block_t *new_block;
    
    if (block->size < size + sizeof(heap_block_t) + HEAP_MIN_BLOCK) return;

    remaining = block->size - size - sizeof(heap_block_t);

    if (remaining >= HEAP_MIN_BLOCK) {
        new_block = (heap_block_t *)((uint8_t *)block + sizeof(heap_block_t) + size);
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
    heap_block_t *next;
    heap_block_t *prev;
    uint64_t block_end;
    uint64_t prev_end;
    
    if (block->next && block->next->is_free) {
        next = block->next;
        block_end = (uint64_t)block + sizeof(heap_block_t) + block->size;
        if (block_end == (uint64_t)next) {
            block->size += sizeof(heap_block_t) + next->size;
            block->next = next->next;
            if (block->next) {
                block->next->prev = block;
            }
            next->magic = 0xDEAD0001;
        } else {
            DEBUG_MEMORY("coalesce: skip non-adjacent next (block_end=0x%08X next=0x%08X)\n", block_end, (uint64_t)next);
        }
    }

    if (block->prev && block->prev->is_free) {
        prev = block->prev;
        prev_end = (uint64_t)prev + sizeof(heap_block_t) + prev->size;
        if (prev_end == (uint64_t)block) {
            prev->size += sizeof(heap_block_t) + block->size;
            prev->next = block->next;
            if (block->next) {
                block->next->prev = prev;
            }
            block->magic = 0xDEAD0002;
            block = prev;
        } else {
            DEBUG_MEMORY("coalesce: skip non-adjacent prev (prev_end=0x%08X block=0x%08X)\n", prev_end, (uint64_t)block);
        }
    }

#if HEAP_USE_DEMAND_PAGING
    {
        uint64_t region_start;
        uint64_t region_end;
        uint64_t first_page;
        uint64_t last_page;
        uint64_t pg;

        region_start = (uint64_t)block + sizeof(heap_block_t);
        region_end = (uint64_t)block + sizeof(heap_block_t) + block->size;

        first_page = (region_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        last_page = region_end & ~(PAGE_SIZE - 1);

        for (pg = first_page; pg < last_page; pg += PAGE_SIZE) {
            demand_decommit_page(pg);
        }
    }
#endif
}

static void heap_trim(void) {
    heap_block_t *last;
    heap_block_t *cur;
    uint64_t block_end;
    uint64_t trim_start;
    uint64_t new_end;
    uint64_t pg;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t pde;
    uint64_t *pt64;
    uint64_t pte;
    uint64_t phys;
    uint64_t pdpt_idx;
    uint64_t pdpte;
    uint64_t *pd;
    extern uint64_t boot_pdpt_high[];
    uint64_t *kv_pdpt;

    kv_pdpt = (uint64_t *)((uintptr_t)boot_pdpt_high + KERNEL_VMA);
    last = NULL;
    cur = kernel_heap.free_list;
    while (cur) {
        last = cur;
        cur = cur->next;
    }

    if (!last || !last->is_free) return;

    block_end = (uint64_t)last + sizeof(heap_block_t) + last->size;
    if (block_end != kernel_heap.end_addr) return;

    trim_start = ((uint64_t)last + sizeof(heap_block_t) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (trim_start >= kernel_heap.end_addr) return;

    for (pg = trim_start; pg < kernel_heap.end_addr; pg += PAGE_SIZE) {
        pdpt_idx = (pg >> 30) & 0x1FF;
        pdpte = kv_pdpt[pdpt_idx];
        if (!(pdpte & 1)) continue;
        pd = (uint64_t *)((pdpte & ~0xFFFULL) + KERNEL_VMA);
        pd_idx = (pg >> 21) & 0x1FF;
        pt_idx = (pg >> 12) & 0x1FF;
        pde = pd[pd_idx];
        if (!(pde & 1)) continue;
        if (pde & 0x80) continue;
        pt64 = (uint64_t *)((pde & ~0xFFFULL) + KERNEL_VMA);
        pte = pt64[pt_idx];
        if (pte & 1) {
            phys = (pte & ~0xFFFULL);
            pt64[pt_idx] = 0;
            __asm__ volatile("invlpg (%0)" : : "r"(pg) : "memory");
            if (phys >= 0x1000) pfa_free(phys);
        }
    }

    new_end = trim_start;
    last->size = new_end - (uint64_t)last - sizeof(heap_block_t);

    if (last->size == 0) {
        if (last->prev) {
            last->prev->next = NULL;
        } else {
            kernel_heap.free_list = NULL;
        }
        new_end = (uint64_t)last;
    }

    kernel_heap.end_addr = new_end;
    kernel_heap.total_size = new_end - kernel_heap.start_addr;
}

extern int heap_map_page(uint64_t virt_addr);

static int heap_reserve_virtual(uint64_t virt_start, uint64_t size) {
#if HEAP_USE_DEMAND_PAGING
    return demand_reserve_range(virt_start, size);
#else
    return 0;
#endif
}

static int heap_expand(uint64_t new_end) {
    uint64_t addr;

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
    return 0;
}

void heap_init(void) {
    uint64_t total_kb;
    uint64_t heap_max;
    
    total_kb = pfa_get_total_ram_kb();
    heap_max = (total_kb / 4) * 1024;
    if (heap_max < HEAP_MAX_SIZE_DEFAULT) heap_max = HEAP_MAX_SIZE_DEFAULT;
    if (heap_max > HEAP_MAX_SIZE_CAP) heap_max = HEAP_MAX_SIZE_CAP;
    heap_max = (heap_max + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    kernel_heap.start_addr = HEAP_START;
    kernel_heap.end_addr = HEAP_START;
    kernel_heap.max_addr = HEAP_START + heap_max;
    kernel_heap.free_list = NULL;
    kernel_heap.total_size = 0;
    kernel_heap.used_size = 0;

    demand_paging_init();

    #if HEAP_USE_DEMAND_PAGING
        if (heap_reserve_virtual(HEAP_START, HEAP_INITIAL_SIZE) < 0) {
            printf("heap_init: Failed to reserve initial virtual range\n");
        }
        
        kernel_heap.end_addr = HEAP_START;
        kernel_heap.total_size = 0;
        kernel_heap.free_list = NULL;
    #else
        heap_expand(HEAP_START + HEAP_INITIAL_SIZE);
        
        initial_block = (heap_block_t *)HEAP_START;
        initial_block->magic = HEAP_MAGIC;
        initial_block->size = kernel_heap.total_size - sizeof(heap_block_t);
        initial_block->alloc_size = 0;
        initial_block->flags = 0;
        initial_block->is_free = 1;
        initial_block->next = NULL;
        initial_block->prev = NULL;

        kernel_heap.free_list = initial_block;
    #endif

    slab_init();

    main_heap_initialized = 1;

    printf("Heap initialized: 0x%08X - 0x%08X (%u KB) [demand paging + slab]\n",
           kernel_heap.start_addr, kernel_heap.end_addr,
           kernel_heap.total_size / 1024);
    printf("Early heap used: %u / %u bytes\n", early_heap_offset, EARLY_HEAP_SIZE);
}

static void *kmalloc_internal(size_t size) {
    size_t orig_size;
    size_t total_size;
    heap_block_t *block;
    heap_block_t *initial_block;
    uint64_t old_end;
    uint64_t needed;
    uint64_t new_end;
    heap_block_t *adjacent;
    heap_block_t *iter;
    uint64_t iter_end;
    uint64_t added;
    heap_block_t *new_block;
    uint64_t new_block_size;
    heap_block_t *prev;
    heap_block_t *cur;
    void *ptr;

    orig_size = size;
    total_size = size + CANARY_OVERHEAD;
    total_size = (total_size + 7) & ~7;

    block = find_best_fit(total_size);

    if (!block) {
        old_end = kernel_heap.end_addr;
        
        if (!kernel_heap.free_list) {
            if (old_end == HEAP_START) {
                if (heap_expand(HEAP_START + HEAP_INITIAL_SIZE) < 0) {
                    printf("kmalloc: Initial heap_expand failed\n");
                    return NULL;
                }
                old_end = kernel_heap.end_addr;
                
                initial_block = (heap_block_t *)HEAP_START;
                initial_block->magic = HEAP_MAGIC;
                initial_block->size = kernel_heap.total_size - sizeof(heap_block_t);
                initial_block->alloc_size = 0;
                initial_block->flags = 0;
                initial_block->is_free = 1;
                initial_block->next = NULL;
                initial_block->prev = NULL;
                kernel_heap.free_list = initial_block;
                
                block = find_best_fit(total_size);
                if (block) goto alloc_found;
            }
        }
        
        needed = total_size + sizeof(heap_block_t) + PAGE_SIZE;
        new_end = old_end + needed;

        DEBUG_MEMORY("kmalloc: expanding to new_end=0x%08X\n", new_end);
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
            iter_end = (uint64_t)iter + sizeof(heap_block_t) + iter->size;
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
            new_block->magic = HEAP_MAGIC;
            new_block->size = new_block_size;
            new_block->alloc_size = 0;
            new_block->flags = 0;
            new_block->is_free = 1;
            new_block->next = NULL;
            new_block->prev = NULL;

            prev = NULL;
            cur = kernel_heap.free_list;
            while (cur && (uint64_t)cur < (uint64_t)new_block) {
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

alloc_found:

    split_block(block, total_size);

    block->is_free = 0;
    block->alloc_size = orig_size;
    block->flags = 0;
    kernel_heap.used_size += block->size + sizeof(heap_block_t);

    set_canaries(block);
    
    ptr = get_user_ptr(block);

    if (((uintptr_t)ptr & 0x3) != 0) {
        printf("kmalloc: WARNING - returned pointer not 4-byte aligned: 0x%08X\n", (uint64_t)ptr);
        heap_verify();
    }

    return ptr;
}

void *kmalloc(size_t size) {
    void *result;
    size_t max_slab;
    if (size == 0) return NULL;
    if (!main_heap_initialized) return early_kmalloc(size);
    if (size > SIZE_MAX - CANARY_OVERHEAD - 7) return NULL;
    max_slab = slab_max_size();
    if (size <= max_slab) {
        result = slab_alloc(size);
        if (result) return result;
    }
    heap_lock_acquire();
    result = kmalloc_internal(size);
    heap_lock_release();
    return result;
}

void *ksafe_alloc(size_t size, uint64_t flags) {
    void *ptr;
    heap_block_t *block;
    
    if (size == 0) return NULL;
    
    if (size > SIZE_MAX - CANARY_OVERHEAD - 7) {
        printf("ksafe_alloc: size overflow detected\n");
        return NULL;
    }
    
    ptr = kmalloc(size);
    if (!ptr) return NULL;

    if (is_early_heap_ptr(ptr)) {
        return ptr;
    }
    
    block = get_block_from_ptr(ptr);
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
    size_t total;
    
    if (nmemb == 0 || size == 0) return NULL;
    
    if (nmemb > SIZE_MAX / size) {
        printf("kcalloc: integer overflow detected (%u * %u)\n", 
               (uint64_t)nmemb, (uint64_t)size);
        return NULL;
    }
    
    total = nmemb * size;
    return ksafe_alloc(total, KMALLOC_ZERO);
}

void *kmalloc_aligned(size_t size, uint64_t alignment) {
    size_t alloc_size;
    void *ptr;
    uintptr_t addr;
    uintptr_t aligned;
    
    if (size == 0) return NULL;
    if (alignment == 0) alignment = 1;

    if (size > SIZE_MAX - alignment - sizeof(void *)) {
        printf("kmalloc_aligned: size overflow\n");
        return NULL;
    }

    alloc_size = size + alignment + sizeof(void *);
    ptr = kmalloc(alloc_size);
    if (!ptr) return NULL;

    addr = (uintptr_t)ptr + sizeof(void *);
    aligned = (addr + alignment - 1) & ~(alignment - 1);

    ((void **)aligned)[-1] = ptr;

    return (void *)aligned;
}

static void kfree_internal(void *ptr) {
    heap_block_t *block;

    block = get_block_from_ptr(ptr);

    if (block->magic != HEAP_MAGIC) {
        printf("kfree: Invalid pointer 0x%08X (bad magic 0x%08X)\n", (uint64_t)ptr, block->magic);
        heap_verify();
        return;
    }

    if (block->is_free) {
        printf("kfree: Double free detected at 0x%08X\n", (uint64_t)ptr);
        return;
    }
    
    if (heap_check_canaries(ptr) != 0) {
        printf("kfree: Memory corruption detected for ptr 0x%08X - refusing to free\n", (uint64_t)ptr);
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
    heap_trim();
}

void kfree(void *ptr) {
    if (!ptr) return;
    if (is_early_heap_ptr(ptr)) return;
    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }
    heap_lock_acquire();
    kfree_internal(ptr);
    heap_lock_release();
}

void ksafe_free(void *ptr) {
    kfree(ptr); 
}

void kfree_secure(void *ptr) {
    heap_block_t *block;
    
    if (!ptr) return;

    if (is_early_heap_ptr(ptr)) return;
    
    heap_lock_acquire();

    block = get_block_from_ptr(ptr);
    
    if (block->magic != HEAP_MAGIC) {
        printf("kfree_secure: Invalid pointer 0x%08X\n", (uint64_t)ptr);
        heap_lock_release();
        return;
    }
    
    if (block->is_free) {
        printf("kfree_secure: Double free detected at 0x%08X\n", (uint64_t)ptr);
        heap_lock_release();
        return;
    }
    
    if (heap_check_canaries(ptr) != 0) {
        printf("kfree_secure: Memory corruption at 0x%08X\n", (uint64_t)ptr);
        heap_lock_release();
        return;
    }
    
    memset(ptr, 0, block->alloc_size);
    
    block->is_free = 1;
    kernel_heap.used_size -= block->size + sizeof(heap_block_t);
    
    coalesce_free_blocks(block);
    heap_trim();
    heap_lock_release();
}

void *krealloc(void *ptr, size_t new_size) {
    heap_block_t *block;
    void *new_ptr;
    size_t old_size;
    
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    if (is_early_heap_ptr(ptr)) {
        new_ptr = kmalloc(new_size);
        return new_ptr;
    }

    if (slab_owns(ptr)) {
        old_size = slab_alloc_size(ptr);
        if (new_size <= old_size)
            return ptr;
        new_ptr = kmalloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, old_size);
        slab_free(ptr);
        return new_ptr;
    }

    heap_lock_acquire();

    block = get_block_from_ptr(ptr);

    if (block->magic != HEAP_MAGIC) {
        printf("krealloc: Invalid pointer %p\n", ptr);
        heap_lock_release();
        return NULL;
    }
    
    if (heap_check_canaries(ptr) != 0) {
        printf("krealloc: Memory corruption detected, refusing to reallocate\n");
        heap_lock_release();
        return NULL;
    }

    if (block->alloc_size >= new_size) {
        block->alloc_size = new_size;
        set_canaries(block);
        heap_lock_release();
        return ptr;
    }

    old_size = block->alloc_size;
    heap_lock_release();

    new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);

    return new_ptr;
}

void heap_dump(void) {
    heap_block_t *block;
    uint64_t count;
    
    printf("Start: 0x%08X  End: 0x%08X  Max: 0x%08X\n",
           kernel_heap.start_addr, kernel_heap.end_addr, kernel_heap.max_addr);
    printf("Total: %u KB  Used: %u KB  Free: %u KB\n",
           kernel_heap.total_size / 1024,
           kernel_heap.used_size / 1024,
           (kernel_heap.total_size - kernel_heap.used_size) / 1024);

    block = kernel_heap.free_list;
    count = 0;
    while (block && count < 20) {
        printf("Block %u: addr=0x%08X size=%u %s\n",
               count, (uint64_t)block, block->size,
               block->is_free ? "FREE" : "USED");
        block = block->next;
        count++;
    }
}

uint64_t heap_free_space(void) {
    uint64_t free;
    heap_block_t *block;
    
    free = 0;
    block = kernel_heap.free_list;
    while (block) {
        if (block->is_free) {
            free += block->size;
        }
        block = block->next;
    }
    return free;
}

uint64_t heap_block_size_for_ptr(void *ptr) {
    heap_block_t *block;
    
    if (!ptr) return 0;
    if (is_early_heap_ptr(ptr)) return 0;
    block = get_block_from_ptr(ptr);
    if (block->magic != HEAP_MAGIC) return 0;
    return block->alloc_size; 
}

static void dump_memory_around(uint64_t addr, uint64_t radius) {
    uint64_t start;
    uint64_t end;
    uint64_t a;
    
    start = (addr >= radius) ? (addr - radius) : 0;
    end = addr + radius;
    if (start < kernel_heap.start_addr) start = kernel_heap.start_addr;
    if (end > kernel_heap.end_addr) end = kernel_heap.end_addr;

    printf(" dump mem around 0x%08X (0x%08X - 0x%08X):\n", addr, start, end);
    for (a = start; a < end; a += 4) {
        printf("  0x%08X: 0x%08X\n", a, *(volatile uint64_t *)a);
    }
}

void heap_verify(void) {
    heap_block_t *block;
    int i;
    int corruption_found;
    uint64_t baddr;
    uint64_t block_end;
    uint64_t naddr;
    uint64_t *head_canary;
    uint64_t *tail_canary;
    
    block = kernel_heap.free_list;
    i = 0;
    corruption_found = 0;
    
    while (block) {
        baddr = (uint64_t)block;
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

        block_end = baddr + sizeof(heap_block_t) + block->size;
        if (block_end > kernel_heap.end_addr) {
            printf("heap_verify: ERROR - block extends past heap end: block_end=0x%08X heap_end=0x%08X\n", block_end, kernel_heap.end_addr);
            dump_memory_around(baddr, 64);
            break;
        }
        
        if (!block->is_free && block->alloc_size > 0) {
            head_canary = get_head_canary(block);
            tail_canary = get_tail_canary(block);
            
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
            naddr = (uint64_t)block->next;
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

#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern char _kernel_end[];

mem_region_t memory_map[MAX_REGIONS];
uint32_t num_regions = 0;
uint64_t bump_current = 0;
static uint32_t active_region = 0;
uint64_t low_bump = 0;
static uint32_t kernel_reserved_frames = 0;

heap_t kernel_heap;

void init_mem_map(uint32_t mb_magic, uint32_t mb_ptr) {
    if (mb_magic != 0x2BADB002 || mb_ptr == 0) {
        printf("Bad multiboot - skip map.\n");
        return;
    }

    multiboot_t *mb = (multiboot_t *) (mb_ptr + 0xC0000000);
    DPRINTF("MB flags: 0x"); if (debugMode) print_hex(mb->flags); DPRINTF("\n");

    if (!(mb->flags & (1 << 6))) {
        printf("No MEMINFO - skip map.\n");
        return;
    }

    DPRINTF("Mmap addr: 0x"); if (debugMode) print_hex((unsigned long)mb->mmap_addr); DPRINTF("\n");
    DPRINTF("Mmap virt: 0x"); if (debugMode) print_hex((unsigned long)(mb->mmap_addr + 0xC0000000)); DPRINTF("\n");
    DPRINTF("  Length: 0x"); if (debugMode) print_hex(mb->mmap_length); DPRINTF("\n");

    multiboot_memory_map_t *entry = (multiboot_memory_map_t *) (mb->mmap_addr + 0xC0000000);
    uint8_t *mmap_virt_start = (uint8_t *)entry;
    uint8_t *mmap_virt_end = mmap_virt_start + mb->mmap_length;

    DPRINTF("Raw map dump:\n");
    DPRINTF(" mmap_virt_start=0x"); if (debugMode) print_hex((unsigned long)mmap_virt_start); DPRINTF(" mmap_virt_end=0x"); if (debugMode) print_hex((unsigned long)mmap_virt_end); DPRINTF("\n");
    uint32_t entry_count = 0;
    while ((uint8_t *)entry < mmap_virt_end) {
        if (entry->size == 0) break;

        uint64_t base = ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;
        uint64_t len = ((uint64_t)entry->length_high << 32) | entry->length_low;

        DPRINTF("Entry %d: Addr 0x", entry_count); if (debugMode) print_hex((unsigned long)entry); DPRINTF(" size=0x"); if (debugMode) print_hex(entry->size); DPRINTF(" Type %d, Base 0x", entry->type); if (debugMode) print_hex((unsigned long)base); DPRINTF("\n");
        DPRINTF("  Len 0x"); if (debugMode) print_hex((unsigned long)len); DPRINTF("\n");

        if (entry->type == 1 && len > 0) {
            DPRINTF("  -> Usable ~%lu KB\n", (unsigned long)(len / 1024));
        }

            entry = (multiboot_memory_map_t *) ((uint8_t *)entry + entry->size + 4);
        entry_count++;
        if (entry_count > 50) break;
    }

    uint32_t merged_count = 0;
    entry = (multiboot_memory_map_t *) (mb->mmap_addr + 0xC0000000);
    mmap_virt_start = (uint8_t *)entry;
    mmap_virt_end = mmap_virt_start + mb->mmap_length;
    while ((uint8_t *)entry < mmap_virt_end) {
        if (entry->size == 0) break;

        uint64_t base = ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;
        uint64_t len = ((uint64_t)entry->length_high << 32) | entry->length_low;

        if (entry->type != 1 || len == 0) {
            entry = (multiboot_memory_map_t *) ((uint8_t *)entry + entry->size + 4);
            continue;
        }

        if (merged_count == 0 || memory_map[merged_count-1].base + memory_map[merged_count-1].length != base) {
            memory_map[merged_count].base = base;
            memory_map[merged_count].length = len;
            memory_map[merged_count].type = 1;
            merged_count++;
        } else {
            memory_map[merged_count-1].length += len;
        }
        entry = (multiboot_memory_map_t *) ((uint8_t *)entry + entry->size + 4);
    }
    num_regions = merged_count;

    DPRINTF("Merged: %d regions\n", num_regions);
    uint64_t total_mb = 0;
    for (uint32_t i = 0; i < num_regions; i++) {
        uint64_t end = memory_map[i].base + memory_map[i].length;
        DPRINTF(" [%d] 0x", i); if (debugMode) print_hex((unsigned long)memory_map[i].base); DPRINTF("\n");
        DPRINTF("  - 0x"); if (debugMode) print_hex((unsigned long)end); DPRINTF("\n");
        unsigned long mb = (unsigned long)(memory_map[i].length / 1024 / 1024);
        unsigned long kb = (unsigned long)(memory_map[i].length / 1024);
        DPRINTF("  (%lu MB / %lu KB)\n", mb, kb);
        total_mb += mb;
    }
    DPRINTF("Total usable: %lu MB\n", total_mb);

    if (num_regions > 0) {
        uint32_t kernel_end_phys = (uint32_t)_kernel_end - 0xC0000000;
        kernel_end_phys = (kernel_end_phys + 0xFFF) & ~0xFFF;

        DPRINTF("Kernel ends at phys 0x"); if (debugMode) print_hex(kernel_end_phys); DPRINTF("\n");

        bump_current = kernel_end_phys;
        active_region = 0;

        for (uint32_t i = 0; i < num_regions; i++) {
            uint64_t region_end = memory_map[i].base + memory_map[i].length;
            if (bump_current >= memory_map[i].base && bump_current < region_end) {
                active_region = i;
                break;
            } else if (bump_current < memory_map[i].base) {
                bump_current = memory_map[i].base;
                active_region = i;
                break;
            }
        }

        low_bump = 0;
        for (uint32_t i = 0; i < num_regions; i++) {
            if (memory_map[i].base < 0x00400000) {
                uint64_t end = memory_map[i].base + memory_map[i].length;
                if (end > 0x1000) {
                    low_bump = memory_map[i].base + 0x1000;
                    if (low_bump < kernel_end_phys) low_bump = kernel_end_phys;
                    break;
                }
            }
        }

        DPRINTF("PMM bump ready: Starting at 0x"); if (debugMode) print_hex((unsigned long)bump_current); DPRINTF("\n");
        DPRINTF("PMM low bump starting at 0x"); if (debugMode) print_hex((unsigned long)low_bump); DPRINTF("\n");
    }
}

uint8_t pfa_bitmap[BITMAP_BYTES];

static inline void set_bit(uint32_t bit_idx) {
    pfa_bitmap[bit_idx / 8] |= (1 << (bit_idx % 8));
}

static inline void clear_bit(uint32_t bit_idx) {
    pfa_bitmap[bit_idx / 8] &= ~(1 << (bit_idx % 8));
}

static inline bool test_bit(uint32_t bit_idx) {
    return pfa_bitmap[bit_idx / 8] & (1 << (bit_idx % 8));
}

static uint32_t find_free_frames(uint32_t num) {
    if (num == 0) return 0;
    for (uint32_t idx = kernel_reserved_frames; idx + num <= TOTAL_PAGES; idx++) {
        bool ok = true;
        for (uint32_t j = 0; j < num; j++) {
            if (test_bit(idx + j)) { ok = false; break; }
        }
        if (!ok) continue;
        for (uint32_t j = 0; j < num; j++) set_bit(idx + j);
        return idx * PAGE_SIZE;
    }
    return 0;
}

static uint32_t count_free_frames(void) {
    uint32_t count = 0;
    for (uint32_t idx = 0; idx < TOTAL_PAGES; idx++) {
        if (!test_bit(idx)) count++;
    }
    return count;
}

void pfa_init(void) {
    memset(pfa_bitmap, 0xFF, sizeof(pfa_bitmap));

    uint32_t kernel_end_phys = (uint32_t)_kernel_end - 0xC0000000;
    kernel_end_phys = (kernel_end_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t kernel_frames = kernel_end_phys / PAGE_SIZE;
    kernel_reserved_frames = kernel_frames;

    printf("PFA: Kernel ends at phys 0x%08X (%u frames reserved)\n", kernel_end_phys, kernel_frames);
    printf("PFA: Managing up to 0x%08X (%u frames, bitmap %u bytes)\n",
           (uint32_t)MAX_PHYSICAL_MEMORY, (uint32_t)TOTAL_PAGES, (uint32_t)BITMAP_BYTES);

    uint64_t total_free_frames = 0;
    for (uint32_t r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;

        uint64_t region_base = memory_map[r].base;
        uint64_t region_end = region_base + memory_map[r].length;

        if (region_base >= MAX_PHYSICAL_MEMORY) {
            printf("PFA: Region %u [0x%08X%08X-0x%08X%08X]: skipped (beyond managed range)\n",
                   r, (uint32_t)(region_base >> 32), (uint32_t)region_base,
                   (uint32_t)(region_end >> 32), (uint32_t)region_end);
            continue;
        }
        if (region_end > MAX_PHYSICAL_MEMORY) {
            region_end = MAX_PHYSICAL_MEMORY;
        }

        uint32_t start_frame = (uint32_t)(region_base / PAGE_SIZE);
        uint32_t end_frame = (uint32_t)((region_end + PAGE_SIZE - 1) / PAGE_SIZE);
        if (end_frame > TOTAL_PAGES) end_frame = TOTAL_PAGES;

        uint32_t region_free = 0;
        for (uint32_t f = start_frame; f < end_frame; f++) {
            if (f < kernel_reserved_frames) continue;
            clear_bit(f);
            total_free_frames++;
            region_free++;
        }
        if (region_free > 0) {
            printf("PFA: Region %u [0x%08X-0x%08X]: %u free frames\n", r, (uint32_t)memory_map[r].base, (uint32_t)(memory_map[r].base + memory_map[r].length), (uint32_t)region_free);
        } else {
            printf("PFA: Region %u skipped (low RAM protected)\n", r);
        }
    }
    uint64_t total_mb = (total_free_frames + 255ULL) / 256ULL;
    printf("PFA ready: %llu total free frames (~%llu MB)\n", total_free_frames, total_mb);

    uint32_t actual_free = count_free_frames();
    if (actual_free != total_free_frames) {
        printf("PFA WARNING: counted %u but bitmap shows %u free\n", total_free_frames, actual_free);
    }
}

uint32_t pfa_alloc(void) {
    uint32_t addr = find_free_frames(1);
    if (addr) {
        if (addr < 0x00400000) memset((void*)addr, 0, PAGE_SIZE);
    }
    return addr;
}

void pfa_free(uint32_t phys_addr) {
    if (phys_addr % PAGE_SIZE != 0) {
        printf("PFA free: Invalid addr 0x%08X\n", phys_addr);
        return;
    }
    uint32_t idx = phys_addr / PAGE_SIZE;
    if (idx < kernel_reserved_frames) {
        printf("PFA free: Attempt to free kernel frame 0x%08X (idx %u) ignored\n", phys_addr, idx);
        return;
    }
    if (test_bit(idx)) {
        clear_bit(idx);
        printf("PFA: Freed frame 0x%08X (idx %u)\n", phys_addr, idx);
    } else {
        printf("PFA free: Already free? 0x%08X\n", phys_addr);
    }
}

uint32_t pfa_alloc_contiguous(uint32_t num_frames) {
    if (num_frames == 0) return 0;
    return find_free_frames(num_frames);
}

void pfa_free_contiguous(uint32_t phys_addr, uint32_t num_frames) {
    if (phys_addr % PAGE_SIZE != 0 || num_frames == 0) return;
    uint32_t start_idx = phys_addr / PAGE_SIZE;
    for (uint32_t i = 0; i < num_frames; i++) {
        uint32_t idx = start_idx + i;
        if (idx >= TOTAL_PAGES) break;
        if (idx < kernel_reserved_frames) continue;
        if (test_bit(idx)) clear_bit(idx);
    }
}

uint32_t pfa_count_free(void) {
    return count_free_frames();
}

void *pmm_alloc_page(void) {
    if (num_regions == 0) return NULL;

    for (uint32_t i = active_region; i < num_regions; i++) {
        uint64_t region_end = memory_map[i].base + memory_map[i].length;

        if (bump_current < memory_map[i].base) {
            bump_current = memory_map[i].base + 0x1000;
        }

        bump_current = (bump_current + 0xFFF) & ~0xFFF; 

        if (bump_current + 4096 <= region_end) {
            active_region = i;
            uint64_t alloc_start = bump_current;
            uint64_t candidate = alloc_start;
            while (candidate + 4096 <= region_end) {
                uint32_t idx_alloc = (uint32_t)(candidate / PAGE_SIZE);
                if (idx_alloc >= TOTAL_PAGES) break;
                if (!test_bit(idx_alloc)) {
                    alloc_start = candidate;
                    break;
                }
                candidate += PAGE_SIZE;
            }
            if (alloc_start == bump_current && test_bit((uint32_t)(alloc_start / PAGE_SIZE))) {
                bump_current = (i + 1 < num_regions) ? memory_map[i+1].base : 0;
                continue;
            }
            bump_current = alloc_start + 4096;
            void *page = (void *)(uint32_t)alloc_start;
            uint32_t idx_alloc = (uint32_t)(alloc_start / PAGE_SIZE);
            if (idx_alloc < TOTAL_PAGES) {
                set_bit(idx_alloc);
            }
            if (alloc_start < 0x00400000) {
                memset(page, 0, 4096);
            }
            return page;
        }

        bump_current = (i + 1 < num_regions) ? memory_map[i+1].base : 0;
    }
    return NULL;
}

void *pmm_alloc_pages(uint32_t num) {
    if (num == 0) return NULL;
    uint32_t addr = find_free_frames(num);
    if (!addr) return NULL;
    return (void *)(uint32_t)addr;
}

void *pmm_alloc_low_page(void) {
    if (num_regions == 0) return NULL;
    if (low_bump == 0) return NULL;

    uint64_t try = (low_bump + 0xFFF) & ~0xFFF;
    for (uint32_t i = 0; i < num_regions; i++) {
        uint64_t rstart = memory_map[i].base;
        uint64_t rend = rstart + memory_map[i].length;
        if (try >= rstart && try + 4096 <= rend && try < 0x00400000) {
            low_bump = try + 4096;
            void *page = (void *)(uint32_t)try;
            uint32_t idx_alloc = (uint32_t)(try / PAGE_SIZE);
            if (idx_alloc < TOTAL_PAGES) set_bit(idx_alloc);
            memset(page, 0, 4096);
            return page;
        }
    }
    return NULL;
}

static void heap_map_page(uint32_t virt_addr) {
    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;

    uint32_t *pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) {
        void *pt_page = pmm_alloc_low_page();
        if (!pt_page) {
            printf("heap_map_page: Failed to alloc page table\n");
            return;
        }
        pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | 3;
    }

    uint32_t *pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));

    if (!(pt[pt_idx] & 1)) {
        void *phys_page = pmm_alloc_page();
        if (!phys_page) {
            printf("heap_map_page: Failed to alloc phys page\n");
            return;
        }
        pt[pt_idx] = ((uint32_t)phys_page & ~0xFFF) | 3;

        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
}

static void heap_expand(uint32_t new_end) {
    if (new_end > kernel_heap.max_addr) {
        new_end = kernel_heap.max_addr;
    }

    new_end = (new_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t addr = kernel_heap.end_addr; addr < new_end; addr += PAGE_SIZE) {
        heap_map_page(addr);
    }

    kernel_heap.end_addr = new_end;
    kernel_heap.total_size = new_end - kernel_heap.start_addr;
}

void heap_init(void) {
    kernel_heap.start_addr = HEAP_START;
    kernel_heap.end_addr = HEAP_START;
    kernel_heap.max_addr = HEAP_START + HEAP_MAX_SIZE;
    kernel_heap.free_list = NULL;
    kernel_heap.total_size = 0;
    kernel_heap.used_size = 0;

    heap_expand(HEAP_START + HEAP_INITIAL_SIZE);

    heap_block_t *initial_block = (heap_block_t *)HEAP_START;
    initial_block->magic = HEAP_MAGIC;
    initial_block->size = kernel_heap.total_size - sizeof(heap_block_t);
    initial_block->is_free = 1;
    initial_block->next = NULL;
    initial_block->prev = NULL;

    kernel_heap.free_list = initial_block;

    printf("Heap initialized: 0x%08X - 0x%08X (%u KB)\n",
           kernel_heap.start_addr, kernel_heap.end_addr,
           kernel_heap.total_size / 1024);
}

static heap_block_t *find_best_fit(size_t size) {
    heap_block_t *best = NULL;
    heap_block_t *current = kernel_heap.free_list;

    while (current) {
        if (current->magic != HEAP_MAGIC) {
            printf("Heap corruption detected at 0x%08X\n", (uint32_t)current);
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
    size_t remaining = block->size - size - sizeof(heap_block_t);

    if (remaining >= HEAP_MIN_BLOCK + sizeof(heap_block_t)) {
        heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + sizeof(heap_block_t) + size);
        new_block->magic = HEAP_MAGIC;
        new_block->size = remaining;
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
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    if (block->prev && block->prev->is_free) {
        block->prev->size += sizeof(heap_block_t) + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    size = (size + 7) & ~7;

    heap_block_t *block = find_best_fit(size);

    if (!block) {
        uint32_t needed = size + sizeof(heap_block_t) + PAGE_SIZE;
        uint32_t new_end = kernel_heap.end_addr + needed;

        if (new_end > kernel_heap.max_addr) {
            printf("kmalloc: Heap exhausted\n");
            return NULL;
        }

        heap_expand(new_end);

        heap_block_t *new_block = (heap_block_t *)(kernel_heap.end_addr - needed + PAGE_SIZE);
        new_block->magic = HEAP_MAGIC;
        new_block->size = needed - sizeof(heap_block_t);
        new_block->is_free = 1;
        new_block->next = NULL;

        heap_block_t *last = kernel_heap.free_list;
        if (!last) {
            kernel_heap.free_list = new_block;
            new_block->prev = NULL;
        } else {
            while (last->next) last = last->next;
            last->next = new_block;
            new_block->prev = last;
        }

        block = find_best_fit(size);
        if (!block) {
            printf("kmalloc: Failed after expand\n");
            return NULL;
        }
    }

    split_block(block, size);

    block->is_free = 0;
    kernel_heap.used_size += block->size + sizeof(heap_block_t);

    return (void *)((uint8_t *)block + sizeof(heap_block_t));
}

void *kmalloc_aligned(size_t size, uint32_t alignment) {
    if (size == 0) return NULL;
    if (alignment == 0) alignment = 1;

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

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));

    if (block->magic != HEAP_MAGIC) {
        printf("kfree: Invalid pointer 0x%08X\n", (uint32_t)ptr);
        return;
    }

    if (block->is_free) {
        printf("kfree: Double free detected at 0x%08X\n", (uint32_t)ptr);
        return;
    }

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

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));

    if (block->magic != HEAP_MAGIC) {
        printf("krealloc: Invalid pointer\n");
        return NULL;
    }

    if (block->size >= new_size) {
        return ptr;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, block->size);
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
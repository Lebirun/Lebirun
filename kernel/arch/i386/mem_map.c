#include <kernel/mem_map.h>
#include <kernel/common.h> 
#include <string.h> 

extern char _kernel_end[];

mem_region_t memory_map[MAX_REGIONS];
uint32_t num_regions = 0;
uint64_t bump_current = 0;
static uint32_t active_region = 0;
uint64_t low_bump = 0;

void init_mem_map(uint32_t mb_magic, uint32_t mb_ptr) {
    if (mb_magic != 0x2BADB002 || mb_ptr == 0) {
        printf("Bad multiboot - skip map.\n");
        return;
    }

    multiboot_t *mb = (multiboot_t *) (mb_ptr + 0xC0000000);
    printf("MB flags: 0x"); print_hex(mb->flags); printf("\n");

    if (!(mb->flags & (1 << 6))) {
        printf("No MEMINFO - skip map.\n");
        return;
    }

    printf("Mmap addr: 0x"); print_hex((unsigned long)mb->mmap_addr); printf("\n");
    printf("Mmap virt: 0x"); print_hex((unsigned long)(mb->mmap_addr + 0xC0000000)); printf("\n");
    printf("  Length: 0x"); print_hex(mb->mmap_length); printf("\n");

    multiboot_memory_map_t *entry = (multiboot_memory_map_t *) (mb->mmap_addr + 0xC0000000);
    uint8_t *mmap_virt_start = (uint8_t *)entry;
    uint8_t *mmap_virt_end = mmap_virt_start + mb->mmap_length;

    printf("Raw map dump:\n");
    printf(" mmap_virt_start=0x"); print_hex((unsigned long)mmap_virt_start); printf(" mmap_virt_end=0x"); print_hex((unsigned long)mmap_virt_end); printf("\n");
    uint32_t entry_count = 0;
    while ((uint8_t *)entry < mmap_virt_end) {
        if (entry->size == 0) break;

        uint64_t base = ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;
        uint64_t len = ((uint64_t)entry->length_high << 32) | entry->length_low;

        printf("Entry %d: Addr 0x", entry_count); print_hex((unsigned long)entry); printf(" size=0x"); print_hex(entry->size); printf(" Type %d, Base 0x", entry->type); print_hex((unsigned long)base); printf("\n");
        printf("  Len 0x"); print_hex((unsigned long)len); printf("\n");

        if (entry->type == 1 && len > 0) {
            printf("  -> Usable ~%lu KB\n", (unsigned long)(len / 1024));
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

    printf("Merged: %d regions\n", num_regions);
    uint64_t total_mb = 0;
    for (uint32_t i = 0; i < num_regions; i++) {
        uint64_t end = memory_map[i].base + memory_map[i].length;
        printf(" [%d] 0x"); print_hex((unsigned long)memory_map[i].base); printf("\n");
        printf("  - 0x"); print_hex((unsigned long)end); printf("\n");
        unsigned long mb = (unsigned long)(memory_map[i].length / 1024 / 1024);
        unsigned long kb = (unsigned long)(memory_map[i].length / 1024);
        printf("  (%lu MB / %lu KB)\n", mb, kb);
        total_mb += mb;
    }
    printf("Total usable: %lu MB\n", total_mb);

    if (num_regions > 0) {
        uint32_t kernel_end_phys = (uint32_t)_kernel_end - 0xC0000000;
        kernel_end_phys = (kernel_end_phys + 0xFFF) & ~0xFFF;

        printf("Kernel ends at phys 0x"); print_hex(kernel_end_phys); printf("\n");

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

        printf("PMM bump ready: Starting at 0x"); print_hex((unsigned long)bump_current); printf("\n");
        printf("PMM low bump starting at 0x"); print_hex((unsigned long)low_bump); printf("\n");
    }
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
            bump_current += 4096;
            void *page = (void *)(uint32_t)alloc_start;
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
    void *first = NULL;
    for (uint32_t j = 0; j < num; j++) {
        void *page = pmm_alloc_page();
        if (!page) {
            return first; 
        }
        if (j == 0) first = page;
    }
    return first;
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
            memset(page, 0, 4096);
            return page;
        }
    }
    return NULL;
}

void *malloc_virt(size_t size) {
    if (size == 0) return NULL;
    uint32_t num_pages = (size + 4095) / 4096;
    void *phys_pages = pmm_alloc_pages(num_pages);
    if (!phys_pages) {
        printf("malloc_virt: pmm_alloc_pages failed\n");
        return NULL;
    }

    void *kernel_pt = pmm_alloc_low_page();
    if (!kernel_pt) {
        printf("malloc_virt: pmm_alloc_low_page failed\n");
        return phys_pages;
    }

    uint32_t *boot_pde = (uint32_t *)0xFFFFF000;
    boot_pde[769] = ((uint32_t)kernel_pt & ~0xFFF) | 3;

    uint32_t *pt = (uint32_t *)kernel_pt; 
    pt[0] = ((uint32_t)phys_pages & ~0xFFF) | 3;

    uint32_t cr3_phys = (uint32_t)(read_cr3() & ~0xFFF);
    __asm__ volatile ("movl %0, %%cr3\n\t"
                      "movl %0, %%cr3" : : "r" (cr3_phys));

    return (void *)0xC0400000;
}

void free_virt(void *ptr) {
    printf("Free: Unmapped 0x"); print_hex((unsigned long)ptr); printf("\n");
}
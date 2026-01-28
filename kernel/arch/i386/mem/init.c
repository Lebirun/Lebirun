#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;
extern char _kernel_end[];
extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));
extern uint32_t kernel_reserved_frames;
extern uint32_t total_pages_managed;

extern mem_region_t memory_map[MAX_REGIONS];
extern uint32_t num_regions;
extern uint64_t bump_current;
extern uint32_t active_region;
extern uint64_t low_bump;

reserved_region_t reserved_regions[MAX_RESERVED_REGIONS];
uint32_t num_reserved_regions = 0;

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);
extern void pfa_init_internal_setup(uint32_t bitmap_bytes, uint32_t total_pages, uint32_t kernel_frames);
extern void pfa_init_ram_stats(uint32_t total_kb, uint32_t usable_kb);
extern void pae_init_temp_mapping(void);
extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);

void init_mem_map(uint32_t mb_magic, uint32_t mb_ptr) {
    if (mb_magic != 0x2BADB002 || mb_ptr == 0) {
        printf("Bad multiboot - skip map.\n");
        return;
    }

    multiboot_t *mb = (multiboot_t *) (mb_ptr);
    DPRINTF4("MB flags: 0x"); DEBUG_HEX4(mb->flags); DPRINTF4("\n");

    if (!(mb->flags & (1 << 6))) {
        printf("No MEMINFO - skip map.\n");
        return;
    }

    DPRINTF4("Mmap addr: 0x"); DEBUG_HEX4((unsigned long)mb->mmap_addr); DPRINTF4("\n");
    DPRINTF4("Mmap virt: 0x"); DEBUG_HEX4((unsigned long)(mb->mmap_addr + 0xC0000000)); DPRINTF4("\n");
    DPRINTF4("  Length: 0x"); DEBUG_HEX4(mb->mmap_length); DPRINTF4("\n");

    multiboot_memory_map_t *entry = (multiboot_memory_map_t *) (mb->mmap_addr);
    uint8_t *mmap_virt_start = (uint8_t *)entry;
    uint8_t *mmap_virt_end = mmap_virt_start + mb->mmap_length;

    DPRINTF5("Raw map dump:\n");
    DPRINTF5(" mmap_virt_start=0x"); DEBUG_HEX5((unsigned long)mmap_virt_start); DPRINTF5(" mmap_virt_end=0x"); DEBUG_HEX5((unsigned long)mmap_virt_end); DPRINTF5("\n");
    uint32_t entry_count = 0;
    while ((uint8_t *)entry < mmap_virt_end) {
        if (entry->size == 0) break;

        uint64_t base = ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;
        uint64_t len = ((uint64_t)entry->length_high << 32) | entry->length_low;

        DPRINTF5("Entry %d: Addr 0x", entry_count); DEBUG_HEX5((unsigned long)entry); DPRINTF5(" size=0x"); DEBUG_HEX5(entry->size); DPRINTF5(" Type %d, Base 0x", entry->type); DEBUG_HEX5((unsigned long)base); DPRINTF5("\n");
        DPRINTF5("  Len 0x"); DEBUG_HEX5((unsigned long)len); DPRINTF5("\n");

        if (entry->type == 1 && len > 0) {
            DPRINTF5("  -> Usable ~%lu KB\n", (unsigned long)(len / 1024));
        }

            entry = (multiboot_memory_map_t *) ((uint8_t *)entry + entry->size + 4);
        entry_count++;
        if (entry_count > 50) break;
    }

    uint32_t merged_count = 0;
    entry = (multiboot_memory_map_t *) (mb->mmap_addr);
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

    DPRINTF3("Merged: %d regions\n", num_regions);
    uint64_t total_mb = 0;
    for (uint32_t i = 0; i < num_regions; i++) {
        uint64_t end = memory_map[i].base + memory_map[i].length;
        DPRINTF4(" [%d] 0x", i); DEBUG_HEX4((unsigned long)memory_map[i].base); DPRINTF4("\n");
        DPRINTF4("  - 0x"); DEBUG_HEX4((unsigned long)end); DPRINTF4("\n");
        unsigned long mb = (unsigned long)(memory_map[i].length / 1024 / 1024);
        unsigned long kb = (unsigned long)(memory_map[i].length / 1024);
        DPRINTF4("  (%lu MB / %lu KB)\n", mb, kb);
        total_mb += mb;
    }
    DPRINTF3("Total usable: %lu MB\n", total_mb);

    if (num_regions > 0) {
        uint32_t kernel_end_phys = (uint32_t)_kernel_end - 0xC0000000;
        kernel_end_phys = (kernel_end_phys + 0xFFF) & ~0xFFF;

        DPRINTF3("Kernel ends at phys 0x"); DEBUG_HEX3(kernel_end_phys); DPRINTF3("\n");

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

        DPRINTF3("PMM bump ready: Starting at 0x"); DEBUG_HEX3((unsigned long)bump_current); DPRINTF3("\n");
        DPRINTF3("PMM low bump starting at 0x"); DEBUG_HEX3((unsigned long)low_bump); DPRINTF3("\n");
    }

    num_reserved_regions = 0;
    if (mb->mods_count > 0 && mb->mods_addr) {
        uint32_t *mods_ptr = (uint32_t *)(mb->mods_addr + 0xC0000000);
        
        for (uint32_t i = 0; i < mb->mods_count && num_reserved_regions < MAX_RESERVED_REGIONS; i++) {
            uint32_t mod_start = mods_ptr[i * 4];    
            uint32_t mod_end = mods_ptr[i * 4 + 1];   
            
            uint32_t start_page = mod_start & ~0xFFF;
            uint32_t end_page = (mod_end + 0xFFF) & ~0xFFF;
            
            reserved_regions[num_reserved_regions].start_phys = start_page;
            reserved_regions[num_reserved_regions].end_phys = end_page;
            num_reserved_regions++;
            
            DPRINTF3("Reserved module %u: phys 0x%08X - 0x%08X\n", i, start_page, end_page);
        }
    }
}

void pfa_init(void) {
    uint32_t kernel_end_phys;
    uint32_t kernel_frames;
    uint64_t total_free_frames;
    uint32_t r;
    uint64_t region_base;
    uint64_t region_end;
    uint64_t region_start_capped;
    uint64_t region_end_capped;
    uint32_t start_frame;
    uint32_t end_frame;
    uint32_t region_free;
    uint32_t f;
    uint64_t max_phys;
    uint64_t total_mb;
    uint32_t actual_free;
    uint64_t region_size;

    if (pae_enabled) {
        pfa_init_internal_setup(BITMAP_BYTES_PAE, TOTAL_PAGES_PAE, 0);
        max_phys = MAX_PHYSICAL_MEMORY_PAE;
        printf("PFA: PAE enabled, managing up to 64GB physical memory\n");
    } else {
        pfa_init_internal_setup(BITMAP_BYTES_32BIT, TOTAL_PAGES_32BIT, 0);
        max_phys = MAX_PHYSICAL_MEMORY_32BIT;
        printf("PFA: Legacy 32-bit paging, managing up to 4GB physical memory\n");
    }

    extern uint8_t pfa_bitmap[];
    memset(pfa_bitmap, 0xFF, pae_enabled ? BITMAP_BYTES_PAE : BITMAP_BYTES_32BIT);

    kernel_end_phys = (uint32_t)_kernel_end - 0xC0000000;
    kernel_end_phys = (kernel_end_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kernel_frames = kernel_end_phys / PAGE_SIZE;

    printf("PFA: Kernel ends at phys 0x%08X (%u frames reserved)\n", kernel_end_phys, kernel_frames);
    printf("PFA: Managing up to 0x%08X%08X (%u frames, bitmap %u bytes)\n",
           (uint32_t)(max_phys >> 32), (uint32_t)max_phys, pae_enabled ? TOTAL_PAGES_PAE : TOTAL_PAGES_32BIT, 
           pae_enabled ? BITMAP_BYTES_PAE : BITMAP_BYTES_32BIT);

    total_free_frames = 0;
    for (r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;

        region_base = memory_map[r].base;
        region_end = region_base + memory_map[r].length;

        if (region_end <= max_phys) {
            region_start_capped = region_base;
            region_end_capped = region_end;
        } else if (region_base >= max_phys) {
            printf("PFA: Region %u [0x%08X%08X-0x%08X%08X]: skipped (beyond managed range)\n",
                   r, (uint32_t)(region_base >> 32), (uint32_t)region_base,
                   (uint32_t)(region_end >> 32), (uint32_t)region_end);
            continue;
        } else {
            region_start_capped = region_base;
            region_end_capped = max_phys;
            printf("PFA: Region %u [0x%08X%08X-0x%08X%08X]: capped to 0x%08X%08X\n",
                   r, (uint32_t)(region_base >> 32), (uint32_t)region_base,
                   (uint32_t)(region_end >> 32), (uint32_t)region_end,
                   (uint32_t)(max_phys >> 32), (uint32_t)max_phys);
        }

        start_frame = (uint32_t)(region_start_capped / PAGE_SIZE);
        end_frame = (uint32_t)((region_end_capped + PAGE_SIZE - 1) / PAGE_SIZE);
        region_free = 0;

        uint32_t total_pages = pae_enabled ? TOTAL_PAGES_PAE : TOTAL_PAGES_32BIT;
        if (end_frame > total_pages) end_frame = total_pages;

        extern void clear_bit(uint32_t bit_idx);
        for (f = start_frame; f < end_frame; f++) {
            if (f < kernel_frames) continue;
            clear_bit(f);
            total_free_frames++;
            region_free++;
        }
        if (region_free > 0) {
            printf("PFA: Region %u [0x%08X-0x%08X]: %u free frames\n", r, (uint32_t)region_start_capped, (uint32_t)region_end_capped, region_free);
        } else {
            printf("PFA: Region %u skipped (low RAM protected)\n", r);
        }
    }

    extern void set_bit(uint32_t bit_idx);
    extern bool test_bit(uint32_t bit_idx);
    for (r = 0; r < num_reserved_regions; r++) {
        uint32_t res_start_frame;
        uint32_t res_end_frame;
        uint32_t reserved_count;
        res_start_frame = reserved_regions[r].start_phys / PAGE_SIZE;
        res_end_frame = reserved_regions[r].end_phys / PAGE_SIZE;
        reserved_count = 0;
        for (f = res_start_frame; f < res_end_frame && f < total_pages_managed; f++) {
            if (!test_bit(f)) {
                set_bit(f);
                total_free_frames--;
                reserved_count++;
            }
        }
        printf("PFA: Reserved region %u [0x%08X-0x%08X]: %u frames marked as used\n",
               r, reserved_regions[r].start_phys, reserved_regions[r].end_phys, reserved_count);
    }

    total_mb = (total_free_frames + 255ULL) / 256ULL;
    printf("PFA ready: %llu total free frames (~%llu MB)\n", total_free_frames, total_mb);

    extern uint32_t count_free_frames(void);
    actual_free = count_free_frames();
    if (actual_free != total_free_frames) {
        printf("PFA WARNING: counted %u but bitmap shows %u free\n", (uint32_t)total_free_frames, actual_free);
    }
    
    uint32_t system_total_ram_kb = 0;
    for (r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;
        region_size = memory_map[r].length;
        if (memory_map[r].base + region_size > max_phys) {
            if (memory_map[r].base >= max_phys) continue;
            region_size = max_phys - memory_map[r].base;
        }
        system_total_ram_kb += (uint32_t)(region_size / 1024);
    }
    uint32_t system_usable_ram_kb = (uint32_t)(total_free_frames * 4);
    pfa_init_ram_stats(system_total_ram_kb, system_usable_ram_kb);
    printf("PFA: Total system RAM: %u KB (~%u MB), Free: %u KB\n", 
           system_total_ram_kb, system_total_ram_kb / 1024, system_usable_ram_kb);

    uint32_t cr3_val;
    uint32_t *pd;
    uint32_t temp_pd_idx;
    uint64_t try_addr;
    void *pt_page;
    uint32_t idx_alloc;
    uint32_t pt_addr;
    uint32_t *pt;
    uint32_t i_reg;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
    vmm_register_kernel_cr3(cr3_val);

    if (pae_enabled) {
        pae_init_temp_mapping();
        return;
    }

    pd = (uint32_t *)0xFFFFF000;
    temp_pd_idx = 0xF7000000 >> 22;
    if (!(pd[temp_pd_idx] & 1)) {
        try_addr = (low_bump + 0xFFF) & ~0xFFF;
        pt_page = NULL;
        for (i_reg = 0; i_reg < num_regions; i_reg++) {
            uint64_t rstart = memory_map[i_reg].base;
            uint64_t rend = rstart + memory_map[i_reg].length;
            if (try_addr >= rstart && try_addr + 4096 <= rend && try_addr < 0x00400000) {
                low_bump = try_addr + 4096;
                pt_page = (void *)(uint32_t)try_addr;
                idx_alloc = (uint32_t)(try_addr / PAGE_SIZE);
                if (idx_alloc < total_pages_managed) set_bit(idx_alloc);
                break;
            }
        }
        if (pt_page) {
            pd[temp_pd_idx] = ((uint32_t)pt_page & ~0xFFF) | 3;
            pt_addr = 0xFFC00000 + (temp_pd_idx << 12);
            __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
            pt = (uint32_t *)pt_addr;
            memset(pt, 0, PAGE_SIZE);
            idx_alloc = ((uint32_t)pt_page) / PAGE_SIZE;
            printf("PFA: Temp mapping PT ready (PDE[%u]=0x%08X) phys=0x%08X idx=%u\n", temp_pd_idx, pd[temp_pd_idx], (uint32_t)pt_page, idx_alloc);
        }
    }
}

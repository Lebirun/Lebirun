#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern char _kernel_end[];
extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt[] __attribute__((aligned(32)));

#define PAE_PDPT_POOL_SIZE 16

static uint64_t pae_pdpt_pool[PAE_PDPT_POOL_SIZE][4] __attribute__((aligned(32)));
static uint64_t pae_pd_pool[PAE_PDPT_POOL_SIZE][4][512] __attribute__((aligned(4096)));
static uint8_t pae_pdpt_pool_used[PAE_PDPT_POOL_SIZE];

static void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
static void temp_unmap_raw(uint32_t temp_virt);
static void pae_init_temp_mapping(void);

mem_region_t memory_map[MAX_REGIONS];
uint32_t num_regions = 0;
uint64_t bump_current = 0;
static uint32_t active_region = 0;
uint64_t low_bump = 0;
static uint32_t kernel_reserved_frames = 0;
static uint32_t kernel_pd_phys = 0;
static uint32_t system_total_ram_kb = 0;
static uint32_t system_usable_ram_kb = 0;

reserved_region_t reserved_regions[MAX_RESERVED_REGIONS];
uint32_t num_reserved_regions = 0;

static uint32_t kernel_pdpt_phys = 0;

#define TEMP_MAP_BASE 0xF7000000u
#define TEMP_MAP_PAGES 8u

static uint32_t pae_temp_pt_ready = 0;
static uint64_t *pae_temp_pt = NULL;
static uint32_t pae_temp_pd_idx = 0;

static struct {
    uint32_t v;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t old;
    uint32_t newval;
    uint32_t pd1023;
    uint32_t pd_pde;
    uint32_t op_count;
} map_debug = {0};

heap_t kernel_heap;

static uint64_t pae_heap_page_tables[16][512] __attribute__((aligned(4096)));
static uint32_t pae_heap_pt_count = 0;
static uint64_t pae_vmm_page_tables[32][512] __attribute__((aligned(4096)));
static uint32_t pae_vmm_pt_count = 0;

void pae_sync_kernel_mappings(void) {
    uint32_t slot;
    uint32_t j;
    uint64_t entry;
    
    if (!pae_enabled) return;
    
    for (slot = 0; slot < PAE_PDPT_POOL_SIZE; slot++) {
        if (!pae_pdpt_pool_used[slot]) continue;
        for (j = 0; j < 512; j++) {
            entry = boot_pd_high[j];
            if (entry & 1) {
                pae_pd_pool[slot][3][j] = entry & ~0x4ULL;
            } else {
                pae_pd_pool[slot][3][j] = 0;
            }
        }
    }
}

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

uint8_t pfa_bitmap[BITMAP_BYTES_MAX];
static uint32_t bitmap_bytes_used = BITMAP_BYTES_32BIT;
static uint32_t total_pages_managed = TOTAL_PAGES_32BIT;

static volatile int pfa_lock = 0;

static inline void pfa_lock_acquire(uint32_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&pfa_lock, 1)) {

    }
}

static inline void pfa_lock_release(uint32_t eflags) {
    __sync_lock_release(&pfa_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static inline void set_bit(uint32_t bit_idx) {
    if (bit_idx / 8 < bitmap_bytes_used)
        pfa_bitmap[bit_idx / 8] |= (1 << (bit_idx % 8));
}

static inline void clear_bit(uint32_t bit_idx) {
    if (bit_idx / 8 < bitmap_bytes_used)
        pfa_bitmap[bit_idx / 8] &= ~(1 << (bit_idx % 8));
}

static inline bool test_bit(uint32_t bit_idx) {
    if (bit_idx / 8 >= bitmap_bytes_used) return true;
    return pfa_bitmap[bit_idx / 8] & (1 << (bit_idx % 8));
}

static uint32_t find_free_frames(uint32_t num) {
    uint32_t idx;
    uint32_t j;
    uint32_t eflags;
    uint32_t result;
    bool ok;
    if (num == 0) return 0;
    
    pfa_lock_acquire(&eflags);
    
    for (idx = kernel_reserved_frames; idx + num <= total_pages_managed; idx++) {
        ok = true;
        for (j = 0; j < num; j++) {
            if (test_bit(idx + j)) { ok = false; break; }
        }
        if (!ok) continue;
        for (j = 0; j < num; j++) set_bit(idx + j);
        result = idx * PAGE_SIZE;
        pfa_lock_release(eflags);
        return result;
    }
    pfa_lock_release(eflags);
    return 0;
}

static uint32_t count_free_frames(void) {
    uint32_t count = 0;
    uint32_t idx;
    for (idx = 0; idx < total_pages_managed; idx++) {
        if (!test_bit(idx)) count++;
    }
    return count;
}

static uint32_t count_kernel_frames_in_usable(void) {
    uint32_t count = 0;
    uint32_t r;
    uint32_t f;
    uint64_t max_phys;
    max_phys = pae_enabled ? MAX_PHYSICAL_MEMORY_PAE : MAX_PHYSICAL_MEMORY_32BIT;
    for (r = 0; r < num_regions; r++) {
        uint64_t region_base;
        uint64_t region_end;
        uint32_t start_frame;
        uint32_t end_frame;
        if (memory_map[r].type != 1) continue;
        region_base = memory_map[r].base;
        region_end = region_base + memory_map[r].length;
        if (region_base >= max_phys) continue;
        if (region_end > max_phys) region_end = max_phys;
        start_frame = (uint32_t)(region_base / PAGE_SIZE);
        end_frame = (uint32_t)((region_end + PAGE_SIZE - 1) / PAGE_SIZE);
        if (end_frame > total_pages_managed) end_frame = total_pages_managed;
        for (f = start_frame; f < end_frame && f < kernel_reserved_frames; f++) {
            count++;
        }
    }
    return count;
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
        bitmap_bytes_used = BITMAP_BYTES_PAE;
        total_pages_managed = TOTAL_PAGES_PAE;
        max_phys = MAX_PHYSICAL_MEMORY_PAE;
        printf("PFA: PAE enabled, managing up to 64GB physical memory\n");
    } else {
        bitmap_bytes_used = BITMAP_BYTES_32BIT;
        total_pages_managed = TOTAL_PAGES_32BIT;
        max_phys = MAX_PHYSICAL_MEMORY_32BIT;
        printf("PFA: Legacy 32-bit paging, managing up to 4GB physical memory\n");
    }

    memset(pfa_bitmap, 0xFF, bitmap_bytes_used);

    kernel_end_phys = (uint32_t)_kernel_end - 0xC0000000;
    kernel_end_phys = (kernel_end_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kernel_frames = kernel_end_phys / PAGE_SIZE;
    kernel_reserved_frames = kernel_frames;

    printf("PFA: Kernel ends at phys 0x%08X (%u frames reserved)\n", kernel_end_phys, kernel_frames);
    printf("PFA: Managing up to 0x%08X%08X (%u frames, bitmap %u bytes)\n",
           (uint32_t)(max_phys >> 32), (uint32_t)max_phys, total_pages_managed, bitmap_bytes_used);

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

        if (end_frame > total_pages_managed) end_frame = total_pages_managed;

        for (f = start_frame; f < end_frame; f++) {
            if (f < kernel_reserved_frames) continue;
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

    actual_free = count_free_frames();
    if (actual_free != total_free_frames) {
        printf("PFA WARNING: counted %u but bitmap shows %u free\n", (uint32_t)total_free_frames, actual_free);
    }
    
    system_total_ram_kb = 0;
    for (r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;
        region_size = memory_map[r].length;
        if (memory_map[r].base + region_size > max_phys) {
            if (memory_map[r].base >= max_phys) continue;
            region_size = max_phys - memory_map[r].base;
        }
        system_total_ram_kb += (uint32_t)(region_size / 1024);
    }
    system_usable_ram_kb = (uint32_t)(total_free_frames * 4);
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

uint32_t pfa_alloc(void) {
    uint32_t addr = find_free_frames(1);
    if (addr) {
        if (addr < 0x00400000) pmm_zero_page_phys(addr);
    }
    return addr;
}

uint64_t pfa_alloc64(void) {
    uint32_t i;
    uint32_t byte_idx;
    uint32_t bit_idx;
    uint32_t max_bytes;
    uint32_t eflags;
    uint64_t result;

    if (!pae_enabled) {
        return (uint64_t)pfa_alloc();
    }

    pfa_lock_acquire(&eflags);
    
    max_bytes = bitmap_bytes_used;
    for (i = kernel_reserved_frames / 8; i < max_bytes; i++) {
        if (pfa_bitmap[i] != 0xFF) {
            byte_idx = i;
            for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                if (!(pfa_bitmap[byte_idx] & (1 << bit_idx))) {
                    uint64_t frame_idx = (uint64_t)byte_idx * 8 + bit_idx;
                    if (frame_idx < (uint64_t)total_pages_managed) {
                        pfa_bitmap[byte_idx] |= (1 << bit_idx);
                        result = frame_idx * PAGE_SIZE;
                        pfa_lock_release(eflags);
                        return result;
                    }
                }
            }
        }
    }
    pfa_lock_release(eflags);
    return 0;
}

void pfa_free64(uint64_t phys_addr) {
    uint64_t idx;
    uint32_t byte_idx;
    uint32_t bit_idx;
    uint32_t eflags;

    if (phys_addr % PAGE_SIZE != 0) {
        return;
    }
    idx = phys_addr / PAGE_SIZE;
    if (idx >= (uint64_t)total_pages_managed) {
        return;
    }
    if (idx < kernel_reserved_frames) {
        return;
    }
    byte_idx = (uint32_t)(idx / 8);
    bit_idx = (uint32_t)(idx % 8);
    
    pfa_lock_acquire(&eflags);
    pfa_bitmap[byte_idx] &= ~(1 << bit_idx);
    pfa_lock_release(eflags);
}

void pfa_free(uint32_t phys_addr) {
    uint32_t eflags;
    
    if (phys_addr % PAGE_SIZE != 0) {
        printf("PFA free: Invalid addr 0x%08X\n", phys_addr);
        return;
    }
    uint32_t idx = phys_addr / PAGE_SIZE;
    if (idx < kernel_reserved_frames) {
        printf("PFA free: Attempt to free kernel frame 0x%08X (idx %u) ignored\n", phys_addr, idx);
        return;
    }
    
    pfa_lock_acquire(&eflags);
    if (test_bit(idx)) {
        clear_bit(idx);
        pfa_lock_release(eflags);
        DPRINTF5("PFA: Freed frame 0x%08X (idx %u)\n", phys_addr, idx);
    } else {
        pfa_lock_release(eflags);
        printf("PFA free: Already free? 0x%08X\n", phys_addr);
    }
}

uint32_t pfa_alloc_contiguous(uint32_t num_frames) {
    if (num_frames == 0) return 0;
    return find_free_frames(num_frames);
}

void pfa_free_contiguous(uint32_t phys_addr, uint32_t num_frames) {
    uint32_t eflags;
    uint32_t i;
    uint32_t idx;
    uint32_t start_idx;
    
    if (phys_addr % PAGE_SIZE != 0 || num_frames == 0) return;
    start_idx = phys_addr / PAGE_SIZE;
    
    pfa_lock_acquire(&eflags);
    for (i = 0; i < num_frames; i++) {
        idx = start_idx + i;
        if (idx >= total_pages_managed) break;
        if (idx < kernel_reserved_frames) continue;
        if (test_bit(idx)) clear_bit(idx);
    }
    pfa_lock_release(eflags);
}

uint32_t pfa_count_free(void) {
    return count_free_frames();
}

uint32_t pfa_get_total_ram_kb(void) {
    return system_total_ram_kb;
}

uint32_t pfa_get_usable_ram_kb(void) {
    return system_usable_ram_kb;
}

uint32_t pfa_get_kernel_used_kb(void) {
    return count_kernel_frames_in_usable() * 4;
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
                if (idx_alloc >= total_pages_managed) break;
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
            if (idx_alloc < total_pages_managed) {
                set_bit(idx_alloc);
            }
            DPRINTF5("pmm_alloc_page: alloc page phys=0x%08X idx=%u\n", (uint32_t)page, idx_alloc);
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

    while (try < 0x00400000) {
        int in_region = 0;
        for (uint32_t i = 0; i < num_regions; i++) {
            uint64_t rstart = memory_map[i].base;
            uint64_t rend = rstart + memory_map[i].length;
            if (try >= rstart && try + PAGE_SIZE <= rend && try < 0x00400000) {
                in_region = 1;
                uint32_t idx_alloc = (uint32_t)(try / PAGE_SIZE);
                if (idx_alloc < total_pages_managed && !test_bit(idx_alloc)) {
                    low_bump = try + PAGE_SIZE;
                    set_bit(idx_alloc);
                    void *page = (void *)(uint32_t)try;
                    DPRINTF5("pmm_alloc_low_page: alloc page phys=0x%08X idx=%u\n", (uint32_t)page, idx_alloc);
                    pmm_zero_page_phys((uint32_t)page);
                    return page;
                } else {
                    try += PAGE_SIZE;
                    low_bump = try;
                    break;
                }
            }
        }

        if (!in_region) {
            uint64_t next_start = 0;
            for (uint32_t i = 0; i < num_regions; i++) {
                uint64_t rstart = memory_map[i].base;
                if (rstart > try && rstart < 0x00400000) {
                    if (next_start == 0 || rstart < next_start) next_start = rstart;
                }
            }
            if (next_start == 0) return NULL;
            try = next_start;
            low_bump = try;
        }
    }

    return NULL;
}

static void vmm_map_page_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    if (pdpt_idx == 3) {
        pd = boot_pd_high;
    } else {
        printf("vmm_map_page_pae: Low address mapping not supported yet (virt=0x%08X)\n", virt_addr);
        return;
    }

    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        if (pae_vmm_pt_count >= 32) {
            printf("vmm_map_page_pae: Out of VMM page tables\n");
            return;
        }
        pt_slot = pae_vmm_pt_count++;
        pt = pae_vmm_page_tables[pt_slot];
        memset(pt, 0, PAGE_SIZE);
        pt_phys = (uint32_t)((uintptr_t)pt - 0xC0000000);
        pd[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | (flags | 3);
        pae_sync_kernel_mappings();
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    } else {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        if ((flags & 0x4) && !(pd[pae_pd_idx] & 0x4)) {
            pd[pae_pd_idx] |= 0x4;
        }
    }

    pt[pae_pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt_new;
    uint32_t *pt;

    if (pae_enabled) {
        vmm_map_page_pae(virt_addr, phys_addr, flags);
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) {
        pt_page = pmm_alloc_low_page();
        if (!pt_page) {
            pt_page = pmm_alloc_page();
        }
        if (!pt_page) {
            printf("vmm_map_page: Failed to alloc page table\n");
            return;
        }
        pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | (flags & 0xFFF);
        pt_addr = 0xFFC00000 + (pd_idx << 12);
        __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
        pt_new = (uint32_t *)pt_addr;
        memset(pt_new, 0, PAGE_SIZE);
    } else {
        if ((flags & 0x4) && !(pd[pd_idx] & 0x4)) {
            pd[pd_idx] |= 0x4;
        }
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

static void vmm_map_page_early_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    if (pdpt_idx != 3) {
        return;
    }

    pd = boot_pd_high;
    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        return;
    }

    pt_phys = (uint32_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + 0xC0000000);
    pt[pae_pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

static void vmm_map_page_early(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    uint32_t *pt;

    if (pae_enabled) {
        vmm_map_page_early_pae(virt_addr, phys_addr, flags);
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) {
        return;
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_map_page_early_avail(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    vmm_map_page_early(virt_addr, phys_addr, flags);
}

static void vmm_map_range_alloc_pae(uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;
    uint32_t v;
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;
    void *phys_page;

    if (size == 0) return;
    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        pdpt_idx = (v >> 30) & 0x3;
        pae_pd_idx = (v >> 21) & 0x1FF;
        pae_pt_idx = (v >> 12) & 0x1FF;

        if (pdpt_idx != 3) {
            printf("vmm_map_range_alloc_pae: Low address not supported (v=0x%08X)\n", v);
            return;
        }

        pd = boot_pd_high;
        pde = pd[pae_pd_idx];
        if (!(pde & 1)) {
            if (pae_vmm_pt_count >= 32) {
                printf("vmm_map_range_alloc_pae: Out of VMM page tables\n");
                return;
            }
            pt_slot = pae_vmm_pt_count++;
            pt = pae_vmm_page_tables[pt_slot];
            memset(pt, 0, PAGE_SIZE);
            pt_phys = (uint32_t)((uintptr_t)pt - 0xC0000000);
            pd[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | (flags | 3);
            pae_sync_kernel_mappings();
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        } else {
            pt_phys = (uint32_t)(pde & ~0xFFFULL);
            pt = (uint64_t *)(pt_phys + 0xC0000000);
            if ((flags & 0x4) && !(pd[pae_pd_idx] & 0x4)) {
                pd[pae_pd_idx] |= 0x4;
            }
        }

        if (!(pt[pae_pt_idx] & 1)) {
            phys_page = pmm_alloc_page();
            if (!phys_page) {
                printf("vmm_map_range_alloc_pae: Failed to alloc phys page\n");
                return;
            }
            pt[pae_pt_idx] = ((uint64_t)(uint32_t)phys_page & ~0xFFFULL) | (flags & 0xFFF);
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        } else {
            pt[pae_pt_idx] = (pt[pae_pt_idx] & ~0xFFFULL) | (flags & 0xFFF);
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        }
    }
}

void vmm_map_range_alloc(uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;
    uint32_t v;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt_new;
    uint32_t *pt;
    void *phys_page;
    uint32_t old;
    uint32_t newval;
    uint32_t cr3;

    if (pae_enabled) {
        vmm_map_range_alloc_pae(virt_addr, size, flags);
        return;
    }

    if (size == 0) return;
    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        pd_idx = v >> 22;
        pt_idx = (v >> 12) & 0x3FF;
        pd = (uint32_t *)0xFFFFF000;

        if (!(pd[pd_idx] & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_range_alloc: Failed to alloc page table\n");
                return;
            }
            pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | (flags & 0xFFF);
            pt_addr = 0xFFC00000 + (pd_idx << 12);
            __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
            pt_new = (uint32_t *)pt_addr;
            memset(pt_new, 0, PAGE_SIZE);
        } else {
            if ((flags & 0x4) && !(pd[pd_idx] & 0x4)) {
                pd[pd_idx] |= 0x4;
                __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
                __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
            }
        }

        pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
        if (!(pt[pt_idx] & 1)) {
            phys_page = pmm_alloc_page();
            if (!phys_page) {
                printf("vmm_map_range_alloc: Failed to alloc phys page\n");
                return;
            }
            old = pt[pt_idx];
            newval = ((uint32_t)phys_page & ~0xFFF) | (flags & 0xFFF);
            pt[pt_idx] = newval;
            map_debug.v = v; map_debug.pd_idx = pd_idx; map_debug.pt_idx = pt_idx; map_debug.old = old; map_debug.newval = newval; map_debug.pd1023 = ((uint32_t *)0xFFFFF000)[1023]; map_debug.pd_pde = pd[pd_idx]; map_debug.op_count++;
                 if (debugMode && debugLevel >= 5) {
                  printf("vmm_map_range_alloc: map v=0x%08X pd_idx=%u pt_idx=%u -> phys=0x%08X old=0x%08X new=0x%08X (pd[1023]=0x%08X pd=%08X)\n",
                      v, pd_idx, pt_idx, (uint32_t)phys_page, old, newval, map_debug.pd1023, map_debug.pd_pde);
                 }
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
                 if (debugMode && debugLevel >= 5) heap_verify();
        } else {
            old = pt[pt_idx];
            newval = (old & ~0xFFF) | (flags & 0xFFF);
            pt[pt_idx] = newval;
            map_debug.v = v; map_debug.pd_idx = pd_idx; map_debug.pt_idx = pt_idx; map_debug.old = old; map_debug.newval = newval; map_debug.pd1023 = ((uint32_t *)0xFFFFF000)[1023]; map_debug.pd_pde = pd[pd_idx]; map_debug.op_count++;
                 if (debugMode && debugLevel >= 5) {
                  printf("vmm_map_range_alloc: remap v=0x%08X pd_idx=%u pt_idx=%u old=0x%08X new=0x%08X (pd[1023]=0x%08X pd=%08X)\n",
                      v, pd_idx, pt_idx, old, newval, map_debug.pd1023, map_debug.pd_pde);
                 }
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
                 if (debugMode && debugLevel >= 5) heap_verify();
        }
    }
}

static int heap_map_page_pae(uint32_t virt_addr) {
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint32_t pt_slot;
    uint64_t pde;
    uint64_t *pt;
    void *phys_page;
    uint32_t pt_phys;

    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    pde = boot_pd_high[pae_pd_idx];
    if (!(pde & 1)) {
        if (pae_heap_pt_count >= 16) {
            printf("heap_map_page_pae: Out of heap page tables\n");
            return -1;
        }
        pt_slot = pae_heap_pt_count++;
        pt = pae_heap_page_tables[pt_slot];
        memset(pt, 0, PAGE_SIZE);
        pt_phys = (uint32_t)((uintptr_t)pt - 0xC0000000);
        boot_pd_high[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
        pae_sync_kernel_mappings();
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    } else {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
    }

    if (!(pt[pae_pt_idx] & 1)) {
        phys_page = pmm_alloc_page();
        if (!phys_page) {
            printf("heap_map_page_pae: Failed to alloc phys page\n");
            return -1;
        }
        pt[pae_pt_idx] = ((uint64_t)(uint32_t)phys_page & ~0xFFFULL) | 3;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return 0;
}

static int heap_map_page(uint32_t virt_addr) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt_new;
    uint32_t *pt;
    void *phys_page;

    if (pae_enabled) {
        return heap_map_page_pae(virt_addr);
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    pd = (uint32_t *)0xFFFFF000;

    DPRINTF4("heap_map_page: virt=0x"); DEBUG_HEX4(virt_addr); DPRINTF4(" pd_idx=%u pt_idx=%u\n", pd_idx, pt_idx);

    if (!(pd[pd_idx] & 1)) {
        pt_page = pmm_alloc_low_page();
        if (!pt_page) {
            pt_page = pmm_alloc_page();
        }
        if (!pt_page) {
            printf("heap_map_page: Failed to alloc page table\n");
            return -1;
        }
        pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | 3;
        pt_addr = 0xFFC00000 + (pd_idx << 12);
        __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
        pt_new = (uint32_t *)pt_addr;
        memset(pt_new, 0, PAGE_SIZE);
        DPRINTF4("heap_map_page: Created PD entry for idx %u -> phys 0x", pd_idx); DEBUG_HEX4((uint32_t)pt_page); DPRINTF4("\n");
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));

    DPRINTF4("heap_map_page: PT pointer 0x"); DEBUG_HEX4((uint32_t)pt); DPRINTF4("\n");

    if (!(pt[pt_idx] & 1)) {
        phys_page = pmm_alloc_page();
        if (!phys_page) {
            printf("heap_map_page: Failed to alloc phys page\n");
            return -1;
        }
        pt[pt_idx] = ((uint32_t)phys_page & ~0xFFF) | 3;
        DPRINTF4("heap_map_page: Mapped phys 0x"); DEBUG_HEX4((uint32_t)phys_page); DPRINTF4(" at virt 0x"); DEBUG_HEX4(virt_addr); DPRINTF4("\n");

        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return 0;
}

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

static heap_block_t *find_best_fit(size_t size) {
    heap_block_t *best = NULL;
    heap_block_t *current = kernel_heap.free_list;

    while (current) {
        if (current->magic != HEAP_MAGIC) {
            printf("Heap corruption detected at 0x%08X\n", (uint32_t)current);
            printf("Last map op: v=0x%08X pd_idx=%u pt_idx=%u old=0x%08X new=0x%08X pd[1023]=0x%08X pd_pde=0x%08X op_count=%u\n",
                   map_debug.v, map_debug.pd_idx, map_debug.pt_idx, map_debug.old, map_debug.newval, map_debug.pd1023, map_debug.pd_pde, map_debug.op_count);
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

void vmm_debug_page(uint32_t virt_addr) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t pte;

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pae_pd_idx = (virt_addr >> 21) & 0x1FF;
        pae_pt_idx = (virt_addr >> 12) & 0x1FF;
        if (pdpt_idx != 3) {
            if (debugMode && debugLevel >= 5) printf("VMM debug 0x%08X: not in kernel space (PDPT[%u])\n", virt_addr, pdpt_idx);
            return;
        }
        pde = boot_pd_high[pae_pd_idx];
        if (debugMode && debugLevel >= 5) printf("VMM debug 0x%08X: PD[%u]=0x%016llX", virt_addr, pae_pd_idx, (unsigned long long)pde);
        if (pde & 1) {
            uint32_t pt_phys = (uint32_t)(pde & ~0xFFFULL);
            uint32_t temp_virt = 0xF7000000;
            temp_map_raw(temp_virt, pt_phys);
            uint64_t *pt = (uint64_t *)temp_virt;
            pte = pt[pae_pt_idx];
            temp_unmap_raw(temp_virt);
            if (debugMode && debugLevel >= 5) printf(" PT[%u]=0x%016llX", pae_pt_idx, (unsigned long long)pte);
            if (pte & 1) {
                if (debugMode && debugLevel >= 5) printf(" (P%s%s)", (pte & 2) ? "W" : "R", (pte & 4) ? "U" : "S");
            }
        }
        if (debugMode && debugLevel >= 5) printf("\n");
        return;
    }

    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;
    uint32_t *pd = (uint32_t *)0xFFFFF000;
    
    if (debugMode && debugLevel >= 5) printf("VMM debug 0x%08X: PD[%u]=0x%08X", virt_addr, pd_idx, pd[pd_idx]);
    if (pd[pd_idx] & 1) {
        uint32_t *pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
        if (debugMode && debugLevel >= 5) printf(" PT[%u]=0x%08X", pt_idx, pt[pt_idx]);
        if (pt[pt_idx] & 1) {
            if (debugMode && debugLevel >= 5) printf(" (P%s%s)", 
                   (pt[pt_idx] & 2) ? "W" : "R",
                   (pt[pt_idx] & 4) ? "U" : "S");
        }
    }
    if (debugMode && debugLevel >= 5) printf("\n");
}

static volatile int temp_map_lock = 0;

static inline void temp_lock_acquire(uint32_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&temp_map_lock, 1)) {
    }
}

static inline void temp_lock_release(uint32_t eflags) {
    __sync_lock_release(&temp_map_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static void pae_init_temp_mapping(void) {
    uint32_t pd_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;

    if (!pae_enabled) return;
    if (pae_temp_pt_ready) return;

    pd_idx = (TEMP_MAP_BASE >> 21) & 0x1FF;
    pd = boot_pd_high;
    pde = pd[pd_idx];

    if (pde & 1) {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        pae_temp_pt = pt;
        pae_temp_pd_idx = pd_idx;
        pae_temp_pt_ready = 1;
        return;
    }

    if (pae_vmm_pt_count >= 32) {
        printf("pae_init_temp_mapping: out of vmm page tables\n");
        return;
    }

    pt_slot = pae_vmm_pt_count++;
    pt = pae_vmm_page_tables[pt_slot];
    memset(pt, 0, PAGE_SIZE);
    pt_phys = (uint32_t)((uintptr_t)pt - 0xC0000000);
    pd[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
    pae_sync_kernel_mappings();

    pae_temp_pt = pt;
    pae_temp_pd_idx = pd_idx;
    pae_temp_pt_ready = 1;
}

static void temp_map_raw_pae(uint32_t temp_virt, uint64_t phys_addr) {
    uint32_t flags;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t *pt;

    temp_lock_acquire(&flags);

    pdpt_idx = (temp_virt >> 30) & 0x3;
    pd_idx = (temp_virt >> 21) & 0x1FF;
    pt_idx = (temp_virt >> 12) & 0x1FF;

    if (pdpt_idx != 3) {
        DPRINTF4("temp_map_raw_pae: only kernel space supported (pdpt_idx=%u)\n", pdpt_idx);
        temp_lock_release(flags);
        return;
    }

    if (!pae_temp_pt_ready) {
        pae_init_temp_mapping();
    }
    if (!pae_temp_pt_ready || pd_idx != pae_temp_pd_idx) {
        temp_lock_release(flags);
        return;
    }

    pt = pae_temp_pt;
    pt[pt_idx] = (phys_addr & ~0xFFFULL) | 3;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    temp_lock_release(flags);
}

static void temp_unmap_raw_pae(uint32_t temp_virt) {
    uint32_t flags;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t *pt;

    temp_lock_acquire(&flags);

    pdpt_idx = (temp_virt >> 30) & 0x3;
    pd_idx = (temp_virt >> 21) & 0x1FF;
    pt_idx = (temp_virt >> 12) & 0x1FF;

    if (pdpt_idx != 3) {
        temp_lock_release(flags);
        return;
    }

    if (!pae_temp_pt_ready) {
        pae_init_temp_mapping();
    }
    if (!pae_temp_pt_ready || pd_idx != pae_temp_pd_idx) {
        temp_lock_release(flags);
        return;
    }

    pt = pae_temp_pt;
    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    temp_lock_release(flags);
}

static void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr) {
    uint32_t orig_cr3;
    uint32_t flags;

    if (pae_enabled) {
        temp_map_raw_pae(temp_virt, (uint64_t)phys_addr);
        return;
    }

    temp_lock_acquire(&flags);

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    DPRINTF5("temp_map_raw: temp_virt=0x%08X phys=0x%08X orig_cr3=0x%08X kernel_pd=0x%08X\n", temp_virt, phys_addr, orig_cr3, kernel_pd_phys);
    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pd_phys) : "memory");
    }

    uint32_t pd_idx = temp_virt >> 22;
    uint32_t pt_idx = (temp_virt >> 12) & 0x3FF;
    uint32_t pt_addr = 0xFFC00000 + (pd_idx << 12);
    uint32_t pd_entry = ((uint32_t *)0xFFFFF000)[pd_idx];
    if (!(pd_entry & 1)) {
        DPRINTF4("temp_map_raw: WARNING - PD entry not present for temp mapping (pd_idx=%u)\n", pd_idx);
    }

    uint32_t *pt = (uint32_t *)pt_addr;
    pt[pt_idx] = (phys_addr & ~0xFFF) | 3;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    temp_lock_release(flags);
}

static void temp_unmap_raw(uint32_t temp_virt) {
    uint32_t orig_cr3;
    uint32_t flags;

    if (pae_enabled) {
        temp_unmap_raw_pae(temp_virt);
        return;
    }

    temp_lock_acquire(&flags);

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pd_phys) : "memory");
    }

    uint32_t pd_idx = temp_virt >> 22;
    uint32_t pt_idx = (temp_virt >> 12) & 0x3FF;
    uint32_t pt_addr = 0xFFC00000 + (pd_idx << 12);
    uint32_t pd_entry = ((uint32_t *)0xFFFFF000)[pd_idx];
    DPRINTF5("temp_unmap_raw: temp_virt=0x%08X pd_idx=%u pt_idx=%u pt_addr=0x%08X pd_entry=0x%08X\n", temp_virt, pd_idx, pt_idx, pt_addr, pd_entry);

    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    temp_lock_release(flags);
}

void vmm_temp_map_raw(uint32_t temp_virt, uint32_t phys_addr) {
    temp_map_raw(temp_virt, phys_addr);
}

void vmm_temp_map_raw64(uint32_t temp_virt, uint64_t phys_addr) {
    if (pae_enabled) {
        temp_map_raw_pae(temp_virt, phys_addr);
    } else {
        temp_map_raw(temp_virt, (uint32_t)phys_addr);
    }
}

void vmm_temp_unmap_raw(uint32_t temp_virt) {
    temp_unmap_raw(temp_virt);
}

static uint32_t vmm_create_pae_structure(void) {
    uint32_t slot;
    uint32_t pdpt_phys;
    uint32_t pd_phys[4];
    uint64_t *pdpt;
    uint64_t *new_pd;
    uint32_t i;
    uint32_t j;

    for (slot = 0; slot < PAE_PDPT_POOL_SIZE; slot++) {
        if (!pae_pdpt_pool_used[slot]) break;
    }
    if (slot >= PAE_PDPT_POOL_SIZE) {
        printf("vmm_create_pae_structure: PDPT pool exhausted\n");
        return 0;
    }
    pae_pdpt_pool_used[slot] = 1;

    pdpt_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[slot] - 0xC0000000);
    if (debugMode && debugLevel >= 3) printf("vmm_create_pae: allocated slot %u pdpt_phys=0x%08X\n", slot, pdpt_phys);
    for (i = 0; i < 4; i++) {
        pd_phys[i] = (uint32_t)((uintptr_t)&pae_pd_pool[slot][i] - 0xC0000000);
    }

    pdpt = pae_pdpt_pool[slot];
    for (i = 0; i < 4; i++) {
        pdpt[i] = ((uint64_t)pd_phys[i] & ~0xFFFULL) | 1;
    }

    for (i = 0; i < 4; i++) {
        new_pd = pae_pd_pool[slot][i];
        memset(new_pd, 0, PAGE_SIZE);

        if (i == 3) {
            for (j = 0; j < 512; j++) {
                uint64_t entry = boot_pd_high[j];
                if (entry & 1) {
                    new_pd[j] = entry & ~0x4ULL;
                }
            }
        }
    }

    return pdpt_phys;
}

uint32_t vmm_create_page_directory(void) {
    void *pd_page;
    uint32_t pd_phys;
    uint32_t temp_virt;
    uint32_t *new_pd;
    uint32_t *cur_pd;
    uint32_t i;

    if (pae_enabled) {
        return vmm_create_pae_structure();
    }

    pd_page = pmm_alloc_low_page();
    if (!pd_page) {
        pd_page = pmm_alloc_page();
    }
    if (!pd_page) {
        printf("vmm_create_page_directory: Failed to allocate PD page\n");
        return 0;
    }
    pd_phys = (uint32_t)pd_page;
    pmm_zero_page_phys(pd_phys);

    temp_virt = 0xF7000000;
    temp_map_raw(temp_virt, pd_phys);

    new_pd = (uint32_t *)temp_virt;
    memset(new_pd, 0, PAGE_SIZE);

    cur_pd = (uint32_t *)0xFFFFF000;
    for (i = 768; i < 1023; i++) {
        new_pd[i] = cur_pd[i] & ~0x4;
    }

    new_pd[1023] = (pd_phys & ~0xFFF) | 3;

    temp_unmap_raw(temp_virt);

    return pd_phys;
}

uint32_t vmm_get_phys_in_pd(uint32_t pd_phys, uint32_t virt_addr) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t temp_pd_virt;
    uint32_t temp_pt_virt;
    uint32_t pd_entry;
    uint32_t pt_phys;
    uint32_t pt_entry;
    uint32_t *foreign_pd;
    uint32_t *foreign_pt;
    uint32_t pdpt_idx;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;
    uint64_t *pd64;
    uint64_t *pt64;
    uint32_t temp_pdpt_virt;
    uint32_t pool_slot;
    int use_direct_access;

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pd_idx = (virt_addr >> 21) & 0x1FF;
        pt_idx = (virt_addr >> 12) & 0x1FF;

        use_direct_access = 0;
        pool_slot = 0;
        for (pool_slot = 0; pool_slot < PAE_PDPT_POOL_SIZE; pool_slot++) {
            uint32_t pool_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[pool_slot] - 0xC0000000);
            if (pool_phys == pd_phys && pae_pdpt_pool_used[pool_slot]) {
                use_direct_access = 1;
                break;
            }
        }

        if (use_direct_access) {
            pdpt = pae_pdpt_pool[pool_slot];
            pdpte = pdpt[pdpt_idx];

            if (!(pdpte & 1)) {
                return 0;
            }

            pd64 = pae_pd_pool[pool_slot][pdpt_idx];
            pde = pd64[pd_idx];

            if (!(pde & 1)) {
                return 0;
            }

            temp_pt_virt = 0xF7002000;
            temp_map_raw(temp_pt_virt, (uint32_t)(pde & ~0xFFFULL));
            pt64 = (uint64_t *)temp_pt_virt;
            pte = pt64[pt_idx];
            temp_unmap_raw(temp_pt_virt);

            if (!(pte & 1)) {
                return 0;
            }

            return (uint32_t)(pte & ~0xFFFULL);
        }

        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)temp_pdpt_virt;
        pdpte = pdpt[pdpt_idx];
        temp_unmap_raw(temp_pdpt_virt);

        if (!(pdpte & 1)) {
            return 0;
        }

        temp_pd_virt = 0xF7001000;
        temp_map_raw(temp_pd_virt, (uint32_t)(pdpte & ~0xFFFULL));
        pd64 = (uint64_t *)temp_pd_virt;
        pde = pd64[pd_idx];
        temp_unmap_raw(temp_pd_virt);

        if (!(pde & 1)) {
            return 0;
        }

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, (uint32_t)(pde & ~0xFFFULL));
        pt64 = (uint64_t *)temp_pt_virt;
        pte = pt64[pt_idx];
        temp_unmap_raw(temp_pt_virt);

        if (!(pte & 1)) {
            return 0;
        }

        return (uint32_t)(pte & ~0xFFFULL);
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    foreign_pd = (uint32_t *)temp_pd_virt;
    pd_entry = foreign_pd[pd_idx];
    temp_unmap_raw(temp_pd_virt);

    if (!(pd_entry & 1)) {
        return 0;
    }

    pt_phys = pd_entry & ~0xFFF;
    temp_pt_virt = 0xF7001000;
    temp_map_raw(temp_pt_virt, pt_phys);
    foreign_pt = (uint32_t *)temp_pt_virt;
    pt_entry = foreign_pt[pt_idx];
    temp_unmap_raw(temp_pt_virt);

    if (!(pt_entry & 1)) {
        return 0;
    }

    return pt_entry & ~0xFFF;
}

void vmm_map_page_in_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t temp_pd_virt;
    uint32_t temp_pt_virt;
    uint32_t temp_pdpt_virt;
    uint32_t pd_entry;
    uint32_t pt_phys;
    uint32_t *foreign_pd;
    uint32_t *foreign_pt;
    uint32_t cur_cr3;
    void *pt_page;
    uint32_t pdpt_idx;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t *pd64;
    uint64_t pde;
    uint64_t *pt64;
    uint32_t pool_slot;
    int use_direct_access;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pd_idx = (virt_addr >> 21) & 0x1FF;
        pt_idx = (virt_addr >> 12) & 0x1FF;

        use_direct_access = 0;
        pool_slot = 0;
        for (pool_slot = 0; pool_slot < PAE_PDPT_POOL_SIZE; pool_slot++) {
            uint32_t pool_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[pool_slot] - 0xC0000000);
            if (pool_phys == pd_phys && pae_pdpt_pool_used[pool_slot]) {
                use_direct_access = 1;
                break;
            }
        }

        if (use_direct_access) {
            uint32_t pde_flags;

            pdpt = pae_pdpt_pool[pool_slot];
            pdpte = pdpt[pdpt_idx];

            if (!(pdpte & 1)) {
                printf("vmm_map_page_in_pd: PDPT entry not present\n");
                return;
            }

            pd64 = pae_pd_pool[pool_slot][pdpt_idx];
            pde = pd64[pd_idx];

            pde_flags = (flags & 0x7) | 1;

            if (!(pde & 1)) {
                pt_page = pmm_alloc_low_page();
                if (!pt_page) {
                    pt_page = pmm_alloc_page();
                }
                if (!pt_page) {
                    printf("vmm_map_page_in_pd: Failed to alloc PAE PT\n");
                    return;
                }
                pt_phys = (uint32_t)pt_page;
                pmm_zero_page_phys(pt_phys);
                pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_flags;
            } else {
                pt_phys = (uint32_t)(pde & ~0xFFFULL);
                if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                    pd64[pd_idx] |= (flags & 0x7);
                }
            }

            temp_pt_virt = 0xF7002000;
            temp_map_raw(temp_pt_virt, pt_phys);
            pt64 = (uint64_t *)temp_pt_virt;
            pt64[pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
            temp_unmap_raw(temp_pt_virt);

            __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
            if (cur_cr3 == pd_phys) {
                __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
            }
            return;
        }

        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)temp_pdpt_virt;
        pdpte = pdpt[pdpt_idx];
        temp_unmap_raw(temp_pdpt_virt);

        if (!(pdpte & 1)) {
            printf("vmm_map_page_in_pd: PDPT entry not present\n");
            return;
        }

        {
            uint32_t pde_flags_nd;

            pde_flags_nd = (flags & 0x7) | 1;

            temp_pd_virt = 0xF7001000;
            temp_map_raw(temp_pd_virt, (uint32_t)(pdpte & ~0xFFFULL));
            pd64 = (uint64_t *)temp_pd_virt;
            pde = pd64[pd_idx];

            if (!(pde & 1)) {
                pt_page = pmm_alloc_low_page();
                if (!pt_page) {
                    pt_page = pmm_alloc_page();
                }
                if (!pt_page) {
                    printf("vmm_map_page_in_pd: Failed to alloc PAE PT\n");
                    temp_unmap_raw(temp_pd_virt);
                    return;
                }
                pt_phys = (uint32_t)pt_page;
                pmm_zero_page_phys(pt_phys);
                pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_flags_nd;
            } else {
                pt_phys = (uint32_t)(pde & ~0xFFFULL);
                if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                    pd64[pd_idx] |= (flags & 0x7);
                }
            }
            temp_unmap_raw(temp_pd_virt);
        }

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, pt_phys);
        pt64 = (uint64_t *)temp_pt_virt;
        pt64[pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
        temp_unmap_raw(temp_pt_virt);

        __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == pd_phys) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    temp_pd_virt = 0xF7000000;
    DPRINTF5("vmm_map_page_in_pd: pd_phys=0x%08X virt=0x%08X phys=0x%08X flags=0x%X\n", pd_phys, virt_addr, phys_addr, flags);
    temp_map_raw(temp_pd_virt, pd_phys);
    foreign_pd = (uint32_t *)temp_pd_virt;
    pd_entry = foreign_pd[pd_idx];
    DPRINTF5("vmm_map_page_in_pd: foreign_pd[pd_idx]=0x%08X\n", pd_entry);

    if (!(pd_entry & 1)) {
            uint32_t pde_32_flags;

            DPRINTF5("vmm_map_page_in_pd: pd entry not present for idx %u - allocating PT\n", pd_idx);
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_page_in_pd: Failed to alloc PT\n");
                temp_unmap_raw(temp_pd_virt);
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pmm_zero_page_phys(pt_phys);
            pde_32_flags = (flags & 0x7) | 1;
            foreign_pd[pd_idx] = (pt_phys & ~0xFFF) | pde_32_flags;
            DPRINTF5("vmm_map_page_in_pd: allocated PT phys=0x%08X\n", pt_phys);
    } else {
            pt_phys = pd_entry & ~0xFFF;
            if ((flags & 0x7) & ~(foreign_pd[pd_idx] & 0x7)) {
                foreign_pd[pd_idx] |= (flags & 0x7);
            }
    }

    temp_unmap_raw(temp_pd_virt);

    temp_pt_virt = 0xF7001000;
    DPRINTF5("vmm_map_page_in_pd: mapping into PT phys=0x%08X pt_idx=%u\n", pt_phys, pt_idx);
    temp_map_raw(temp_pt_virt, pt_phys);
    foreign_pt = (uint32_t *)temp_pt_virt;

    if (!(foreign_pt[pt_idx] & 1)) {
        DPRINTF5("vmm_map_page_in_pd: PT entry not present - writing entry\n");
    }
    foreign_pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF);
    DPRINTF5("vmm_map_page_in_pd: wrote PT entry = 0x%08X\n", foreign_pt[pt_idx]);

    temp_unmap_raw(temp_pt_virt);

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == pd_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
}

void vmm_map_page_in_pd64(uint32_t pd_phys, uint32_t virt_addr, uint64_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t temp_pdpt_virt;
    uint32_t temp_pd_virt;
    uint32_t temp_pt_virt;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t *pd64;
    uint64_t pde;
    uint64_t *pt64;
    void *pt_page;
    uint32_t pt_phys;
    uint32_t cur_cr3;
    uint32_t pool_slot;
    int use_direct_access;

    if (!pae_enabled) {
        vmm_map_page_in_pd(pd_phys, virt_addr, (uint32_t)phys_addr, flags);
        return;
    }

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;

    use_direct_access = 0;
    pool_slot = 0;
    for (pool_slot = 0; pool_slot < PAE_PDPT_POOL_SIZE; pool_slot++) {
        uint32_t pool_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[pool_slot] - 0xC0000000);
        if (pool_phys == pd_phys && pae_pdpt_pool_used[pool_slot]) {
            use_direct_access = 1;
            break;
        }
    }

    if (use_direct_access) {
        uint32_t pde_64_flags;

        pdpt = pae_pdpt_pool[pool_slot];
        pdpte = pdpt[pdpt_idx];

        if (!(pdpte & 1)) {
            printf("vmm_map_page_in_pd64: PDPT entry not present\n");
            return;
        }

        pd64 = pae_pd_pool[pool_slot][pdpt_idx];
        pde = pd64[pd_idx];

        pde_64_flags = (flags & 0x7) | 1;

        if (!(pde & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_page_in_pd64: Failed to alloc PAE PT\n");
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pmm_zero_page_phys(pt_phys);
            pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_64_flags;
        } else {
            pt_phys = (uint32_t)(pde & ~0xFFFULL);
            if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                pd64[pd_idx] |= (flags & 0x7);
            }
        }

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, pt_phys);
        pt64 = (uint64_t *)temp_pt_virt;
        pt64[pt_idx] = (phys_addr & ~0xFFFULL) | (flags & 0xFFF);
        temp_unmap_raw(temp_pt_virt);

        __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == pd_phys) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
        return;
    }

    {
        uint32_t pde_64nd_flags;

        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)temp_pdpt_virt;
        pdpte = pdpt[pdpt_idx];
        temp_unmap_raw(temp_pdpt_virt);

        if (!(pdpte & 1)) {
            printf("vmm_map_page_in_pd64: PDPT entry not present\n");
            return;
        }

        pde_64nd_flags = (flags & 0x7) | 1;

        temp_pd_virt = 0xF7001000;
        temp_map_raw(temp_pd_virt, (uint32_t)(pdpte & ~0xFFFULL));
        pd64 = (uint64_t *)temp_pd_virt;
        pde = pd64[pd_idx];

        if (!(pde & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_page_in_pd64: Failed to alloc PAE PT\n");
                temp_unmap_raw(temp_pd_virt);
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pmm_zero_page_phys(pt_phys);
            pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_64nd_flags;
        } else {
            pt_phys = (uint32_t)(pde & ~0xFFFULL);
            if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                pd64[pd_idx] |= (flags & 0x7);
            }
        }
        temp_unmap_raw(temp_pd_virt);

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, pt_phys);
        pt64 = (uint64_t *)temp_pt_virt;
        pt64[pt_idx] = (phys_addr & ~0xFFFULL) | (flags & 0xFFF);
        temp_unmap_raw(temp_pt_virt);

        __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == pd_phys) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
    }
}

void vmm_map_range_in_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;
    uint32_t v;
    uint32_t phys_page;

    if (size == 0) return;

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pd: Failed to alloc phys page\n");
            return;
        }
        pmm_zero_page_phys(phys_page);
        vmm_map_page_in_pd(pd_phys, v, phys_page, flags);
    }
}

uint32_t* vmm_map_range_in_pd_tracked(uint32_t pd_phys, uint32_t virt_addr, uint32_t size, uint32_t flags, uint32_t *out_count) {
    uint32_t start;
    uint32_t end;
    uint32_t num_pages;
    uint32_t *pages;
    uint32_t idx;
    uint32_t v;
    uint32_t phys_page;
    uint32_t i;

    if (size == 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    num_pages = (end - start) / PAGE_SIZE;

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }
    
    pages = (uint32_t *)kmalloc(num_pages * sizeof(uint32_t));
    if (!pages) {
        printf("vmm_map_range_in_pd_tracked: kmalloc failed for %u pages (%u bytes)\n", num_pages, num_pages * 4);
        if (out_count) *out_count = 0;
        return NULL;
    }

    idx = 0;
    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pd_tracked: Failed to alloc phys page %u/%u (free=%u)\n", idx, num_pages, pfa_count_free());
            for (i = 0; i < idx; i++) {
                pfa_free(pages[i]);
            }
            kfree(pages);
            if (out_count) *out_count = 0;
            return NULL;
        }
        pmm_zero_page_phys(phys_page);
        pages[idx++] = phys_page;
        vmm_map_page_in_pd(pd_phys, v, phys_page, flags);
    }
    
    if (out_count) *out_count = num_pages;
    return pages;
}

static uint32_t get_page_phys_in_pd(uint32_t pd_phys, uint32_t virt_addr) {
    uint32_t result;

    result = vmm_get_phys_in_pd(pd_phys, virt_addr);
    return result;
}

void vmm_copy_to_pd(uint32_t pd_phys, uint32_t dest_virt, const void *src, uint32_t size) {
    const uint8_t *s = (const uint8_t *)src;
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t page_offset = (dest_virt + offset) & 0xFFF;
        uint32_t chunk = PAGE_SIZE - page_offset;
        if (chunk > size - offset) chunk = size - offset;

        uint32_t virt_page = (dest_virt + offset) & ~0xFFF;
        uint32_t page_phys = get_page_phys_in_pd(pd_phys, virt_page);
        if (page_phys == 0) {
            printf("vmm_copy_to_pd: page not mapped for 0x%08X\n", virt_page);
            return;
        }

        temp_map_raw(0xF7003000, page_phys);
        memcpy((void *)(0xF7003000 + page_offset), s + offset, chunk);
        temp_unmap_raw(0xF7003000);

        offset += chunk;
    }
}

void vmm_read_from_pd(uint32_t pd_phys, uint32_t src_virt, void *dest, uint32_t size) {
    uint8_t *d = (uint8_t *)dest;
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t page_offset = (src_virt + offset) & 0xFFF;
        uint32_t chunk = PAGE_SIZE - page_offset;
        if (chunk > size - offset) chunk = size - offset;

        uint32_t virt_page = (src_virt + offset) & ~0xFFF;
        uint32_t page_phys = get_page_phys_in_pd(pd_phys, virt_page);
        if (page_phys == 0) return;

        temp_map_raw(0xF7003000, page_phys);
        memcpy(d + offset, (void *)(0xF7003000 + page_offset), chunk);
        temp_unmap_raw(0xF7003000);

        offset += chunk;
    }
}

static void vmm_unmap_page_pae(uint32_t virt_addr) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (pdpt_idx != 3) {
        return;
    }

    pd = boot_pd_high;
    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        return;
    }

    pt_phys = (uint32_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + 0xC0000000);
    pt[pae_pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_unmap_page(uint32_t virt_addr) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    uint32_t *pt;

    if (pae_enabled) {
        vmm_unmap_page_pae(virt_addr);
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;
    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) return;

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_register_kernel_cr3(uint32_t pd_phys) {
    if (pae_enabled) {
        kernel_pdpt_phys = pd_phys & ~0x1Fu;
    } else {
        kernel_pd_phys = pd_phys & ~0xFFFu;
    }
}

uint32_t vmm_get_kernel_cr3(void) {
    if (pae_enabled) {
        return kernel_pdpt_phys;
    }
    return kernel_pd_phys;
}

void dump_map_debug(void) {
    printf("map_debug: v=0x%08X pd_idx=%u pt_idx=%u old=0x%08X new=0x%08X pd1023=0x%08X pd_pde=0x%08X op_count=%u\n",
           map_debug.v, map_debug.pd_idx, map_debug.pt_idx, map_debug.old, map_debug.newval, map_debug.pd1023, map_debug.pd_pde, map_debug.op_count);
}

void dump_pd_pt_for_virt(uint32_t virt_addr) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t pte;

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pae_pd_idx = (virt_addr >> 21) & 0x1FF;
        pae_pt_idx = (virt_addr >> 12) & 0x1FF;
        if (pdpt_idx != 3) {
            printf("dump_pd_pt_for_virt: virt=0x%08X not in kernel space\n", virt_addr);
            return;
        }
        pde = boot_pd_high[pae_pd_idx];
        if (!(pde & 1)) {
            printf("dump_pd_pt_for_virt: virt=0x%08X pd_idx=%u pd_entry=0x%016llX (not present)\n",
                   virt_addr, pae_pd_idx, (unsigned long long)pde);
            return;
        }
        uint32_t pt_phys = (uint32_t)(pde & ~0xFFFULL);
        uint32_t temp_virt = 0xF7000000;
        temp_map_raw(temp_virt, pt_phys);
        pte = ((uint64_t *)temp_virt)[pae_pt_idx];
        temp_unmap_raw(temp_virt);
        printf("dump_pd_pt_for_virt: virt=0x%08X pd_idx=%u pt_idx=%u pd_entry=0x%016llX pt_entry=0x%016llX\n",
               virt_addr, pae_pd_idx, pae_pt_idx, (unsigned long long)pde, (unsigned long long)pte);
        return;
    }

    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;
    uint32_t pd_entry = ((uint32_t *)0xFFFFF000)[pd_idx];
    uint32_t *pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    uint32_t pt_entry = pt[pt_idx];
    printf("dump_pd_pt_for_virt: virt=0x%08X pd_idx=%u pt_idx=%u pd_entry=0x%08X pt_entry=0x%08X\n", virt_addr, pd_idx, pt_idx, pd_entry, pt_entry);
}

void vmm_dump_for_pd(uint32_t pd_phys, uint32_t virt_addr) {
    if (pd_phys == 0) {
        return;
    }
    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;

    uint32_t temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    uint32_t *foreign_pd = (uint32_t *)temp_pd_virt;
    uint32_t pd_entry = foreign_pd[pd_idx];
    if (!(pd_entry & 1)) {
        temp_unmap_raw(temp_pd_virt);
        return;
    }
    uint32_t pt_phys = pd_entry & ~0xFFF;
    temp_unmap_raw(temp_pd_virt);

    uint32_t temp_pt_virt = 0xF7001000;
    temp_map_raw(temp_pt_virt, pt_phys);
    uint32_t *foreign_pt = (uint32_t *)temp_pt_virt;
    uint32_t pt_entry = foreign_pt[pt_idx];
    temp_unmap_raw(temp_pt_virt);

    printf("vmm_dump_for_pd: virt=0x%08X pd_entry=0x%08X pt_entry=0x%08X\n", virt_addr, pd_entry, pt_entry);
}

void pmm_zero_page_phys(uint32_t phys_addr) {
    uint32_t temp_virt = 0xF7006000;
    DPRINTF5("pmm_zero_page_phys: zeroing phys=0x%08X\n", phys_addr);
    
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    int had_if = (eflags & (1<<9)) != 0;
    if (had_if) __asm__ volatile ("cli");
    
    temp_map_raw(temp_virt, phys_addr);

    DPRINTF5("pmm_zero_page_phys: pre-zero first dword=0x%08X\n", *(volatile uint32_t *)temp_virt);

    volatile uint32_t *w = (volatile uint32_t *)temp_virt;
    for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) {
        w[i] = 0;
    }

    DPRINTF5("pmm_zero_page_phys: post-zero first dword=0x%08X\n", *(volatile uint32_t *)temp_virt);

    temp_unmap_raw(temp_virt);
    
    if (had_if) __asm__ volatile ("sti");
}

void vmm_set_cr3(uint32_t pd_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

uint32_t vmm_get_cr3(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline bool clone_should_log_detail(uint32_t index) {
    return index < 2 || (index & 0x3FF) == 0;
}

static inline bool clone_should_log_sample(uint32_t index) {
    return index == 0;
}

void vmm_free_page_directory(uint32_t pd_phys) {
    uint32_t temp_pd_virt;
    uint32_t temp_pdpt_virt;
    uint32_t temp_pt_virt;
    uint32_t *pd;
    uint64_t *pdpt;
    uint64_t *pd64;
    uint64_t *pt64;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    uint32_t slot;
    uint32_t pool_phys;
    uint64_t pde;
    uint32_t pt_phys_val;
    uint64_t pte;
    uint64_t pdpte;
    uint32_t pd_phys_entry;

    if (!pd_phys) return;

    if (pae_enabled) {
        for (slot = 0; slot < PAE_PDPT_POOL_SIZE; slot++) {
            pool_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[slot] - 0xC0000000);
            if (pool_phys == pd_phys) {
                if (pae_pdpt_pool_used[slot]) {
                    if (debugMode && debugLevel >= 3) printf("vmm_free_pd: freeing PAE pool slot %u pd=0x%08X\n", slot, pd_phys);
                    for (i = 0; i < 3; i++) {
                        pd64 = pae_pd_pool[slot][i];
                        for (j = 0; j < 512; j++) {
                            pde = pd64[j];
                            if (!(pde & 1)) continue;
                            pt_phys_val = (uint32_t)(pde & ~0xFFFULL);
                            temp_pt_virt = 0xF7002000;
                            temp_map_raw(temp_pt_virt, pt_phys_val);
                            pt64 = (uint64_t *)temp_pt_virt;
                            for (k = 0; k < 512; k++) {
                                pte = pt64[k];
                                if (pte & 1) {
                                    pfa_free64(pte & ~0xFFFULL);
                                }
                            }
                            temp_unmap_raw(temp_pt_virt);
                            pfa_free(pt_phys_val);
                        }
                        memset(pd64, 0, PAGE_SIZE);
                    }
                    pae_pdpt_pool_used[slot] = 0;
                    return;
                } else {
                    printf("vmm_free_pd: WARNING: PAE pool slot %u already free! pd=0x%08X\n", slot, pd_phys);
                    return;
                }
            }
        }

        printf("vmm_free_pd: PAE pd=0x%08X not in pool, treating as dynamic\n", pd_phys);
        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)temp_pdpt_virt;

        for (i = 0; i < 4; i++) {
            pdpte = pdpt[i];
            if (!(pdpte & 1)) continue;
            if (i >= 3) continue;

            pd_phys_entry = (uint32_t)(pdpte & ~0xFFFULL);
            temp_pd_virt = 0xF7001000;
            temp_map_raw(temp_pd_virt, pd_phys_entry);
            pd64 = (uint64_t *)temp_pd_virt;

            for (j = 0; j < 512; j++) {
                pde = pd64[j];
                if (!(pde & 1)) continue;

                pt_phys_val = (uint32_t)(pde & ~0xFFFULL);
                temp_pt_virt = 0xF7002000;
                temp_map_raw(temp_pt_virt, pt_phys_val);
                pt64 = (uint64_t *)temp_pt_virt;

                for (k = 0; k < 512; k++) {
                    pte = pt64[k];
                    if (pte & 1) {
                        pfa_free64(pte & ~0xFFFULL);
                    }
                }
                temp_unmap_raw(temp_pt_virt);
                pfa_free(pt_phys_val);
            }
            temp_unmap_raw(temp_pd_virt);
            pfa_free(pd_phys_entry);
        }
        temp_unmap_raw(temp_pdpt_virt);
        pfa_free(pd_phys);
        return;
    }

    temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    pd = (uint32_t *)temp_pd_virt;

    for (i = 0; i < 768; i++) {
        if (pd[i] & 1) {
            pt_phys_val = pd[i] & ~0xFFF;
            pfa_free(pt_phys_val);
        }
    }

    temp_unmap_raw(temp_pd_virt);

    pfa_free(pd_phys);
}

static uint32_t vmm_clone_pae_structure(uint32_t src_pdpt_phys, uint32_t **out_user_pages, uint32_t *out_user_pages_count) {
    uint32_t user_page_capacity;
    uint32_t user_page_count;
    uint32_t *user_pages;
    uint32_t eflags;
    uint32_t orig_cr3;
    uint32_t new_pdpt_phys;
    uint32_t new_pd_phys[4];
    uint32_t temp_src_pdpt;
    uint32_t temp_src_pd;
    uint32_t temp_src_pt;
    uint32_t temp_src_page;
    uint32_t temp_new_page;
    uint64_t *src_pdpt;
    uint64_t *new_pdpt;
    uint64_t *src_pd;
    uint64_t *new_pd;
    uint64_t *src_pt;
    uint64_t *new_pt;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    uint32_t slot;
    uint64_t src_pdpte;
    uint32_t src_pd_phys;
    uint64_t src_pde;
    uint32_t src_pt_phys;
    uint32_t pde_flags;
    void *new_pt_page;
    uint32_t new_pt_phys;
    uint32_t temp_new_pt;
    uint64_t src_pte;
    uint64_t src_page_phys;
    uint32_t pte_flags;
    uint64_t new_page_phys;
    uint64_t entry;
    uint64_t *cleanup_pd;
    uint32_t cleanup_pt_phys;

    user_page_capacity = 512;
    user_page_count = 0;

    user_pages = (uint32_t *)kmalloc(user_page_capacity * sizeof(uint32_t));
    if (!user_pages) {
        printf("vmm_clone_pae: Failed to allocate user_pages array\n");
        return 0;
    }

    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    if (kernel_pdpt_phys && orig_cr3 != kernel_pdpt_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pdpt_phys) : "memory");
    }

    for (slot = 0; slot < PAE_PDPT_POOL_SIZE; slot++) {
        if (!pae_pdpt_pool_used[slot]) break;
    }
    if (slot >= PAE_PDPT_POOL_SIZE) {
        kfree(user_pages);
        if (kernel_pdpt_phys && orig_cr3 != kernel_pdpt_phys) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        }
        if (eflags & (1 << 9)) __asm__ volatile ("sti");
        return 0;
    }
    pae_pdpt_pool_used[slot] = 1;

    new_pdpt_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[slot] - 0xC0000000);
    if (debugMode && debugLevel >= 3) printf("vmm_clone_pae: allocated slot %u pdpt_phys=0x%08X\n", slot, new_pdpt_phys);
    for (i = 0; i < 4; i++) {
        new_pd_phys[i] = (uint32_t)((uintptr_t)&pae_pd_pool[slot][i] - 0xC0000000);
    }

    new_pdpt = pae_pdpt_pool[slot];
    for (i = 0; i < 4; i++) {
        new_pdpt[i] = ((uint64_t)new_pd_phys[i] & ~0xFFFULL) | 1;
    }

    temp_src_pdpt = 0xF7000000;
    temp_src_pd = 0xF7002000;
    temp_src_pt = 0xF7004000;
    temp_src_page = 0xF7006000;
    temp_new_page = 0xF7007000;

    temp_map_raw(temp_src_pdpt, src_pdpt_phys);
    src_pdpt = (uint64_t *)temp_src_pdpt;

    for (i = 0; i < 3; i++) {
        src_pdpte = src_pdpt[i];
        new_pd = pae_pd_pool[slot][i];
        memset(new_pd, 0, PAGE_SIZE);

        if (!(src_pdpte & 1)) continue;

        src_pd_phys = (uint32_t)(src_pdpte & ~0xFFFULL);
        temp_map_raw(temp_src_pd, src_pd_phys);
        src_pd = (uint64_t *)temp_src_pd;

        for (j = 0; j < 512; j++) {
            src_pde = src_pd[j];
            if (!(src_pde & 1)) continue;

            src_pt_phys = (uint32_t)(src_pde & ~0xFFFULL);
            pde_flags = (uint32_t)(src_pde & 0xFFF);

            new_pt_page = pmm_alloc_page();
            if (!new_pt_page) {
                temp_unmap_raw(temp_src_pd);
                temp_unmap_raw(temp_src_pdpt);
                goto cleanup_fail_pae;
            }
            new_pt_phys = (uint32_t)new_pt_page;
            pmm_zero_page_phys(new_pt_phys);

            new_pd[j] = ((uint64_t)new_pt_phys & ~0xFFFULL) | pde_flags;

            temp_new_pt = 0xF7005000;
            temp_map_raw(temp_src_pt, src_pt_phys);
            src_pt = (uint64_t *)temp_src_pt;
            temp_map_raw(temp_new_pt, new_pt_phys);
            new_pt = (uint64_t *)temp_new_pt;

            for (k = 0; k < 512; k++) {
                src_pte = src_pt[k];
                if (!(src_pte & 1)) continue;

                src_page_phys = src_pte & ~0xFFFULL;
                pte_flags = (uint32_t)(src_pte & 0xFFF);

                new_page_phys = pfa_alloc64();
                if (!new_page_phys) {
                    temp_unmap_raw(temp_new_pt);
                    temp_unmap_raw(temp_src_pt);
                    temp_unmap_raw(temp_src_pd);
                    temp_unmap_raw(temp_src_pdpt);
                    goto cleanup_fail_pae;
                }

                temp_map_raw_pae(temp_src_page, src_page_phys);
                temp_map_raw_pae(temp_new_page, new_page_phys);
                memcpy((void *)temp_new_page, (void *)temp_src_page, PAGE_SIZE);
                temp_unmap_raw(temp_src_page);
                temp_unmap_raw(temp_new_page);

                new_pt[k] = (new_page_phys & ~0xFFFULL) | pte_flags;

                if (user_page_count >= user_page_capacity) {
                    temp_unmap_raw(temp_new_pt);
                    temp_unmap_raw(temp_src_pt);
                    temp_unmap_raw(temp_src_pd);
                    temp_unmap_raw(temp_src_pdpt);
                    goto cleanup_fail_pae;
                }
                user_pages[user_page_count++] = (uint32_t)new_page_phys;
            }
            temp_unmap_raw(temp_new_pt);
            temp_unmap_raw(temp_src_pt);
        }
        temp_unmap_raw(temp_src_pd);
    }

    new_pd = pae_pd_pool[slot][3];
    for (j = 0; j < 512; j++) {
        entry = boot_pd_high[j];
        if (entry & 1) {
            new_pd[j] = entry & ~0x4ULL;
        }
    }

    temp_unmap_raw(temp_src_pdpt);

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    if (kernel_pdpt_phys && orig_cr3 != kernel_pdpt_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return new_pdpt_phys;

cleanup_fail_pae:
    for (i = 0; i < user_page_count; i++) {
        pfa_free(user_pages[i]);
    }
    kfree(user_pages);

    for (i = 0; i < 3; i++) {
        cleanup_pd = pae_pd_pool[slot][i];
        for (j = 0; j < 512; j++) {
            if (cleanup_pd[j] & 1) {
                cleanup_pt_phys = (uint32_t)(cleanup_pd[j] & ~0xFFFULL);
                pfa_free(cleanup_pt_phys);
            }
        }
    }
    pae_pdpt_pool_used[slot] = 0;

    if (kernel_pdpt_phys && orig_cr3 != kernel_pdpt_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return 0;
}

uint32_t vmm_clone_page_directory(uint32_t src_pd_phys, uint32_t **out_user_pages, uint32_t *out_user_pages_count) {
    if (!src_pd_phys) return 0;

    if (pae_enabled) {
        return vmm_clone_pae_structure(src_pd_phys, out_user_pages, out_user_pages_count);
    }

    uint32_t clone_log_count = 0;
    uint32_t clone_sample_count = 0;

    uint32_t user_page_capacity = 512;
    uint32_t user_page_count = 0;
    uint32_t *user_pages = (uint32_t *)kmalloc(user_page_capacity * sizeof(uint32_t));
    if (!user_pages) {
        printf("vmm_clone_page_directory: Failed to allocate user_pages array\n");
        return 0;
    }

    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    
    uint32_t orig_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pd_phys) : "memory");
    }

    void *new_pd_page = pmm_alloc_low_page();
    if (!new_pd_page) {
        new_pd_page = pmm_alloc_page();
    }
    if (!new_pd_page) {
        printf("vmm_clone_page_directory: Failed to allocate new PD\n");
        if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        }
        if (eflags & (1 << 9)) __asm__ volatile ("sti");
        kfree(user_pages);
        return 0;
    }
    uint32_t new_pd_phys = (uint32_t)new_pd_page;

    uint32_t temp_src_pd = 0xF7000000;
    uint32_t temp_new_pd = 0xF7001000;
    uint32_t temp_src_pt = 0xF7002000;
    uint32_t temp_new_pt = 0xF7003000;
    uint32_t temp_src_page = 0xF7004000;
    uint32_t temp_new_page = 0xF7005000;

    temp_map_raw(temp_new_pd, new_pd_phys);
    uint32_t *new_pd = (uint32_t *)temp_new_pd;
    memset(new_pd, 0, PAGE_SIZE);
    temp_unmap_raw(temp_new_pd);

    temp_map_raw(temp_src_pd, src_pd_phys);
    uint32_t *src_pd = (uint32_t *)temp_src_pd;

    for (uint32_t pd_idx = 0; pd_idx < 768; pd_idx++) {
        uint32_t src_pde = src_pd[pd_idx];
        if (!(src_pde & 1)) continue;

        uint32_t src_pt_phys = src_pde & ~0xFFF;
        uint32_t pde_flags = src_pde & 0xFFF;

        void *new_pt_page = pmm_alloc_low_page();
        if (!new_pt_page) {
            new_pt_page = pmm_alloc_page();
        }
        if (!new_pt_page) {
            printf("vmm_clone_page_directory: Failed to allocate PT for pd_idx %u\n", pd_idx);
            temp_unmap_raw(temp_src_pd);
            goto cleanup_fail;
        }
        uint32_t new_pt_phys = (uint32_t)new_pt_page;

        temp_map_raw(temp_new_pd, new_pd_phys);
        ((uint32_t *)temp_new_pd)[pd_idx] = (new_pt_phys & ~0xFFF) | pde_flags;
        temp_unmap_raw(temp_new_pd);

        temp_map_raw(temp_src_pt, src_pt_phys);
        temp_map_raw(temp_new_pt, new_pt_phys);
        uint32_t *src_pt = (uint32_t *)temp_src_pt;
        uint32_t *new_pt = (uint32_t *)temp_new_pt;

        memset(new_pt, 0, PAGE_SIZE);

        for (uint32_t pt_idx = 0; pt_idx < 1024; pt_idx++) {
            uint32_t src_pte = src_pt[pt_idx];
            if (!(src_pte & 1)) continue;

            uint32_t src_page_phys = src_pte & ~0xFFF;
            uint32_t pte_flags = src_pte & 0xFFF;

            uint32_t new_page_phys = pfa_alloc();
            if (!new_page_phys) {
                printf("vmm_clone_page_directory: Failed to allocate page copy\n");
                temp_unmap_raw(temp_src_pt);
                temp_unmap_raw(temp_new_pt);
                temp_unmap_raw(temp_src_pd);
                goto cleanup_fail;
            }

            if (debugMode && debugLevel >= 5 && clone_should_log_detail(clone_log_count)) {
                DPRINTF3("vmm_clone: copying page pd_idx=%u pt_idx=%u virt=0x%08X src_phys=0x%08X -> new_phys=0x%08X flags=0x%03X\n",
                         pd_idx, pt_idx, (pd_idx<<22) | (pt_idx<<12), src_page_phys, new_page_phys, pte_flags);
            }
            clone_log_count++;

            temp_map_raw(temp_src_page, src_page_phys);
            temp_map_raw(temp_new_page, new_page_phys);

            memcpy((void *)temp_new_page, (void *)temp_src_page, PAGE_SIZE);

            if (debugMode && debugLevel >= 5 && clone_should_log_sample(clone_sample_count)) {
                uint32_t sample_before = ((uint32_t *)temp_src_page)[0];
                uint32_t sample_after = ((uint32_t *)temp_new_page)[0];
                DPRINTF3("vmm_clone: page copy sample: src_phys=0x%08X before=0x%08X new_phys=0x%08X after=0x%08X\n",
                         src_page_phys, sample_before, new_page_phys, sample_after);
            }
            clone_sample_count++;

            temp_unmap_raw(temp_src_page);
            temp_unmap_raw(temp_new_page);

            volatile uint32_t *vpt = (volatile uint32_t *)new_pt;
            vpt[pt_idx] = (new_page_phys & ~0xFFF) | pte_flags;
            
            __asm__ volatile ("" ::: "memory");
            if (debugMode && debugLevel >= 5 && clone_should_log_detail(clone_log_count)) {
                uint32_t verify_pte = vpt[pt_idx];
                DPRINTF3("vmm_clone: set new_pt[%u]=0x%08X verify=0x%08X\n", pt_idx, (new_page_phys & ~0xFFF) | pte_flags, verify_pte);
            }
            clone_log_count++;

            if (user_page_count >= user_page_capacity) {
                printf("vmm_clone: user_pages array full (capacity=%u)!\n", user_page_capacity);
                temp_unmap_raw(temp_src_pt);
                temp_unmap_raw(temp_new_pt);
                temp_unmap_raw(temp_src_pd);
                goto cleanup_fail;
            }
            user_pages[user_page_count++] = new_page_phys;
        }

        temp_unmap_raw(temp_src_pt);
        temp_unmap_raw(temp_new_pt);
    }

    for (uint32_t i = 768; i < 1023; i++) {
        temp_map_raw(temp_new_pd, new_pd_phys);
        uint32_t pde = src_pd[i] & ~0x4;
        ((uint32_t *)temp_new_pd)[i] = pde;
        temp_unmap_raw(temp_new_pd);
    }

    temp_unmap_raw(temp_src_pd);

    temp_map_raw(temp_new_pd, new_pd_phys);
    ((uint32_t *)temp_new_pd)[1023] = (new_pd_phys & ~0xFFF) | 3;
    temp_unmap_raw(temp_new_pd);

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    DPRINTF3("vmm_clone_page_directory: cloned pd 0x%08X -> 0x%08X (%u user pages)\n",
           src_pd_phys, new_pd_phys, user_page_count);

    if (debugMode && debugLevel >= 4) {
    for (uint32_t v = 0x00400000; v < 0x00403000; v += PAGE_SIZE) {
        uint32_t pd_idx = v >> 22;
        uint32_t pt_idx = (v >> 12) & 0x3FF;

        uint32_t temp_src_pd = 0xF7000000;
        uint32_t temp_new_pd = 0xF7001000;
        uint32_t temp_src_pt = 0xF7002000;
        uint32_t temp_new_pt = 0xF7003000;

        temp_map_raw(temp_src_pd, src_pd_phys);
        uint32_t src_pde = ((uint32_t *)temp_src_pd)[pd_idx];
        if (src_pde & 1) {
            uint32_t src_pt_phys = src_pde & ~0xFFF;
            temp_map_raw(temp_src_pt, src_pt_phys);
            uint32_t src_pte = ((uint32_t *)temp_src_pt)[pt_idx];
            temp_unmap_raw(temp_src_pt);
            DPRINTF2("src: virt=0x%08X pde=0x%08X pte=0x%08X\n", v, src_pde, src_pte);
        } else {
            DPRINTF2("src: virt=0x%08X pde=0x%08X (no pt)\n", v, src_pde);
        }
        temp_unmap_raw(temp_src_pd);

        temp_map_raw(temp_new_pd, new_pd_phys);
        uint32_t new_pde = ((uint32_t *)temp_new_pd)[pd_idx];
        if (new_pde & 1) {
            uint32_t new_pt_phys = new_pde & ~0xFFF;
            temp_map_raw(temp_new_pt, new_pt_phys);
            uint32_t new_pte = ((uint32_t *)temp_new_pt)[pt_idx];
            temp_unmap_raw(temp_new_pt);
            DPRINTF2("new: virt=0x%08X pde=0x%08X pte=0x%08X\n", v, new_pde, new_pte);
        } else {
            DPRINTF2("new: virt=0x%08X pde=0x%08X (no pt)\n", v, new_pde);
        }
        temp_unmap_raw(temp_new_pd);
    }
    }

    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return new_pd_phys;

cleanup_fail:
    for (uint32_t i = 0; i < user_page_count; i++) {
        pfa_free(user_pages[i]);
    }
    kfree(user_pages);

    temp_map_raw(temp_new_pd, new_pd_phys);
    uint32_t *cleanup_pd = (uint32_t *)temp_new_pd;
    for (uint32_t i = 0; i < 768; i++) {
        if (cleanup_pd[i] & 1) {
            pfa_free(cleanup_pd[i] & ~0xFFF);
        }
    }
    temp_unmap_raw(temp_new_pd);
    pfa_free(new_pd_phys);

    if (kernel_pd_phys && orig_cr3 != kernel_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return 0;
}
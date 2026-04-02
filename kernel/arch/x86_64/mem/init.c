#include <kernel/mem_map.h>
#include <kernel/multiboot2.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern char _kernel_end[];
extern uint64_t kernel_reserved_frames;
extern uint64_t total_pages_managed;

extern mem_region_t memory_map[MAX_REGIONS];
extern uint64_t num_regions;
extern uint64_t bump_current;
extern uint64_t active_region;
extern uint64_t low_bump;

reserved_region_t reserved_regions[MAX_RESERVED_REGIONS];
uint64_t num_reserved_regions = 0;

static uint64_t multiboot_physical_ram_kb = 0;
static uint64_t multiboot_usable_ram_kb = 0;

uint64_t early_fb_addr;
uint32_t early_fb_width;
uint32_t early_fb_height;
uint32_t early_fb_pitch;
uint8_t early_fb_bpp;
uint8_t early_fb_type;
int early_fb_valid = 0;

#define EARLY_CMDLINE_MAX 256
char early_cmdline[EARLY_CMDLINE_MAX];

uint32_t early_mod_count;

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern void pfa_init_internal_setup(uint64_t bitmap_bytes, uint64_t total_pages, uint64_t kernel_frames);
extern void pfa_init_ram_stats(uint64_t total_kb, uint64_t usable_kb, uint64_t init_free_frames);

void init_mem_map(uint64_t mb_magic, uint64_t mb_ptr) {
    struct multiboot2_tag *tag;
    struct multiboot2_tag_basic_meminfo *meminfo;
    struct multiboot2_tag_mmap *mmap_tag;
    struct multiboot2_mmap_entry *mmap_entry;
    struct multiboot2_tag_module *mod;
    struct multiboot2_tag_framebuffer *fb_tag;
    struct multiboot2_tag_string *cmd_tag;
    void *mb_info;
    uint64_t max_phys_memory;
    uint64_t total_usable_kb;
    uint64_t entry_count;
    uint64_t merged_count;
    uint64_t base;
    uint64_t len;
    uint64_t kernel_end_phys;
    int cl;

    early_fb_valid = 0;
    early_cmdline[0] = '\0';
    early_mod_count = 0;

    if (mb_magic != MULTIBOOT2_MAGIC || mb_ptr == 0) {
        printf("Bad multiboot2 magic (0x%08lX) - skip map.\n", mb_magic);
        return;
    }

    mb_info = (void *)mb_ptr;
    max_phys_memory = 0;
    total_usable_kb = 0;

    tag = multiboot2_first_tag(mb_info);
    while (tag->type != MULTIBOOT2_TAG_END) {

        if (tag->type == MULTIBOOT2_TAG_BASIC_MEMINFO) {
            meminfo = (struct multiboot2_tag_basic_meminfo *)tag;
            multiboot_physical_ram_kb = meminfo->mem_lower + meminfo->mem_upper;
        }

        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            fb_tag = (struct multiboot2_tag_framebuffer *)tag;
            early_fb_addr = fb_tag->framebuffer_addr;
            early_fb_width = fb_tag->framebuffer_width;
            early_fb_height = fb_tag->framebuffer_height;
            early_fb_pitch = fb_tag->framebuffer_pitch;
            early_fb_bpp = fb_tag->framebuffer_bpp;
            early_fb_type = fb_tag->framebuffer_type;
            early_fb_valid = 1;
        }

        if (tag->type == MULTIBOOT2_TAG_CMDLINE) {
            cmd_tag = (struct multiboot2_tag_string *)tag;
            cl = 0;
            while (cmd_tag->string[cl] && cl < EARLY_CMDLINE_MAX - 1) {
                early_cmdline[cl] = cmd_tag->string[cl];
                cl++;
            }
            early_cmdline[cl] = '\0';
        }

        if (tag->type == MULTIBOOT2_TAG_MODULE) {
            early_mod_count++;
        }

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            mmap_tag = (struct multiboot2_tag_mmap *)tag;
            entry_count = (mmap_tag->size - 16) / mmap_tag->entry_size;
            mmap_entry = mmap_tag->entries;

            while (entry_count > 0) {
                base = mmap_entry->addr;
                len = mmap_entry->len;

                if (mmap_entry->type == MULTIBOOT2_MMAP_AVAILABLE && len > 0) {
                    total_usable_kb += len / 1024;
                }
                if (len > 0 && base + len > max_phys_memory) {
                    max_phys_memory = base + len;
                }

                mmap_entry = (struct multiboot2_mmap_entry *)((uint8_t *)mmap_entry + mmap_tag->entry_size);
                entry_count--;
            }
        }
        tag = multiboot2_next_tag(tag);
    }

    if (max_phys_memory > MAX_PHYSICAL_MEMORY) {
        max_phys_memory = MAX_PHYSICAL_MEMORY;
    }
    if ((uint64_t)(max_phys_memory / 1024) > multiboot_physical_ram_kb) {
        multiboot_physical_ram_kb = (uint64_t)(max_phys_memory / 1024);
    }
    if ((uint64_t)total_usable_kb > multiboot_physical_ram_kb) {
        multiboot_physical_ram_kb = (uint64_t)total_usable_kb;
    }
    multiboot_usable_ram_kb = (uint64_t)total_usable_kb;

    merged_count = 0;
    tag = multiboot2_first_tag(mb_info);
    while (tag->type != MULTIBOOT2_TAG_END) {
        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            mmap_tag = (struct multiboot2_tag_mmap *)tag;
            entry_count = (mmap_tag->size - 16) / mmap_tag->entry_size;
            mmap_entry = mmap_tag->entries;

            while (entry_count > 0) {
                base = mmap_entry->addr;
                len = mmap_entry->len;

                if (mmap_entry->type != MULTIBOOT2_MMAP_AVAILABLE || len == 0) {
                    mmap_entry = (struct multiboot2_mmap_entry *)((uint8_t *)mmap_entry + mmap_tag->entry_size);
                    entry_count--;
                    continue;
                }

                if (merged_count == 0 || memory_map[merged_count-1].base + memory_map[merged_count-1].length != base) {
                    if (merged_count < MAX_REGIONS) {
                        memory_map[merged_count].base = base;
                        memory_map[merged_count].length = len;
                        memory_map[merged_count].type = 1;
                        merged_count++;
                    }
                } else {
                    memory_map[merged_count-1].length += len;
                }
                mmap_entry = (struct multiboot2_mmap_entry *)((uint8_t *)mmap_entry + mmap_tag->entry_size);
                entry_count--;
            }
        }
        tag = multiboot2_next_tag(tag);
    }
    num_regions = merged_count;

    DEBUG_MEMORY("Merged: %d regions\n", num_regions);

    if (num_regions > 0) {
        kernel_end_phys = (uint64_t)(uintptr_t)_kernel_end - KERNEL_VMA;
        kernel_end_phys = (kernel_end_phys + 0xFFF) & ~0xFFFUL;

        DEBUG_MEMORY("Kernel ends at phys 0x%016lX\n", (unsigned long)kernel_end_phys);

        bump_current = kernel_end_phys;
        active_region = 0;

        {
            uint64_t i;
            for (i = 0; i < num_regions; i++) {
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
        }

        low_bump = 0;
        {
            uint64_t i;
            for (i = 0; i < num_regions; i++) {
                if (memory_map[i].base < 0x00400000) {
                    uint64_t end = memory_map[i].base + memory_map[i].length;
                    if (end > 0x1000) {
                        low_bump = memory_map[i].base + 0x1000;
                        if (low_bump < kernel_end_phys) low_bump = kernel_end_phys;
                        break;
                    }
                }
            }
        }

        DEBUG_MEMORY("PMM bump ready: Starting at 0x%08lX\n", (unsigned long)bump_current);
        DEBUG_MEMORY("PMM low bump starting at 0x%08lX\n", (unsigned long)low_bump);
    }

    num_reserved_regions = 0;

    {
        uint32_t mb2_total_size = *(uint32_t *)mb_info;
        uint64_t mb_start_page = mb_ptr & ~0xFFFu;
        uint64_t mb_end_page = (mb_ptr + mb2_total_size + 0xFFFu) & ~0xFFFu;
        if (num_reserved_regions < MAX_RESERVED_REGIONS) {
            reserved_regions[num_reserved_regions].start_phys = mb_start_page;
            reserved_regions[num_reserved_regions].end_phys = mb_end_page;
            num_reserved_regions++;
        }
    }

    tag = multiboot2_first_tag(mb_info);
    while (tag->type != MULTIBOOT2_TAG_END) {
        uint64_t start_page;
        uint64_t end_page;

        if (tag->type == MULTIBOOT2_TAG_MODULE) {
            if (num_reserved_regions < MAX_RESERVED_REGIONS) {
                mod = (struct multiboot2_tag_module *)tag;
                start_page = mod->mod_start & ~0xFFFu;
                end_page = (mod->mod_end + 0xFFF) & ~0xFFFu;

                reserved_regions[num_reserved_regions].start_phys = start_page;
                reserved_regions[num_reserved_regions].end_phys = end_page;
                num_reserved_regions++;
            }
        }
        tag = multiboot2_next_tag(tag);
    }
}

void pfa_init(void) {
    uint64_t kernel_end_phys;
    uint64_t kernel_frames;
    uint64_t total_free_frames;
    uint64_t r;
    uint64_t region_base;
    uint64_t region_end;
    uint64_t region_start_capped;
    uint64_t region_end_capped;
    uint64_t start_frame;
    uint64_t end_frame;
    uint64_t region_free;
    uint64_t f;
    uint64_t max_phys;
    uint64_t total_mb;
    uint64_t actual_free;
    uint64_t system_total_ram_kb;
    uint64_t system_usable_ram_kb;
    uint64_t detected_max_phys;
    uint64_t actual_total_pages;
    uint64_t actual_bitmap_bytes;
    uint64_t bitmap_pages;
    uint64_t bitmap_alloc_phys;
    uint64_t rend;
    uint64_t cr3_val;

    detected_max_phys = 0;
    for (r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;
        rend = memory_map[r].base + memory_map[r].length;
        if (rend > detected_max_phys) detected_max_phys = rend;
    }

    for (r = 0; r < num_reserved_regions; r++) {
        if (reserved_regions[r].end_phys > (uint64_t)bump_current) {
            bump_current = (uint64_t)reserved_regions[r].end_phys;
        }
    }

    max_phys = MAX_PHYSICAL_MEMORY;
    if (detected_max_phys > max_phys) detected_max_phys = max_phys;
    if (detected_max_phys == 0) detected_max_phys = max_phys;

    actual_total_pages = (uint64_t)((detected_max_phys + PAGE_SIZE - 1) / PAGE_SIZE);
    actual_bitmap_bytes = (actual_total_pages + 7) / 8;
    actual_bitmap_bytes = (actual_bitmap_bytes + 3) & ~3u;

    pfa_init_internal_setup(actual_bitmap_bytes, actual_total_pages, 0);

    bitmap_pages = (actual_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap_alloc_phys = (bump_current + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    bump_current = bitmap_alloc_phys + (uint64_t)bitmap_pages * PAGE_SIZE;

    {
        extern uint8_t *pfa_bitmap;
        pfa_bitmap = (uint8_t *)(bitmap_alloc_phys + KERNEL_VMA);
        memset(pfa_bitmap, 0xFF, actual_bitmap_bytes);
    }

    printf("PFA: 64-bit mode, managing %u pages (%u KB bitmap, %u pages)\n",
           actual_total_pages, actual_bitmap_bytes / 1024, bitmap_pages);

    kernel_end_phys = (uint64_t)(uintptr_t)_kernel_end - KERNEL_VMA;
    kernel_end_phys = (kernel_end_phys + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (bitmap_alloc_phys + bitmap_pages * PAGE_SIZE > kernel_end_phys) {
        kernel_end_phys = bitmap_alloc_phys + bitmap_pages * PAGE_SIZE;
    }
    for (r = 0; r < num_reserved_regions; r++) {
        if (reserved_regions[r].end_phys > kernel_end_phys) {
            kernel_end_phys = reserved_regions[r].end_phys;
        }
    }
    kernel_frames = (uint64_t)(kernel_end_phys / PAGE_SIZE);

    {
        extern uint64_t kernel_reserved_frames;
        kernel_reserved_frames = kernel_frames;
    }

    printf("PFA: Kernel ends at phys 0x%016lX (%u frames reserved)\n", (unsigned long)kernel_end_phys, kernel_frames);

    total_free_frames = 0;
    for (r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;

        region_base = memory_map[r].base;
        region_end = region_base + memory_map[r].length;

        if (region_end <= detected_max_phys) {
            region_start_capped = region_base;
            region_end_capped = region_end;
        } else if (region_base >= detected_max_phys) {
            continue;
        } else {
            region_start_capped = region_base;
            region_end_capped = detected_max_phys;
        }

        start_frame = (uint64_t)(region_start_capped / PAGE_SIZE);
        end_frame = (uint64_t)((region_end_capped + PAGE_SIZE - 1) / PAGE_SIZE);
        region_free = 0;

        if (end_frame > actual_total_pages) end_frame = actual_total_pages;

        {
            extern void clear_bit(uint64_t bit_idx);
            for (f = start_frame; f < end_frame; f++) {
                if (f < kernel_frames) continue;
                clear_bit(f);
                total_free_frames++;
                region_free++;
            }
        }
        if (region_free > 0) {
            printf("PFA: Region %u [0x%08lX-0x%08lX]: %u free frames\n", r,
                   (unsigned long)region_start_capped, (unsigned long)region_end_capped, region_free);
        } else {
            printf("PFA: Region %u skipped (low RAM protected)\n", r);
        }
    }

    {
        extern void set_bit(uint64_t bit_idx);
        extern bool test_bit(uint64_t bit_idx);
        uint64_t res_start_frame;
        uint64_t res_end_frame;
        uint64_t reserved_count;
        for (r = 0; r < num_reserved_regions; r++) {
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
            printf("PFA: Reserved region %u [0x%016lX-0x%016lX]: %u frames marked as used\n",
                   r, reserved_regions[r].start_phys, reserved_regions[r].end_phys, reserved_count);
        }
    }

    total_mb = (total_free_frames + 255ULL) / 256ULL;
    printf("PFA ready: %llu total free frames (~%llu MB)\n",
           (unsigned long long)total_free_frames, (unsigned long long)total_mb);

    {
        extern uint64_t count_free_frames(void);
        actual_free = count_free_frames();
        if (actual_free != (uint64_t)total_free_frames) {
            printf("PFA WARNING: counted %llu but bitmap shows %u free\n",
                   (unsigned long long)total_free_frames, actual_free);
        }
    }

    system_total_ram_kb = 0;
    for (r = 0; r < num_regions; r++) {
        if (memory_map[r].type != 1) continue;
        region_base = memory_map[r].base;
        region_end = region_base + memory_map[r].length;
        if (region_base >= detected_max_phys) continue;
        if (region_end > detected_max_phys) region_end = detected_max_phys;
        system_total_ram_kb += (uint64_t)((region_end - region_base) / 1024);
    }
    system_usable_ram_kb = system_total_ram_kb;
    pfa_init_ram_stats(system_total_ram_kb, system_usable_ram_kb, (uint64_t)total_free_frames);

    {
        uint64_t kern_phys_start;
        uint64_t kern_phys_end_raw;
        uint64_t kern_bin_kb;
        uint64_t bmp_kb;
        extern char _kernel_start[];
        kern_phys_start = (uint64_t)(uintptr_t)_kernel_start - KERNEL_VMA;
        kern_phys_end_raw = (uint64_t)(uintptr_t)_kernel_end - KERNEL_VMA;
        kern_phys_end_raw = (kern_phys_end_raw + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        kern_bin_kb = (uint64_t)((kern_phys_end_raw - kern_phys_start) / 1024);
        bmp_kb = (bitmap_pages * PAGE_SIZE) / 1024;
        pfa_set_reserved_stats(kern_bin_kb, bmp_kb);
    }

    printf("PFA: Total system RAM: %u KB (~%u MB), InitFree: %u KB\n",
           system_total_ram_kb, system_total_ram_kb / 1024, (uint64_t)(total_free_frames * 4));

    __asm__ volatile ("movq %%cr3, %0" : "=r"(cr3_val));
    vmm_register_kernel_cr3(cr3_val);
}

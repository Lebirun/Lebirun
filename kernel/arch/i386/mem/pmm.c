#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;
extern uint32_t total_pages_managed;
extern uint32_t kernel_reserved_frames;

uint8_t *pfa_bitmap = 0;
static uint32_t bitmap_bytes_used = 0;

static volatile int pfa_lock = 0;

extern mem_region_t memory_map[MAX_REGIONS];
extern uint32_t num_regions;
extern uint64_t bump_current;
extern uint32_t active_region;
extern uint64_t low_bump;

extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);

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

void set_bit(uint32_t bit_idx) {
    if (bit_idx / 8 < bitmap_bytes_used)
        pfa_bitmap[bit_idx / 8] |= (1 << (bit_idx % 8));
}

void clear_bit(uint32_t bit_idx) {
    if (bit_idx / 8 < bitmap_bytes_used)
        pfa_bitmap[bit_idx / 8] &= ~(1 << (bit_idx % 8));
}

bool test_bit(uint32_t bit_idx) {
    if (bit_idx / 8 >= bitmap_bytes_used) return true;
    return pfa_bitmap[bit_idx / 8] & (1 << (bit_idx % 8));
}

uint32_t count_free_frames(void) {
    uint32_t count;
    uint32_t i;
    uint32_t bytes;
    uint32_t word_count;
    uint32_t *words;
    uint32_t w;
    uint32_t b;

    count = 0;
    bytes = (total_pages_managed + 7) / 8;
    words = (uint32_t *)pfa_bitmap;
    word_count = bytes / 4;

    for (i = 0; i < word_count; i++) {
        w = ~words[i];
        w = w - ((w >> 1) & 0x55555555);
        w = (w & 0x33333333) + ((w >> 2) & 0x33333333);
        w = (w + (w >> 4)) & 0x0F0F0F0F;
        count += (w * 0x01010101) >> 24;
    }

    for (i = word_count * 4; i < bytes; i++) {
        b = (uint32_t)(uint8_t)(~pfa_bitmap[i]);
        b = b - ((b >> 1) & 0x55);
        b = (b & 0x33) + ((b >> 2) & 0x33);
        b = (b + (b >> 4)) & 0x0F;
        count += b;
    }

    return count;
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
    } else {
        pfa_lock_release(eflags);
        printf("PFA free: Already free? 0x%08X\n", phys_addr);
    }
}

void pfa_reclaim_kernel_range(uint32_t phys_start, uint32_t phys_end) {
    uint32_t eflags;
    uint32_t start_frame;
    uint32_t end_frame;
    uint32_t f;
    uint32_t count;

    phys_start = (phys_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    phys_end = phys_end & ~(PAGE_SIZE - 1);
    if (phys_end <= phys_start) return;

    start_frame = phys_start / PAGE_SIZE;
    end_frame = phys_end / PAGE_SIZE;
    count = 0;

    pfa_lock_acquire(&eflags);
    for (f = start_frame; f < end_frame && f < total_pages_managed; f++) {
        if (test_bit(f)) {
            clear_bit(f);
            count++;
        }
    }
    pfa_lock_release(eflags);

    printf("PFA: Reclaimed %u kernel pages (%u KB) from 0x%08X-0x%08X\n",
           count, count * 4, phys_start, phys_end);
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

static uint32_t system_total_ram_kb = 0;
static uint32_t system_usable_ram_kb = 0;
static uint32_t initial_free_frames = 0;
static uint32_t kernel_binary_kb = 0;
static uint32_t bitmap_alloc_kb = 0;

uint32_t pfa_get_total_ram_kb(void) {
    return system_total_ram_kb;
}

uint32_t pfa_get_usable_ram_kb(void) {
    return system_usable_ram_kb;
}

uint32_t pfa_get_kernel_used_kb(void) {
    uint32_t allocated;
    uint32_t idx;

    allocated = 0;
    for (idx = kernel_reserved_frames; idx < total_pages_managed; idx++) {
        if (test_bit(idx)) allocated++;
    }

    return allocated * 4;
}

uint32_t pfa_get_kernel_binary_kb(void) {
    return kernel_binary_kb;
}

uint32_t pfa_get_bitmap_kb(void) {
    return bitmap_alloc_kb;
}

void pfa_set_reserved_stats(uint32_t kern_bin_kb, uint32_t bmp_kb) {
    kernel_binary_kb = kern_bin_kb;
    bitmap_alloc_kb = bmp_kb;
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

void pmm_zero_page_phys(uint32_t phys_addr) {
    uint32_t temp_virt;
    uint32_t eflags;
    int had_if;
    volatile uint32_t *w;
    uint32_t i;
    extern uint32_t pae_temp_pt_ready_check(void);

    if (phys_addr < 0x00400000) {
        w = (volatile uint32_t *)(phys_addr + 0xC0000000);
        for (i = 0; i < PAGE_SIZE / 4; i++) {
            w[i] = 0;
        }
        return;
    }

    if (pae_enabled && !pae_temp_pt_ready_check()) {
        return;
    }

    temp_virt = 0xF7006000;

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    had_if = (eflags & (1<<9)) != 0;
    if (had_if) __asm__ volatile ("cli");

    temp_map_raw(temp_virt, phys_addr);

    w = (volatile uint32_t *)temp_virt;
    for (i = 0; i < PAGE_SIZE / 4; i++) {
        w[i] = 0;
    }

    temp_unmap_raw(temp_virt);

    if (had_if) __asm__ volatile ("sti");
}

void pfa_init_internal_setup(uint32_t bitmap_bytes, uint32_t total_pages, uint32_t kernel_frames) {
    bitmap_bytes_used = bitmap_bytes;
    total_pages_managed = total_pages;
    kernel_reserved_frames = kernel_frames;
}

void pfa_init_ram_stats(uint32_t total_kb, uint32_t usable_kb, uint32_t init_free_frames) {
    system_total_ram_kb = total_kb;
    system_usable_ram_kb = usable_kb;
    initial_free_frames = init_free_frames;
}

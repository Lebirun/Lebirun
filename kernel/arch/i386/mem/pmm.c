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

static uint32_t last_alloc_hint = 0;

static volatile uint32_t pfa_cached_free = 0;

static uint8_t *pfa_refcounts = NULL;
static uint32_t pfa_refcount_entries = 0;

static int low_exhausted = 0;

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

static uint32_t find_free_frames_range(uint32_t from, uint32_t to, uint32_t num) {
    uint32_t b;
    uint32_t bit;
    uint32_t frame_idx;
    uint32_t j;
    uint32_t run;
    uint32_t run_start;
    uint32_t b_start;
    uint32_t b_end;

    if (num == 1) {
        b_start = from / 8;
        b_end = (to + 7) / 8;
        if (b_end > bitmap_bytes_used) b_end = bitmap_bytes_used;
        for (b = b_start; b < b_end; b++) {
            if (pfa_bitmap[b] == 0xFF) continue;
            for (bit = 0; bit < 8; bit++) {
                frame_idx = b * 8 + bit;
                if (frame_idx < from) continue;
                if (frame_idx >= to) return 0;
                if (!(pfa_bitmap[b] & (1 << bit))) {
                    set_bit(frame_idx);
                    last_alloc_hint = frame_idx + 1;
                    __sync_fetch_and_sub(&pfa_cached_free, 1);
                    return frame_idx * PAGE_SIZE;
                }
            }
        }
        return 0;
    }

    b_start = from / 8;
    b_end = (to + 7) / 8;
    if (b_end > bitmap_bytes_used) b_end = bitmap_bytes_used;
    run = 0;
    run_start = from;

    for (b = b_start; b < b_end; b++) {
        if (pfa_bitmap[b] == 0xFF) {
            run = 0;
            run_start = (b + 1) * 8;
            continue;
        }
        for (bit = 0; bit < 8; bit++) {
            frame_idx = b * 8 + bit;
            if (frame_idx < from) continue;
            if (frame_idx >= to) return 0;
            if (pfa_bitmap[b] & (1 << bit)) {
                run = 0;
                run_start = frame_idx + 1;
            } else {
                if (run == 0) run_start = frame_idx;
                run++;
                if (run >= num) {
                    for (j = 0; j < num; j++) set_bit(run_start + j);
                    last_alloc_hint = run_start + num;
                    __sync_fetch_and_sub(&pfa_cached_free, num);
                    return run_start * PAGE_SIZE;
                }
            }
        }
    }
    return 0;
}

static uint32_t find_free_frames(uint32_t num) {
    uint32_t start;
    uint32_t eflags;
    uint32_t result;
    uint32_t limit;

    if (num == 0) return 0;

    pfa_lock_acquire(&eflags);

    limit = total_pages_managed;
    if (limit > 0x100000)
        limit = 0x100000;

    start = last_alloc_hint;
    if (start < kernel_reserved_frames) start = kernel_reserved_frames;
    if (start >= limit) start = kernel_reserved_frames;

    result = find_free_frames_range(start, limit, num);
    if (!result && start > kernel_reserved_frames) {
        result = find_free_frames_range(kernel_reserved_frames, start, num);
    }

    pfa_lock_release(eflags);
    return result;
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
                        __sync_fetch_and_sub(&pfa_cached_free, 1);
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

    if (pfa_refcounts && (uint32_t)idx < pfa_refcount_entries && pfa_refcounts[(uint32_t)idx] > 0) {
        pfa_refcounts[(uint32_t)idx]--;
        return;
    }

    byte_idx = (uint32_t)(idx / 8);
    bit_idx = (uint32_t)(idx % 8);
    
    pfa_lock_acquire(&eflags);
    if (pfa_bitmap[byte_idx] & (1 << bit_idx)) {
        pfa_bitmap[byte_idx] &= ~(1 << bit_idx);
        __sync_fetch_and_add(&pfa_cached_free, 1);
    }
    pfa_lock_release(eflags);
}

void pfa_free(uint32_t phys_addr) {
    uint32_t eflags;
    uint32_t idx;
    
    if (phys_addr % PAGE_SIZE != 0) {
        printf("PFA free: Invalid addr 0x%08X\n", phys_addr);
        return;
    }
    idx = phys_addr / PAGE_SIZE;
    if (idx < kernel_reserved_frames) {
        printf("PFA free: Attempt to free kernel frame 0x%08X (idx %u) ignored\n", phys_addr, idx);
        return;
    }

    if (pfa_refcounts && idx < pfa_refcount_entries && pfa_refcounts[idx] > 0) {
        pfa_refcounts[idx]--;
        return;
    }
    
    pfa_lock_acquire(&eflags);
    if (test_bit(idx)) {
        clear_bit(idx);
        __sync_fetch_and_add(&pfa_cached_free, 1);
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
    __sync_fetch_and_add(&pfa_cached_free, count);
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
        if (test_bit(idx)) {
            clear_bit(idx);
            __sync_fetch_and_add(&pfa_cached_free, 1);
        }
    }
    pfa_lock_release(eflags);
}

uint32_t pfa_count_free(void) {
    return pfa_cached_free;
}

void pfa_sync_free_count(void) {
    pfa_cached_free = count_free_frames();
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
    uint32_t eflags;
    uint32_t i;
    uint64_t region_end;
    uint64_t alloc_start;
    uint64_t candidate;
    uint32_t idx_alloc;
    void *page;
    uint32_t byte_idx;
    uint32_t bit;
    uint32_t frame_idx;
    uint32_t scan_start;
    uint32_t scan_end;
    uint32_t b;

    if (num_regions == 0) return NULL;

    pfa_lock_acquire(&eflags);

    for (i = active_region; i < num_regions; i++) {
        region_end = memory_map[i].base + memory_map[i].length;

        if (memory_map[i].base >= 0x100000000ULL)
            continue;

        if (bump_current < memory_map[i].base) {
            bump_current = memory_map[i].base + 0x1000;
        }

        bump_current = (bump_current + 0xFFF) & ~0xFFF;

        if (bump_current + 4096 <= region_end) {
            active_region = i;
            idx_alloc = (uint32_t)(bump_current / PAGE_SIZE);
            scan_end = (uint32_t)(region_end / PAGE_SIZE);
            if (scan_end > total_pages_managed) scan_end = total_pages_managed;
            if (scan_end > 0x100000) scan_end = 0x100000;

            scan_start = idx_alloc / 8;
            for (b = scan_start; b < (scan_end + 7) / 8; b++) {
                if (b >= bitmap_bytes_used) break;
                if (pfa_bitmap[b] == 0xFF) continue;
                for (bit = 0; bit < 8; bit++) {
                    frame_idx = b * 8 + bit;
                    if (frame_idx < idx_alloc) continue;
                    if (frame_idx >= scan_end) break;
                    if (!(pfa_bitmap[b] & (1 << bit))) {
                        alloc_start = (uint64_t)frame_idx * PAGE_SIZE;
                        bump_current = alloc_start + 4096;
                        set_bit(frame_idx);
                        __sync_fetch_and_sub(&pfa_cached_free, 1);
                        pfa_lock_release(eflags);
                        return (void *)(uint32_t)alloc_start;
                    }
                }
            }

            bump_current = (i + 1 < num_regions) ? memory_map[i+1].base : 0;
            continue;
        }

        bump_current = (i + 1 < num_regions) ? memory_map[i+1].base : 0;
    }

    pfa_lock_release(eflags);

    idx_alloc = find_free_frames(1);
    if (idx_alloc) {
        return (void *)idx_alloc;
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
    uint32_t eflags;
    uint64_t try_addr;
    int in_region;
    uint32_t i;
    uint64_t rstart;
    uint64_t rend;
    uint32_t idx_alloc;
    void *page;
    uint64_t next_start;

    if (num_regions == 0) return NULL;
    if (low_bump == 0) return NULL;
    if (low_exhausted) return NULL;

    pfa_lock_acquire(&eflags);

    try_addr = (low_bump + 0xFFF) & ~0xFFF;

    while (try_addr < 0x00400000) {
        in_region = 0;
        for (i = 0; i < num_regions; i++) {
            rstart = memory_map[i].base;
            rend = rstart + memory_map[i].length;
            if (try_addr >= rstart && try_addr + PAGE_SIZE <= rend && try_addr < 0x00400000) {
                in_region = 1;
                idx_alloc = (uint32_t)(try_addr / PAGE_SIZE);
                if (idx_alloc < total_pages_managed && !test_bit(idx_alloc)) {
                    low_bump = try_addr + PAGE_SIZE;
                    set_bit(idx_alloc);
                    __sync_fetch_and_sub(&pfa_cached_free, 1);
                    page = (void *)(uint32_t)try_addr;
                    pfa_lock_release(eflags);
                    pmm_zero_page_phys((uint32_t)page);
                    return page;
                } else {
                    try_addr += PAGE_SIZE;
                    low_bump = try_addr;
                    break;
                }
            }
        }

        if (!in_region) {
            next_start = 0;
            for (i = 0; i < num_regions; i++) {
                rstart = memory_map[i].base;
                if (rstart > try_addr && rstart < 0x00400000) {
                    if (next_start == 0 || rstart < next_start) next_start = rstart;
                }
            }
            if (next_start == 0) {
                low_exhausted = 1;
                pfa_lock_release(eflags);
                return NULL;
            }
            try_addr = next_start;
            low_bump = try_addr;
        }
    }

    low_exhausted = 1;
    pfa_lock_release(eflags);
    return NULL;
}

static volatile int pmm_zero_lock = 0;

void pmm_zero_page_phys(uint32_t phys_addr) {
    uint32_t temp_virt;
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

    while (__sync_lock_test_and_set(&pmm_zero_lock, 1)) {}

    temp_map_raw(temp_virt, phys_addr);

    w = (volatile uint32_t *)temp_virt;
    for (i = 0; i < PAGE_SIZE / 4; i++) {
        w[i] = 0;
    }

    temp_unmap_raw(temp_virt);

    __sync_lock_release(&pmm_zero_lock);
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
    pfa_cached_free = count_free_frames();
}

void pfa_ref_init(void) {
    uint32_t size;

    if (pfa_refcounts) return;
    pfa_refcount_entries = total_pages_managed;
    size = pfa_refcount_entries * sizeof(uint8_t);
    pfa_refcounts = (uint8_t *)kmalloc(size);
    if (pfa_refcounts) {
        memset(pfa_refcounts, 0, size);
    }
}

void pfa_ref_inc(uint32_t phys_addr) {
    uint32_t idx;

    if (!pfa_refcounts) pfa_ref_init();
    if (!pfa_refcounts) return;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= pfa_refcount_entries) return;
    if (pfa_refcounts[idx] < 255) {
        pfa_refcounts[idx]++;
    }
}

int pfa_ref_dec(uint32_t phys_addr) {
    uint32_t idx;

    if (!pfa_refcounts) return 0;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= pfa_refcount_entries) return 0;
    if (pfa_refcounts[idx] > 0) {
        pfa_refcounts[idx]--;
        return (int)pfa_refcounts[idx];
    }
    return 0;
}

uint8_t pfa_ref_get(uint32_t phys_addr) {
    uint32_t idx;

    if (!pfa_refcounts) return 0;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= pfa_refcount_entries) return 0;
    return pfa_refcounts[idx];
}

void pfa_cow_release(uint32_t phys_addr) {
    uint32_t idx;
    uint8_t ref;

    if (!phys_addr) return;
    if (!pfa_refcounts) {
        pfa_free(phys_addr);
        return;
    }
    idx = phys_addr / PAGE_SIZE;
    if (idx >= pfa_refcount_entries) {
        pfa_free(phys_addr);
        return;
    }
    ref = pfa_refcounts[idx];
    if (ref > 1) {
        pfa_refcounts[idx]--;
        return;
    }
    if (ref == 1) {
        pfa_refcounts[idx] = 0;
    }
    pfa_free(phys_addr);
}

void pfa_cow_release64(uint64_t phys_addr) {
    pfa_cow_release((uint32_t)phys_addr);
}

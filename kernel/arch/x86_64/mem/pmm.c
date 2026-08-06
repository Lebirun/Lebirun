#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/pit.h>
#include <lebirun/vring.h>
#include <string.h>

extern uint64_t total_pages_managed;
extern uint64_t kernel_reserved_frames;
extern int scheduler_initialized;
extern void task_memory_pressure_request(void);

uint8_t *pfa_bitmap = 0;
static uint32_t pfa_inline_directory[PFA_INLINE_DIRECTORY_ENTRIES];
static uint32_t *pfa_directory_extension = NULL;
static uint64_t pfa_directory_entries = 0;
static uint64_t bitmap_entries_used = 0;
static uint64_t bitmap_leaf_pages = 0;

static volatile int pfa_lock = 0;

static uint64_t last_alloc_hint = 0;

static volatile uint64_t pfa_cached_free = 0;
static uint64_t kernel_reclaimed_pages = 0;

static uint64_t pfa_refcount_entries = 0;

typedef struct refht_node {
    uint64_t page_idx;
    uint8_t refcount;
    struct refht_node *next;
} refht_node_t;

static refht_node_t *refht_head;
static refht_node_t *refht_deferred_free;
static uint64_t refht_active_node_count = 0;
static uint64_t refht_deferred_free_count = 0;
static int refht_initialized = 0;
static volatile int refht_lock_val = 0;

static int low_exhausted = 0;
static uint64_t low_page_limit = 0x00400000;
static uint64_t cold_low_start_frame = 0;
static uint64_t cold_low_end_frame = 0;

extern mem_region_t memory_map[MAX_REGIONS];
extern uint64_t num_regions;
extern uint64_t bump_current;
extern uint64_t active_region;
extern uint64_t low_bump;

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);

bool test_bit(uint64_t frame_idx);

static inline void pfa_lock_acquire(uint64_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&pfa_lock, 1)) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void pfa_lock_release(uint64_t eflags) {
    __sync_lock_release(&pfa_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

void *pmm_alloc_early_page(void) {
    uint64_t addr;
    uint64_t i;
    uint64_t rend;

    bump_current = (bump_current + 0xFFF) & ~0xFFFULL;
    for (i = 0; i < num_regions; i++) {
        if (memory_map[i].type != 1) continue;
        rend = memory_map[i].base + memory_map[i].length;
        if (bump_current < memory_map[i].base) bump_current = memory_map[i].base;
        if (bump_current >= rend) continue;
        if (bump_current + PAGE_SIZE > rend) continue;
        addr = bump_current;
        bump_current += PAGE_SIZE;
        return (void *)addr;
    }
    return NULL;
}

void *pmm_alloc_early_pages(uint64_t num) {
    uint64_t addr;
    uint64_t i;
    uint64_t rend;
    uint64_t bytes;

    if (num == 0) return NULL;
    if (pfa_bitmap) return (void *)pfa_alloc_contiguous(num);
    bytes = num * PAGE_SIZE;
    bump_current = (bump_current + 0xFFF) & ~0xFFFULL;
    for (i = 0; i < num_regions; i++) {
        if (memory_map[i].type != 1) continue;
        rend = memory_map[i].base + memory_map[i].length;
        if (bump_current < memory_map[i].base) bump_current = memory_map[i].base;
        if (bump_current >= rend) continue;
        if (bump_current + bytes > rend) continue;
        addr = bump_current;
        bump_current += bytes;
        return (void *)addr;
    }
    return NULL;
}

static int frame_is_usable(uint64_t frame_idx) {
    uint64_t phys;
    uint64_t region_start;
    uint64_t region_end;
    uint64_t i;

    phys = frame_idx * PAGE_SIZE;
    for (i = 0; i < num_regions; i++) {
        if (memory_map[i].type != 1) continue;
        region_start = memory_map[i].base;
        region_end = region_start + memory_map[i].length;
        if (phys >= region_start && phys + PAGE_SIZE <= region_end) return 1;
    }
    return 0;
}

static int frame_is_reserved(uint64_t frame_idx) {
    uint64_t phys;
    uint64_t i;

    phys = frame_idx * PAGE_SIZE;
    for (i = 0; i < num_reserved_regions; i++) {
        if (phys >= reserved_regions[i].start_phys &&
            phys < reserved_regions[i].end_phys) return 1;
    }
    return 0;
}

static uint32_t bitmap_directory_get(uint64_t chunk) {
    if (chunk >= pfa_directory_entries) return 0;
    if (chunk < PFA_INLINE_DIRECTORY_ENTRIES)
        return pfa_inline_directory[chunk];
    if (!pfa_directory_extension) return 0;
    return pfa_directory_extension[chunk - PFA_INLINE_DIRECTORY_ENTRIES];
}

static void bitmap_directory_set(uint64_t chunk, uint32_t frame) {
    if (chunk >= pfa_directory_entries) return;
    if (chunk < PFA_INLINE_DIRECTORY_ENTRIES) {
        pfa_inline_directory[chunk] = frame;
        return;
    }
    if (!pfa_directory_extension) return;
    pfa_directory_extension[chunk - PFA_INLINE_DIRECTORY_ENTRIES] = frame;
}

static uint8_t *bitmap_get_leaf(uint64_t frame_idx) {
    uint64_t chunk;
    uint32_t leaf_frame;

    if (!pfa_bitmap || frame_idx >= bitmap_entries_used) return NULL;
    chunk = frame_idx / PFA_SPARSE_CHUNK_FRAMES;
    leaf_frame = bitmap_directory_get(chunk);
    if (leaf_frame == 0) return NULL;
    return (uint8_t *)((uint64_t)leaf_frame * PAGE_SIZE + KERNEL_VMA);
}

static bool bitmap_test_raw(uint64_t frame_idx) {
    uint64_t bit_idx;
    uint8_t *leaf;

    leaf = bitmap_get_leaf(frame_idx);
    if (!leaf) return false;
    bit_idx = frame_idx % PFA_SPARSE_CHUNK_FRAMES;
    return (leaf[bit_idx / 8] & (1u << (bit_idx % 8))) != 0;
}

static int bitmap_leaf_candidate(uint64_t exclude_start,
                                 uint64_t exclude_end, uint64_t *frame_out) {
    uint64_t start;
    uint64_t end;
    uint64_t frame;

    start = kernel_reserved_frames;
    end = 0x80000000ULL / PAGE_SIZE;
    if (end > bitmap_entries_used) end = bitmap_entries_used;
    for (frame = start; frame < end; frame++) {
        if (frame >= exclude_start && frame < exclude_end) continue;
        if (!frame_is_usable(frame) || frame_is_reserved(frame)) continue;
        if (bitmap_test_raw(frame)) continue;
        *frame_out = frame;
        return 0;
    }
    return -1;
}

static int bitmap_ensure_leaf(uint64_t frame_idx, uint64_t exclude_start,
                              uint64_t exclude_end) {
    uint64_t chunk;
    uint64_t leaf_frame;
    uint64_t leaf_phys;
    uint64_t leaf_bit;
    uint8_t *storage_leaf;
    uint8_t *leaf;

    if (!pfa_bitmap || frame_idx >= bitmap_entries_used) return -1;
    chunk = frame_idx / PFA_SPARSE_CHUNK_FRAMES;
    if (bitmap_directory_get(chunk) != 0) return 0;
    if (bitmap_leaf_candidate(exclude_start, exclude_end, &leaf_frame) != 0)
        return -1;
    leaf_phys = leaf_frame * PAGE_SIZE;
    leaf = (uint8_t *)(leaf_phys + KERNEL_VMA);
    memset(leaf, 0, PAGE_SIZE);
    bitmap_directory_set(chunk, (uint32_t)leaf_frame);
    leaf_bit = leaf_frame % PFA_SPARSE_CHUNK_FRAMES;
    storage_leaf = bitmap_get_leaf(leaf_frame);
    if (!storage_leaf) {
        bitmap_directory_set(chunk, 0);
        return -1;
    }
    storage_leaf[leaf_bit / 8] |= (uint8_t)(1u << (leaf_bit % 8));
    bitmap_leaf_pages++;
    if (pfa_cached_free != 0) __sync_fetch_and_sub(&pfa_cached_free, 1);
    return 0;
}

static int bitmap_prepare_range(uint64_t start, uint64_t count) {
    uint64_t frame;
    uint64_t end;
    uint64_t chunk_end;

    if (count == 0 || start > UINT64_MAX - count) return -1;
    end = start + count;
    frame = start;
    while (frame < end) {
        if (bitmap_ensure_leaf(frame, start, end) != 0) return -1;
        chunk_end = ((frame / PFA_SPARSE_CHUNK_FRAMES) + 1) *
                    PFA_SPARSE_CHUNK_FRAMES;
        frame = chunk_end < end ? chunk_end : end;
    }
    return 0;
}

void set_bit(uint64_t frame_idx) {
    uint64_t bit_idx;
    uint8_t *leaf;

    if (frame_idx >= bitmap_entries_used) return;
    if (bitmap_ensure_leaf(frame_idx, frame_idx, frame_idx + 1) != 0) return;
    leaf = bitmap_get_leaf(frame_idx);
    if (!leaf) return;
    bit_idx = frame_idx % PFA_SPARSE_CHUNK_FRAMES;
    leaf[bit_idx / 8] |= (uint8_t)(1u << (bit_idx % 8));
}

void clear_bit(uint64_t frame_idx) {
    uint64_t bit_idx;
    uint8_t *leaf;

    leaf = bitmap_get_leaf(frame_idx);
    if (!leaf) return;
    bit_idx = frame_idx % PFA_SPARSE_CHUNK_FRAMES;
    leaf[bit_idx / 8] &= (uint8_t)~(1u << (bit_idx % 8));
}

bool test_bit(uint64_t frame_idx) {
    if (frame_idx >= bitmap_entries_used) return true;
    if (!frame_is_usable(frame_idx)) return true;
    return bitmap_test_raw(frame_idx);
}

uint64_t KERNEL_INIT count_free_frames(void) {
    uint64_t count;
    uint64_t region_start;
    uint64_t region_end;
    uint64_t frame;
    uint64_t i;

    count = 0;
    for (i = 0; i < num_regions; i++) {
        if (memory_map[i].type != 1) continue;
        region_start = (memory_map[i].base + PAGE_SIZE - 1) / PAGE_SIZE;
        region_end = (memory_map[i].base + memory_map[i].length) / PAGE_SIZE;
        if (region_start >= bitmap_entries_used) continue;
        if (region_end > bitmap_entries_used) region_end = bitmap_entries_used;
        for (frame = region_start; frame < region_end; frame++) {
            if (!bitmap_test_raw(frame)) count++;
        }
    }
    return count;
}

static uint64_t find_free_frames_bitmap(uint64_t from, uint64_t to,
                                        uint64_t num, uint64_t phys_offset) {
    uint64_t frame_idx;
    uint64_t j;
    uint64_t run;
    uint64_t run_start;
    run = 0;
    run_start = from;
    for (frame_idx = from; frame_idx < to; frame_idx++) {
        if (test_bit(frame_idx + phys_offset)) {
            run = 0;
            run_start = frame_idx + 1;
        } else {
            if (run == 0) run_start = frame_idx;
            run++;
            if (run >= num) {
                if (bitmap_prepare_range(run_start + phys_offset, num) != 0)
                    return 0;
                for (j = 0; j < num; j++) {
                    set_bit(run_start + phys_offset + j);
                }
                last_alloc_hint = run_start + phys_offset + num;
                __sync_fetch_and_sub(&pfa_cached_free, num);
                return (run_start + phys_offset) * PAGE_SIZE;
            }
        }
    }
    return 0;
}

static uint64_t find_free_frames_range(uint64_t from, uint64_t to, uint64_t num) {
    if (to <= from || num == 0) return 0;
    return find_free_frames_bitmap(from, to, num, 0);
}

static uint64_t find_free_frames(uint64_t num) {
    uint64_t start;
    uint64_t eflags;
    uint64_t result;
    uint64_t limit;

    if (num == 0) return 0;

    pfa_lock_acquire(&eflags);

    limit = total_pages_managed;

    start = last_alloc_hint;
    if (start < kernel_reserved_frames) start = kernel_reserved_frames;
    if (start >= limit) start = kernel_reserved_frames;

    result = find_free_frames_range(start, limit, num);
    if (!result && start > kernel_reserved_frames) {
        result = find_free_frames_range(kernel_reserved_frames, start, num);
    }
    if (!result && cold_low_end_frame > cold_low_start_frame) {
        result = find_free_frames_range(cold_low_start_frame,
                                        cold_low_end_frame, num);
    }

    pfa_lock_release(eflags);
    return result;
}

uint64_t pfa_alloc(void) {
    uint64_t result;

    result = find_free_frames(1);
    if (!result && scheduler_initialized) task_memory_pressure_request();
    return result;
}

void pfa_free(uint64_t phys_addr) {
    uint64_t idx;
    uint64_t eflags;
    uint8_t cur_ref;

    if (phys_addr % PAGE_SIZE != 0) {
        return;
    }

    idx = phys_addr / PAGE_SIZE;
    if (idx >= (uint64_t)total_pages_managed) {
        return;
    }
    if (idx < kernel_reserved_frames &&
        (idx < cold_low_start_frame || idx >= cold_low_end_frame)) {
        return;
    }
    if (!frame_is_usable(idx) || frame_is_reserved(idx)) return;
    if ((uint64_t)idx < pfa_refcount_entries) {
        cur_ref = pfa_ref_get((uint64_t)(idx * PAGE_SIZE));
        if (cur_ref > 1) {
            pfa_ref_dec((uint64_t)(idx * PAGE_SIZE));
            return;
        }
        if (cur_ref == 1) {
            pfa_ref_dec((uint64_t)(idx * PAGE_SIZE));
        }
    }

    pfa_lock_acquire(&eflags);
    if (test_bit(idx)) {
        clear_bit(idx);
        if (idx < last_alloc_hint) last_alloc_hint = idx;
        __sync_fetch_and_add(&pfa_cached_free, 1);
    }
    pfa_lock_release(eflags);
}

uint64_t KERNEL_INIT pfa_release_multiboot_range(uint64_t phys_start,
                                                 uint64_t phys_end) {
    uint64_t start_frame;
    uint64_t end_frame;
    uint64_t frame;
    uint64_t region;
    uint64_t freed;
    uint64_t eflags;
    uint64_t i;

    if ((phys_start & (PAGE_SIZE - 1)) != 0) return 0;
    if ((phys_end & (PAGE_SIZE - 1)) != 0) return 0;
    if (phys_end <= phys_start) return 0;

    region = num_reserved_regions;
    for (i = 0; i < num_reserved_regions; i++) {
        if (reserved_regions[i].kind != RESERVED_REGION_MULTIBOOT_INFO)
            continue;
        if (reserved_regions[i].start_phys == phys_start &&
            reserved_regions[i].end_phys == phys_end) {
            region = i;
            break;
        }
    }
    if (region == num_reserved_regions) return 0;

    start_frame = phys_start / PAGE_SIZE;
    end_frame = phys_end / PAGE_SIZE;
    if (end_frame > total_pages_managed) return 0;
    for (frame = start_frame; frame < end_frame; frame++) {
        if (!frame_is_usable(frame)) return 0;
    }

    freed = 0;
    pfa_lock_acquire(&eflags);
    for (frame = start_frame; frame < end_frame; frame++) {
        if (bitmap_test_raw(frame)) {
            clear_bit(frame);
            freed++;
        }
    }
    for (i = region + 1; i < num_reserved_regions; i++) {
        reserved_regions[i - 1] = reserved_regions[i];
    }
    num_reserved_regions--;
    if (freed != 0) {
        __sync_fetch_and_add(&pfa_cached_free, freed);
        if (start_frame < last_alloc_hint) last_alloc_hint = start_frame;
    }
    pfa_lock_release(eflags);
    return freed;
}

uint64_t KERNEL_INIT pfa_release_cold_low_memory(uint64_t phys_end) {
    uint64_t start_frame;
    uint64_t end_frame;
    uint64_t frame;
    uint64_t freed;
    uint64_t eflags;

    if ((phys_end & (PAGE_SIZE - 1)) != 0) return 0;
    start_frame = 1;
    end_frame = phys_end / PAGE_SIZE;
    if (end_frame > kernel_reserved_frames)
        end_frame = kernel_reserved_frames;
    if (end_frame <= start_frame) return 0;

    freed = 0;
    pfa_lock_acquire(&eflags);
    for (frame = start_frame; frame < end_frame; frame++) {
        if (!frame_is_usable(frame) || frame_is_reserved(frame)) continue;
        if (bitmap_test_raw(frame)) {
            clear_bit(frame);
            freed++;
        }
    }
    cold_low_start_frame = start_frame;
    cold_low_end_frame = end_frame;
    if (freed != 0) {
        __sync_fetch_and_add(&pfa_cached_free, freed);
        kernel_reclaimed_pages += freed;
    }
    pfa_lock_release(eflags);
    return freed;
}

static void pfa_reclaim_kernel_range_internal(uint64_t phys_start,
                                              uint64_t phys_end,
                                              int report) {
    uint64_t eflags;
    uint64_t start_frame;
    uint64_t end_frame;
    uint64_t f;
    uint64_t count;
    uint64_t us;
    uint32_t secs;
    uint32_t frac;

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
    kernel_reclaimed_pages += count;
    pfa_lock_release(eflags);

    if (report) {
        us = pit_get_uptime_us();
        secs = (uint32_t)(us / 1000000);
        frac = (uint32_t)(us % 1000000);
        klog_printf(0,
                    "[%5u.%06u] PFA: Reclaimed %u kernel pages (%u KB) from 0x%08X-0x%08X\n",
                    secs, frac, count, count * 4, phys_start, phys_end);
    }
}

void pfa_reclaim_kernel_range(uint64_t phys_start, uint64_t phys_end) {
    pfa_reclaim_kernel_range_internal(phys_start, phys_end, 1);
}

void pfa_reclaim_kernel_range_quiet(uint64_t phys_start, uint64_t phys_end) {
    pfa_reclaim_kernel_range_internal(phys_start, phys_end, 0);
}

uint64_t pfa_alloc_contiguous(uint64_t num_frames) {
    uint64_t result;

    if (num_frames == 0) return 0;
    result = find_free_frames(num_frames);
    if (!result && scheduler_initialized) task_memory_pressure_request();
    return result;
}

void pfa_free_contiguous(uint64_t phys_addr, uint64_t num_frames) {
    uint64_t eflags;
    uint64_t i;
    uint64_t idx;
    uint64_t start_idx;
    uint64_t freed;
    
    if (phys_addr % PAGE_SIZE != 0 || num_frames == 0) return;
    start_idx = phys_addr / PAGE_SIZE;
    freed = 0;
    
    pfa_lock_acquire(&eflags);
    for (i = 0; i < num_frames; i++) {
        idx = start_idx + i;
        if (idx >= total_pages_managed) break;
        if (idx < kernel_reserved_frames &&
            (idx < cold_low_start_frame || idx >= cold_low_end_frame))
            continue;
        if (!frame_is_usable(idx) || frame_is_reserved(idx)) continue;
        if (test_bit(idx)) {
            clear_bit(idx);
            __sync_fetch_and_add(&pfa_cached_free, 1);
            freed++;
        }
    }
    if (freed != 0 && start_idx < last_alloc_hint) {
        last_alloc_hint = start_idx;
    }
    pfa_lock_release(eflags);
}

uint64_t pfa_count_free(void) {
    return pfa_cached_free;
}

static uint64_t system_total_ram_kb = 0;
static uint64_t system_usable_ram_kb = 0;
static uint64_t initial_free_frames = 0;
static uint64_t kernel_binary_kb = 0;
static uint64_t bitmap_alloc_kb = 0;

uint64_t pfa_get_total_ram_kb(void) {
    return system_total_ram_kb;
}

uint64_t pfa_get_usable_ram_kb(void) {
    return system_usable_ram_kb;
}

uint64_t pfa_get_kernel_used_kb(void) {
    uint64_t free_frames;

    free_frames = pfa_cached_free;
    if (free_frames >= initial_free_frames) return 0;
    return (initial_free_frames - free_frames) * 4;
}

uint64_t pfa_get_kernel_binary_kb(void) {
    return kernel_binary_kb;
}

uint64_t pfa_get_bitmap_kb(void) {
    return bitmap_alloc_kb + bitmap_leaf_pages * (PAGE_SIZE / 1024);
}

uint64_t pfa_get_kernel_reclaimed_pages(void) {
    return kernel_reclaimed_pages;
}

void KERNEL_INIT pfa_set_reserved_stats(uint64_t kern_bin_kb,
                                        uint64_t bmp_kb) {
    kernel_binary_kb = kern_bin_kb;
    bitmap_alloc_kb = bmp_kb;
}

void *pmm_alloc_page(void) {
    uint64_t eflags;
    uint64_t i;
    uint64_t region_end;
    uint64_t alloc_start;
    uint64_t idx_alloc;
    uint64_t scan_end;

    if (num_regions == 0) return NULL;

    pfa_lock_acquire(&eflags);

    for (i = active_region; i < num_regions; i++) {
        region_end = memory_map[i].base + memory_map[i].length;



        if (bump_current < memory_map[i].base) {
            bump_current = memory_map[i].base + 0x1000;
        }

        bump_current = (bump_current + 0xFFF) & ~0xFFF;

        if (bump_current + 4096 <= region_end) {
            active_region = i;
            idx_alloc = (uint64_t)(bump_current / PAGE_SIZE);
            scan_end = (uint64_t)(region_end / PAGE_SIZE);
            if (scan_end > total_pages_managed) scan_end = total_pages_managed;


            alloc_start = find_free_frames_range(idx_alloc, scan_end, 1);
            if (alloc_start) {
                bump_current = alloc_start + PAGE_SIZE;
                pfa_lock_release(eflags);
                return (void *)(uint64_t)alloc_start;
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

void *pmm_alloc_pages(uint64_t num) {
    uint64_t addr;

    if (num == 0) return NULL;
    addr = find_free_frames(num);
    if (!addr) return NULL;
    return (void *)(uint64_t)addr;
}

void *pmm_alloc_low_page(void) {
    uint64_t eflags;
    uint64_t try_addr;
    int in_region;
    uint64_t i;
    uint64_t rstart;
    uint64_t rend;
    uint64_t idx_alloc;
    void *page;
    uint64_t next_start;

    if (num_regions == 0) return NULL;
    if (low_bump == 0) return NULL;
    if (low_exhausted) return NULL;

    pfa_lock_acquire(&eflags);

    try_addr = (low_bump + 0xFFF) & ~0xFFF;

    while (try_addr < low_page_limit) {
        in_region = 0;
        for (i = 0; i < num_regions; i++) {
            rstart = memory_map[i].base;
            rend = rstart + memory_map[i].length;
            if (try_addr >= rstart && try_addr + PAGE_SIZE <= rend && try_addr < low_page_limit) {
                in_region = 1;
                idx_alloc = (uint64_t)(try_addr / PAGE_SIZE);
                if (idx_alloc < total_pages_managed && !test_bit(idx_alloc)) {
                    low_bump = try_addr + PAGE_SIZE;
                    set_bit(idx_alloc);
                    __sync_fetch_and_sub(&pfa_cached_free, 1);
                    page = (void *)(uint64_t)try_addr;
                    pfa_lock_release(eflags);
                    pmm_zero_page_phys((uint64_t)page);
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
                if (rstart > try_addr && rstart < low_page_limit) {
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

void pmm_zero_page_phys(uint64_t phys_addr) {
    uint64_t temp_virt;
    uint64_t saved_flags;
    extern uint64_t pt_temp_pt_ready_check(void);

    if (!pt_temp_pt_ready_check()) {
        if (phys_addr < low_page_limit) {
            memset((void *)(phys_addr + KERNEL_VMA), 0, PAGE_SIZE);
        }
        return;
    }

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(6);

    temp_map_raw(temp_virt, phys_addr);
    memset((void *)temp_virt, 0, PAGE_SIZE);
    temp_unmap_raw(temp_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

void KERNEL_EARLY_INIT pfa_init_internal_setup(
        uint64_t directory_entries, uint64_t bitmap_entries,
        uint64_t total_pages, uint64_t extension_phys,
        uint64_t extension_pages) {
    uint64_t extension_bytes;

    memset(pfa_inline_directory, 0, sizeof(pfa_inline_directory));
    pfa_directory_entries = directory_entries;
    pfa_directory_extension = NULL;
    extension_bytes = extension_pages * PAGE_SIZE;
    if (extension_phys != 0 && extension_bytes != 0) {
        pfa_directory_extension =
            (uint32_t *)(extension_phys + KERNEL_VMA);
        memset(pfa_directory_extension, 0, extension_bytes);
    }
    pfa_bitmap = (uint8_t *)(void *)pfa_inline_directory;
    bitmap_entries_used = bitmap_entries;
    bitmap_leaf_pages = 0;
    total_pages_managed = total_pages;
    kernel_reserved_frames = 0;
    low_page_limit = 0x00800000;
}

void KERNEL_EARLY_INIT pfa_init_ram_stats(uint64_t total_kb,
                                           uint64_t usable_kb,
                                           uint64_t init_free_frames) {
    system_total_ram_kb = total_kb;
    system_usable_ram_kb = usable_kb;
    initial_free_frames = init_free_frames;
    pfa_cached_free = count_free_frames();
}

static void refht_lock_acquire(uint64_t *eflags_out) {
    uint64_t ef;

    __asm__ volatile("pushf; pop %0; cli" : "=r"(ef));
    while (__sync_lock_test_and_set(&refht_lock_val, 1)) {
        __asm__ volatile("pause");
    }
    *eflags_out = ef;
}

static void refht_lock_release(uint64_t eflags) {
    __sync_lock_release(&refht_lock_val);
    if (eflags & 0x200)
        __asm__ volatile("sti");
}

static void refht_init(void) {
    if (refht_initialized) return;
    refht_head = NULL;
    refht_deferred_free = NULL;
    refht_active_node_count = 0;
    refht_deferred_free_count = 0;
    refht_initialized = 1;
}

static refht_node_t *refht_alloc_node(void) {
    refht_node_t *n;

    n = (refht_node_t *)kmalloc(sizeof(refht_node_t));
    if (n) {
        n->page_idx = 0;
        n->refcount = 0;
        n->next = NULL;
    }
    return n;
}

void pfa_ref_gc(void) {
    refht_node_t *list;
    refht_node_t *next;
    uint64_t eflags;

    if (!refht_initialized) return;
    refht_lock_acquire(&eflags);
    list = refht_deferred_free;
    refht_deferred_free = NULL;
    refht_deferred_free_count = 0;
    refht_lock_release(eflags);
    while (list) {
        next = list->next;
        kfree(list);
        list = next;
    }
}

uint64_t pfa_ref_active_nodes(void) {
    uint64_t result;
    uint64_t eflags;

    result = 0;
    if (!refht_initialized) return 0;
    refht_lock_acquire(&eflags);
    result = refht_active_node_count;
    refht_lock_release(eflags);
    return result;
}

uint64_t pfa_ref_free_nodes(void) {
    uint64_t result;
    uint64_t eflags;

    result = 0;
    if (!refht_initialized) return 0;
    refht_lock_acquire(&eflags);
    result = refht_deferred_free_count;
    refht_lock_release(eflags);
    return result;
}

static refht_node_t *refht_find(uint64_t page_idx) {
    refht_node_t *n;

    n = refht_head;
    while (n) {
        if (n->page_idx == page_idx) return n;
        n = n->next;
    }
    return NULL;
}

static int refht_add(uint64_t phys_addr, uint8_t initial) {
    uint64_t idx;
    uint64_t eflags;
    refht_node_t *n;
    int result;

    if (!refht_initialized) refht_init();
    if (!refht_initialized) return -1;
    pfa_refcount_entries = total_pages_managed;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages_managed) return -1;
    result = -1;
    refht_lock_acquire(&eflags);
    n = refht_find(idx);
    if (n) {
        if (n->refcount < 255) {
            n->refcount++;
            result = 0;
        }
    } else {
        n = refht_alloc_node();
        if (n) {
            n->page_idx = idx;
            n->refcount = initial;
            n->next = refht_head;
            refht_head = n;
            refht_active_node_count++;
            result = 0;
        }
    }
    refht_lock_release(eflags);
    return result;
}

static int refht_release(uint64_t phys_addr, int cow, int *free_page) {
    uint64_t idx;
    uint64_t eflags;
    refht_node_t *n;
    refht_node_t *prev;
    int result;
    int remove;

    if (free_page) *free_page = 0;
    if (!refht_initialized) {
        if (free_page && cow) *free_page = 1;
        return 0;
    }
    idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages_managed) return 0;
    result = 0;
    refht_lock_acquire(&eflags);
    prev = NULL;
    n = refht_head;
    while (n && n->page_idx != idx) {
        prev = n;
        n = n->next;
    }
    if (!n) {
        refht_lock_release(eflags);
        if (free_page && cow) *free_page = 1;
        return 0;
    }
    remove = 0;
    if (cow) {
        if (n->refcount > 2) {
            n->refcount--;
            result = n->refcount;
        } else {
            if (free_page) *free_page = n->refcount == 1;
            remove = 1;
        }
    } else {
        if (n->refcount > 0) n->refcount--;
        result = n->refcount;
        remove = n->refcount == 0;
    }
    if (remove) {
        if (prev)
            prev->next = n->next;
        else
            refht_head = n->next;
        if (refht_active_node_count > 0) refht_active_node_count--;
        n->next = refht_deferred_free;
        refht_deferred_free = n;
        refht_deferred_free_count++;
    }
    refht_lock_release(eflags);
    return result;
}

void pfa_ref_init(void) {
    pfa_refcount_entries = total_pages_managed;
    refht_init();
}

void pfa_ref_inc(uint64_t phys_addr) {
    refht_add(phys_addr, 1);
}

int pfa_ref_share(uint64_t phys_addr) {
    return refht_add(phys_addr, 2);
}

int pfa_ref_dec(uint64_t phys_addr) {
    return refht_release(phys_addr, 0, NULL);
}

uint8_t pfa_ref_get(uint64_t phys_addr) {
    uint64_t idx;
    uint64_t eflags;
    refht_node_t *n;
    uint8_t result;

    if (!refht_initialized) return 0;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages_managed) return 0;
    refht_lock_acquire(&eflags);
    n = refht_find(idx);
    result = n ? n->refcount : 0;
    refht_lock_release(eflags);
    return result;
}

void pfa_cow_release(uint64_t phys_addr) {
    int free_page;

    if (!phys_addr) return;
    free_page = 0;
    refht_release(phys_addr, 1, &free_page);
    if (free_page) pfa_free(phys_addr);
}

void pfa_cow_release64(uint64_t phys_addr) {
    pfa_cow_release((uint64_t)phys_addr);
}

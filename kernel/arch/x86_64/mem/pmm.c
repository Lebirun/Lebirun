#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <string.h>

extern uint64_t total_pages_managed;
extern uint64_t kernel_reserved_frames;

uint8_t *pfa_bitmap = 0;
static uint64_t bitmap_bytes_used = 0;

static volatile int pfa_lock = 0;

static uint64_t last_alloc_hint = 0;

static volatile uint64_t pfa_cached_free = 0;

static uint64_t pfa_refcount_entries = 0;

#define REFHT_BUCKETS 128

typedef struct refht_node {
    uint64_t page_idx;
    uint8_t refcount;
    struct refht_node *next;
} refht_node_t;

static refht_node_t **refht_buckets;
static refht_node_t *refht_free_list;
static uint64_t refht_active_node_count = 0;
static uint64_t refht_free_node_count = 0;
static int refht_initialized = 0;
static volatile int refht_lock_val = 0;

static int low_exhausted = 0;
static uint64_t low_page_limit = 0x00400000;

extern mem_region_t memory_map[MAX_REGIONS];
extern uint64_t num_regions;
extern uint64_t bump_current;
extern uint64_t active_region;
extern uint64_t low_bump;

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);

static inline void pfa_lock_acquire(uint64_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&pfa_lock, 1)) {

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

void set_bit(uint64_t bit_idx) {
    if (bit_idx / 8 < bitmap_bytes_used)
        pfa_bitmap[bit_idx / 8] |= (1 << (bit_idx % 8));
}

void clear_bit(uint64_t bit_idx) {
    if (bit_idx / 8 < bitmap_bytes_used)
        pfa_bitmap[bit_idx / 8] &= ~(1 << (bit_idx % 8));
}

bool test_bit(uint64_t bit_idx) {
    if (bit_idx / 8 >= bitmap_bytes_used) return true;
    return pfa_bitmap[bit_idx / 8] & (1 << (bit_idx % 8));
}

uint64_t count_free_frames(void) {
    uint64_t count;
    uint64_t i;
    uint64_t bytes;
    uint8_t free_bits;

    count = 0;
    bytes = (total_pages_managed + 7) / 8;
    for (i = 0; i < bytes; i++) {
        free_bits = (uint8_t)~pfa_bitmap[i];
        while (free_bits != 0) {
            free_bits &= (uint8_t)(free_bits - 1);
            count++;
        }
    }

    return count;
}

static uint64_t find_free_frames_range(uint64_t from, uint64_t to, uint64_t num) {
    uint64_t b;
    uint64_t bit;
    uint64_t frame_idx;
    uint64_t j;
    uint64_t run;
    uint64_t run_start;
    uint64_t b_start;
    uint64_t b_end;

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

    pfa_lock_release(eflags);
    return result;
}

uint64_t pfa_alloc(void) {
    uint64_t i;
    uint64_t byte_idx;
    uint64_t bit_idx;
    uint64_t max_bytes;
    uint64_t eflags;
    uint64_t result;

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

void pfa_free(uint64_t phys_addr) {
    uint64_t idx;
    uint64_t byte_idx;
    uint64_t bit_idx;
    uint64_t eflags;
    uint8_t cur_ref;

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

    byte_idx = (uint64_t)(idx / 8);
    bit_idx = (uint64_t)(idx % 8);
    
    pfa_lock_acquire(&eflags);
    if (pfa_bitmap[byte_idx] & (1 << bit_idx)) {
        pfa_bitmap[byte_idx] &= ~(1 << bit_idx);
        __sync_fetch_and_add(&pfa_cached_free, 1);
    }
    pfa_lock_release(eflags);
}

void pfa_reclaim_kernel_range(uint64_t phys_start, uint64_t phys_end) {
    uint64_t eflags;
    uint64_t start_frame;
    uint64_t end_frame;
    uint64_t f;
    uint64_t count;

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

uint64_t pfa_alloc_contiguous(uint64_t num_frames) {
    if (num_frames == 0) return 0;
    return find_free_frames(num_frames);
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
        if (idx < kernel_reserved_frames) continue;
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

void pfa_sync_free_count(void) {
    pfa_cached_free = count_free_frames();
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
    return bitmap_alloc_kb;
}

void pfa_set_reserved_stats(uint64_t kern_bin_kb, uint64_t bmp_kb) {
    kernel_binary_kb = kern_bin_kb;
    bitmap_alloc_kb = bmp_kb;
}

void *pmm_alloc_page(void) {
    uint64_t eflags;
    uint64_t i;
    uint64_t region_end;
    uint64_t alloc_start;
    uint64_t idx_alloc;
    uint64_t bit;
    uint64_t frame_idx;
    uint64_t scan_start;
    uint64_t scan_end;
    uint64_t b;

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
                        return (void *)(uint64_t)alloc_start;
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

void *pmm_alloc_pages(uint64_t num) {
    if (num == 0) return NULL;
    uint64_t addr = find_free_frames(num);
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
    volatile uint64_t *w;
    uint64_t i;
    uint64_t saved_flags;
    extern uint64_t pt_temp_pt_ready_check(void);

    if (!pt_temp_pt_ready_check()) {
        if (phys_addr < low_page_limit) {
            w = (volatile uint64_t *)(phys_addr + KERNEL_VMA);
            for (i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
                w[i] = 0;
            }
        }
        return;
    }

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(6);

    temp_map_raw(temp_virt, phys_addr);

    w = (volatile uint64_t *)temp_virt;
    for (i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        w[i] = 0;
    }

    temp_unmap_raw(temp_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

void pfa_init_internal_setup(uint64_t bitmap_bytes, uint64_t total_pages, uint64_t kernel_frames) {
    bitmap_bytes_used = bitmap_bytes;
    total_pages_managed = total_pages;
    kernel_reserved_frames = kernel_frames;
    low_page_limit = 0x00800000;
}

void pfa_init_ram_stats(uint64_t total_kb, uint64_t usable_kb, uint64_t init_free_frames) {
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
    refht_node_t **buckets;

    if (refht_initialized) return;
    buckets = (refht_node_t **)kmalloc(REFHT_BUCKETS * sizeof(refht_node_t *));
    if (!buckets) return;
    memset(buckets, 0, REFHT_BUCKETS * sizeof(refht_node_t *));
    refht_buckets = buckets;
    refht_free_list = NULL;
    refht_active_node_count = 0;
    refht_free_node_count = 0;
    refht_initialized = 1;
}

static refht_node_t *refht_alloc_node(void) {
    refht_node_t *n;

    n = refht_free_list;
    if (n) {
        refht_free_list = n->next;
        if (refht_free_node_count > 0) refht_free_node_count--;
        n->next = NULL;
        return n;
    }
    n = (refht_node_t *)kmalloc(sizeof(refht_node_t));
    if (n) {
        n->page_idx = 0;
        n->refcount = 0;
        n->next = NULL;
    }
    return n;
}

static void refht_free_node(refht_node_t *n) {
    n->next = refht_free_list;
    refht_free_list = n;
    refht_free_node_count++;
}

void pfa_ref_gc(void) {
    refht_node_t *list;
    refht_node_t *next;
    refht_node_t **buckets;
    uint64_t eflags;

    if (!refht_initialized) return;
    buckets = NULL;
    refht_lock_acquire(&eflags);
    list = refht_free_list;
    refht_free_list = NULL;
    refht_free_node_count = 0;
    if (refht_active_node_count == 0) {
        buckets = refht_buckets;
        refht_buckets = NULL;
        refht_initialized = 0;
    }
    refht_lock_release(eflags);

    while (list) {
        next = list->next;
        kfree(list);
        list = next;
    }
    if (buckets) kfree(buckets);
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
    result = refht_free_node_count;
    refht_lock_release(eflags);
    return result;
}

static refht_node_t *refht_find(uint64_t page_idx, uint64_t bucket) {
    refht_node_t *n;

    n = refht_buckets[bucket];
    while (n) {
        if (n->page_idx == page_idx) return n;
        n = n->next;
    }
    return NULL;
}

static int refht_add(uint64_t phys_addr, uint8_t initial) {
    uint64_t idx;
    uint64_t bucket;
    uint64_t eflags;
    refht_node_t *n;
    int result;

    if (!refht_initialized) refht_init();
    if (!refht_initialized) return -1;
    pfa_refcount_entries = total_pages_managed;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages_managed) return -1;
    bucket = idx % REFHT_BUCKETS;
    result = -1;
    refht_lock_acquire(&eflags);
    n = refht_find(idx, bucket);
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
            n->next = refht_buckets[bucket];
            refht_buckets[bucket] = n;
            refht_active_node_count++;
            result = 0;
        }
    }
    refht_lock_release(eflags);
    return result;
}

static int refht_release(uint64_t phys_addr, int cow, int *free_page) {
    uint64_t idx;
    uint64_t bucket;
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
    bucket = idx % REFHT_BUCKETS;
    result = 0;
    refht_lock_acquire(&eflags);
    prev = NULL;
    n = refht_buckets[bucket];
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
            refht_buckets[bucket] = n->next;
        if (refht_active_node_count > 0) refht_active_node_count--;
        refht_free_node(n);
    }
    refht_lock_release(eflags);
    return result;
}

void pfa_ref_init(void) {
    pfa_refcount_entries = total_pages_managed;
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
    uint64_t bucket;
    uint64_t eflags;
    refht_node_t *n;
    uint8_t result;

    if (!refht_initialized) return 0;
    idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages_managed) return 0;
    bucket = idx % REFHT_BUCKETS;
    refht_lock_acquire(&eflags);
    n = refht_find(idx, bucket);
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

#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/debug.h>
#include <string.h>

#define SLAB_REGION_START 0xFFFFFFFFD4000000ULL
#define SLAB_REGION_SIZE  0x00400000ULL
#define SLAB_REGION_MAX_PAGES (SLAB_REGION_SIZE / PAGE_SIZE)
#define SLAB_SIZES_COUNT 3
#define SLAB_SIZE_32    0
#define SLAB_SIZE_64    1
#define SLAB_SIZE_128   2

#define SLAB_MAGIC 0x534C4142

static const uint64_t slab_sizes[SLAB_SIZES_COUNT] = { 32, 64, 128 };

typedef struct slab_page {
    uint64_t magic;
    uint64_t obj_size;
    uint64_t obj_count;
    uint64_t free_count;
    struct slab_page *next;
    struct slab_page *prev;
    uint64_t free_bitmap[4];
} slab_page_t;

typedef struct {
    uint64_t obj_size;
    slab_page_t *partial;
    slab_page_t *full;
    slab_page_t *empty;
    uint64_t pages_allocated;
    uint64_t alloc_count;
    uint64_t free_count;
} slab_cache_t;

static slab_cache_t slab_caches[SLAB_SIZES_COUNT];
static int slab_initialized = 0;
static volatile int slab_lock = 0;

static uint64_t slab_virt_bump = SLAB_REGION_START;
static uint64_t slab_virt_scan = SLAB_REGION_START;

static inline void slab_lock_acquire(uint64_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&slab_lock, 1)) {
    }
}

static inline void slab_lock_release(uint64_t eflags) {
    __sync_lock_release(&slab_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static int slab_size_index(size_t size) {
    int i;
    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        if (size <= slab_sizes[i]) return i;
    }
    return -1;
}

static int slab_bitmap_test(slab_page_t *page, uint64_t idx) {
    uint64_t word;
    uint64_t bit;

    word = idx >> 6;
    bit = idx & 63;
    if (word >= 4) return 0;
    return (page->free_bitmap[word] & (1ULL << bit)) != 0;
}

static void slab_bitmap_set(slab_page_t *page, uint64_t idx) {
    uint64_t word;
    uint64_t bit;

    word = idx >> 6;
    bit = idx & 63;
    if (word >= 4) return;
    page->free_bitmap[word] |= (1ULL << bit);
}

static void slab_bitmap_clear(slab_page_t *page, uint64_t idx) {
    uint64_t word;
    uint64_t bit;

    word = idx >> 6;
    bit = idx & 63;
    if (word >= 4) return;
    page->free_bitmap[word] &= ~(1ULL << bit);
}

static uint64_t slab_obj_index(slab_page_t *page, void *ptr) {
    uint64_t addr;
    uint64_t first;

    addr = (uint64_t)ptr;
    first = (uint64_t)page + sizeof(slab_page_t);
    return (addr - first) / page->obj_size;
}

static void *slab_obj_at(slab_page_t *page, uint64_t idx) {
    return (void *)((uint8_t *)page + sizeof(slab_page_t) + idx * page->obj_size);
}

static int slab_find_free_index(slab_page_t *page, uint64_t *out_idx) {
    uint64_t i;

    if (!page || !out_idx) return 0;
    for (i = 0; i < page->obj_count; i++) {
        if (slab_bitmap_test(page, i)) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

static void slab_page_init(slab_page_t *page, uint64_t obj_size) {
    uint64_t usable_size;
    uint64_t num_objs;
    uint64_t i;
    
    page->magic = SLAB_MAGIC;
    page->obj_size = obj_size;
    usable_size = PAGE_SIZE - sizeof(slab_page_t);
    num_objs = usable_size / obj_size;
    page->obj_count = num_objs;
    page->free_count = num_objs;
    page->next = NULL;
    page->prev = NULL;
    for (i = 0; i < 4; i++) {
        page->free_bitmap[i] = 0;
    }
    
    for (i = 0; i < num_objs; i++) {
        slab_bitmap_set(page, i);
    }
}

static uint64_t slab_virt_alloc(void) {
    uint64_t v;
    uint64_t i;

    if (slab_virt_bump < SLAB_REGION_START + SLAB_REGION_SIZE) {
        v = slab_virt_bump;
        slab_virt_bump += PAGE_SIZE;
        return v;
    }
    for (i = 0; i < SLAB_REGION_MAX_PAGES; i++) {
        v = slab_virt_scan;
        slab_virt_scan += PAGE_SIZE;
        if (slab_virt_scan >= SLAB_REGION_START + SLAB_REGION_SIZE) {
            slab_virt_scan = SLAB_REGION_START;
        }
        if (!vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), v)) return v;
    }
    return 0;
}

static uint64_t slab_virt_to_phys(uint64_t virt) {
    return vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), virt);
}

static int slab_page_mapped(slab_page_t *page) {
    uint64_t virt;

    virt = (uint64_t)page;
    if (virt < SLAB_REGION_START || virt >= SLAB_REGION_START + SLAB_REGION_SIZE) return 0;
    return slab_virt_to_phys(virt) != 0;
}

static int slab_obj_valid(slab_page_t *page, void *ptr) {
    uint64_t addr;
    uint64_t first;
    uint64_t last;
    uint64_t off;

    if (!page || !ptr) return 0;
    addr = (uint64_t)ptr;
    first = (uint64_t)page + sizeof(slab_page_t);
    last = (uint64_t)page + PAGE_SIZE;
    if (addr < first || addr >= last) return 0;
    off = addr - first;
    if (page->obj_size == 0) return 0;
    if ((off % page->obj_size) != 0) return 0;
    if ((off / page->obj_size) >= page->obj_count) return 0;
    return 1;
}

static slab_page_t *slab_alloc_page(uint64_t obj_size) {
    slab_page_t *page;
    void *phys;
    uint64_t virt;
    
    phys = pmm_alloc_page();
    if (!phys) return NULL;
    
    virt = slab_virt_alloc();
    if (!virt) {
        pfa_free((uint64_t)phys);
        return NULL;
    }

    vmm_map_page(virt, (uint64_t)phys, 3);
    page = (slab_page_t *)virt;
    
    memset(page, 0, PAGE_SIZE);
    slab_page_init(page, obj_size);
    
    return page;
}

static void slab_free_page(slab_page_t *page) {
    uint64_t virt;
    uint64_t phys;
    
    virt = (uint64_t)page;
    phys = slab_virt_to_phys(virt);
    vmm_unmap_page(virt);
    if (phys) {
        pfa_free(phys);
    }
}

static void slab_add_to_list(slab_page_t **list, slab_page_t *page) {
    page->prev = NULL;
    page->next = *list;
    if (*list) {
        (*list)->prev = page;
    }
    *list = page;
}

static void slab_remove_from_list(slab_page_t **list, slab_page_t *page) {
    if (page->prev) {
        page->prev->next = page->next;
    } else {
        *list = page->next;
    }
    if (page->next) {
        page->next->prev = page->prev;
    }
    page->prev = NULL;
    page->next = NULL;
}

void slab_init(void) {
    int i;

    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        slab_caches[i].obj_size = slab_sizes[i];
        slab_caches[i].partial = NULL;
        slab_caches[i].full = NULL;
        slab_caches[i].empty = NULL;
        slab_caches[i].pages_allocated = 0;
        slab_caches[i].alloc_count = 0;
        slab_caches[i].free_count = 0;
    }
    slab_initialized = 1;
    printf("Slab allocator initialized (sizes: 32, 64, 128)\n");
}

void *slab_alloc(size_t size, void *caller) {
    int idx;
    slab_cache_t *cache;
    slab_page_t *page;
    uint64_t eflags;
    void *result;
    uint64_t obj_idx;
    int retried;
    
    if (!slab_initialized) return NULL;
    
    idx = slab_size_index(size);
    if (idx < 0) return NULL;
    
    slab_lock_acquire(&eflags);
    
    cache = &slab_caches[idx];
    retried = 0;

retry:
    page = cache->partial;
    
    if (!page) {
        page = cache->empty;
        if (page) {
            slab_remove_from_list(&cache->empty, page);
            slab_add_to_list(&cache->partial, page);
        }
    }
    
    if (!page) {
        page = slab_alloc_page(cache->obj_size);
        if (!page) {
            slab_lock_release(eflags);
            return NULL;
        }
        cache->pages_allocated++;
        slab_add_to_list(&cache->partial, page);
    }
    
    if (!slab_find_free_index(page, &obj_idx)) {
        printf("slab_alloc: corrupt bitmap page=%p size=%u caller=%p\n",
               page, cache->obj_size, caller);
        page->free_count = 0;
        slab_remove_from_list(&cache->partial, page);
        slab_add_to_list(&cache->full, page);
        if (!retried) {
            retried = 1;
            goto retry;
        }
        slab_lock_release(eflags);
        return NULL;
    }
    slab_bitmap_clear(page, obj_idx);
    page->free_count--;
    cache->alloc_count++;
    
    if (page->free_count == 0) {
        slab_remove_from_list(&cache->partial, page);
        slab_add_to_list(&cache->full, page);
    }
    
    result = slab_obj_at(page, obj_idx);
    slab_lock_release(eflags);
    
    return result;
}

void slab_free(void *ptr, void *caller) {
    slab_page_t *page;
    slab_cache_t *cache;
    int idx;
    int was_full;
    uint64_t eflags;
    uint64_t obj_idx;
    
    if (!ptr || !slab_initialized) return;
    
    page = (slab_page_t *)((uint64_t)ptr & ~(PAGE_SIZE - 1));
    
    if (!slab_page_mapped(page)) return;
    if (page->magic != SLAB_MAGIC) return;
    
    slab_lock_acquire(&eflags);
    
    idx = slab_size_index(page->obj_size);
    if (idx < 0) {
        slab_lock_release(eflags);
        return;
    }
    
    cache = &slab_caches[idx];
    was_full = (page->free_count == 0);
    
    if (!slab_obj_valid(page, ptr)) {
        printf("slab_free: invalid object ptr=%p page=%p size=%u caller=%p\n",
               ptr, page, page->obj_size, caller);
        slab_lock_release(eflags);
        return;
    }
    obj_idx = slab_obj_index(page, ptr);
    if (slab_bitmap_test(page, obj_idx)) {
        printf("slab_free: double free ptr=%p page=%p size=%u caller=%p\n",
               ptr, page, page->obj_size, caller);
        slab_lock_release(eflags);
        return;
    }
    slab_bitmap_set(page, obj_idx);
    page->free_count++;
    cache->free_count++;
    
    if (was_full) {
        slab_remove_from_list(&cache->full, page);
        slab_add_to_list(&cache->partial, page);
    } else if (page->free_count == page->obj_count) {
        slab_remove_from_list(&cache->partial, page);
        slab_free_page(page);
        cache->pages_allocated--;
    }

    slab_lock_release(eflags);
}

void slab_gc(void) {
    int i;
    slab_cache_t *cache;
    slab_page_t *page;
    slab_page_t *next;
    uint64_t eflags;

    if (!slab_initialized) return;

    slab_lock_acquire(&eflags);
    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        cache = &slab_caches[i];
        page = cache->empty;
        while (page) {
            next = page->next;
            slab_remove_from_list(&cache->empty, page);
            slab_free_page(page);
            cache->pages_allocated--;
            page = next;
        }
    }
    slab_lock_release(eflags);
}

void slab_reclaim_empty(void) {
    slab_gc();
}

int slab_owns(void *ptr) {
    slab_page_t *page;
    uint64_t virt;
    
    if (!ptr || !slab_initialized) return 0;
    
    virt = (uint64_t)ptr;
    if (virt < SLAB_REGION_START || virt >= SLAB_REGION_START + SLAB_REGION_SIZE) return 0;
    
    page = (slab_page_t *)((uint64_t)ptr & ~(PAGE_SIZE - 1));
    if (!slab_page_mapped(page)) return 0;
    return (page->magic == SLAB_MAGIC);
}

size_t slab_max_size(void) {
    return slab_sizes[SLAB_SIZES_COUNT - 1];
}

size_t slab_alloc_size(void *ptr) {
    slab_page_t *page;

    if (!ptr || !slab_initialized) return 0;
    page = (slab_page_t *)((uint64_t)ptr & ~(PAGE_SIZE - 1));
    if (!slab_page_mapped(page)) return 0;
    if (page->magic != SLAB_MAGIC) return 0;
    return page->obj_size;
}

uint64_t slab_get_total_pages(void) {
    int i;
    uint64_t total;

    total = 0;
    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        total += slab_caches[i].pages_allocated;
    }
    return total;
}

void slab_stats(void) {
    int i;
    slab_cache_t *cache;
    
    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        cache = &slab_caches[i];
        printf("Size %3u: pages=%u allocs=%u frees=%u\n",
               cache->obj_size, cache->pages_allocated,
               cache->alloc_count, cache->free_count);
    }
}

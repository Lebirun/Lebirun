#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

#define SLAB_SIZES_COUNT 5
#define SLAB_SIZE_16    0
#define SLAB_SIZE_32    1
#define SLAB_SIZE_64    2
#define SLAB_SIZE_128   3
#define SLAB_SIZE_256   4

#define SLAB_MAGIC 0x534C4142

static const uint32_t slab_sizes[SLAB_SIZES_COUNT] = { 16, 32, 64, 128, 256 };

typedef struct slab_obj {
    struct slab_obj *next;
} slab_obj_t;

typedef struct slab_page {
    uint32_t magic;
    uint32_t obj_size;
    uint32_t obj_count;
    uint32_t free_count;
    slab_obj_t *free_list;
    struct slab_page *next;
    struct slab_page *prev;
} slab_page_t;

typedef struct {
    uint32_t obj_size;
    slab_page_t *partial;
    slab_page_t *full;
    slab_page_t *empty;
    uint32_t pages_allocated;
    uint32_t alloc_count;
    uint32_t free_count;
} slab_cache_t;

static slab_cache_t slab_caches[SLAB_SIZES_COUNT];
static int slab_initialized = 0;
static volatile int slab_lock = 0;

static inline void slab_lock_acquire(uint32_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&slab_lock, 1)) {
    }
}

static inline void slab_lock_release(uint32_t eflags) {
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

static void slab_page_init(slab_page_t *page, uint32_t obj_size) {
    uint32_t usable_size;
    uint32_t num_objs;
    uint8_t *obj_start;
    uint32_t i;
    slab_obj_t *obj;
    
    page->magic = SLAB_MAGIC;
    page->obj_size = obj_size;
    usable_size = PAGE_SIZE - sizeof(slab_page_t);
    num_objs = usable_size / obj_size;
    page->obj_count = num_objs;
    page->free_count = num_objs;
    page->next = NULL;
    page->prev = NULL;
    page->free_list = NULL;
    
    obj_start = (uint8_t *)page + sizeof(slab_page_t);
    for (i = 0; i < num_objs; i++) {
        obj = (slab_obj_t *)(obj_start + i * obj_size);
        obj->next = page->free_list;
        page->free_list = obj;
    }
}

static slab_page_t *slab_alloc_page(uint32_t obj_size) {
    slab_page_t *page;
    void *phys;
    
    phys = pmm_alloc_page();
    if (!phys) return NULL;
    
    vmm_map_page((uint32_t)phys + 0xC0000000, (uint32_t)phys, 3);
    page = (slab_page_t *)((uint32_t)phys + 0xC0000000);
    
    memset(page, 0, PAGE_SIZE);
    slab_page_init(page, obj_size);
    
    return page;
}

static void slab_free_page(slab_page_t *page) {
    uint32_t virt;
    uint32_t phys;
    
    virt = (uint32_t)page;
    phys = virt - 0xC0000000;
    vmm_unmap_page(virt);
    pfa_free(phys);
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
    printf("Slab allocator initialized (sizes: 16, 32, 64, 128, 256)\n");
}

void *slab_alloc(size_t size) {
    int idx;
    slab_cache_t *cache;
    slab_page_t *page;
    slab_obj_t *obj;
    uint32_t eflags;
    void *result;
    
    if (!slab_initialized) return NULL;
    
    idx = slab_size_index(size);
    if (idx < 0) return NULL;
    
    slab_lock_acquire(&eflags);
    
    cache = &slab_caches[idx];
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
    
    obj = page->free_list;
    page->free_list = obj->next;
    page->free_count--;
    cache->alloc_count++;
    
    if (page->free_count == 0) {
        slab_remove_from_list(&cache->partial, page);
        slab_add_to_list(&cache->full, page);
    }
    
    result = (void *)obj;
    slab_lock_release(eflags);
    
    return result;
}

void slab_free(void *ptr) {
    slab_page_t *page;
    slab_cache_t *cache;
    slab_obj_t *obj;
    int idx;
    int was_full;
    uint32_t eflags;
    
    if (!ptr || !slab_initialized) return;
    
    page = (slab_page_t *)((uint32_t)ptr & ~(PAGE_SIZE - 1));
    
    if (page->magic != SLAB_MAGIC) return;
    
    slab_lock_acquire(&eflags);
    
    idx = slab_size_index(page->obj_size);
    if (idx < 0) {
        slab_lock_release(eflags);
        return;
    }
    
    cache = &slab_caches[idx];
    was_full = (page->free_count == 0);
    
    obj = (slab_obj_t *)ptr;
    obj->next = page->free_list;
    page->free_list = obj;
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
    uint32_t eflags;

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

int slab_owns(void *ptr) {
    slab_page_t *page;
    uint32_t virt;
    
    if (!ptr || !slab_initialized) return 0;
    
    virt = (uint32_t)ptr;
    if (virt < 0xC0000000) return 0;
    
    page = (slab_page_t *)((uint32_t)ptr & ~(PAGE_SIZE - 1));
    return (page->magic == SLAB_MAGIC);
}

size_t slab_max_size(void) {
    return slab_sizes[SLAB_SIZES_COUNT - 1];
}

uint32_t slab_get_total_pages(void) {
    int i;
    uint32_t total;

    total = 0;
    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        total += slab_caches[i].pages_allocated;
    }
    return total;
}

void slab_stats(void) {
    int i;
    slab_cache_t *cache;
    
    printf("=== Slab Allocator Stats ===\n");
    for (i = 0; i < SLAB_SIZES_COUNT; i++) {
        cache = &slab_caches[i];
        printf("Size %3u: pages=%u allocs=%u frees=%u\n",
               cache->obj_size, cache->pages_allocated,
               cache->alloc_count, cache->free_count);
    }
    printf("============================\n");
}

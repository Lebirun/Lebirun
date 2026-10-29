#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <string.h>

#define SLAB_PRIMARY_START  0xFFFFFFFFD8002000ULL
#define SLAB_PRIMARY_SIZE   0x000FE000ULL
#define SLAB_OVERFLOW_START HEAP_VIRTUAL_END
#define SLAB_OVERFLOW_SIZE  (0xFFFFFFFFD8000000ULL - SLAB_OVERFLOW_START)
#define SLAB_MAX_SIZE       65536

#define SLAB_MAGIC 0x534C4142u
#define SLAB_KIND_SMALL 1
#define SLAB_KIND_EXTENT 2
#define SLAB_EXTENT_HEAD_SIZE sizeof(uint64_t)
#define SLAB_EXTENT_TAIL_SIZE sizeof(uint32_t)

typedef struct slab_page {
    struct slab_page *next;
    struct slab_page *prev;
    uint32_t magic;
    uint32_t payload_size;
    uint16_t pages;
    uint16_t live_count;
    uint8_t kind;
    uint8_t reserved;
    uint16_t max_free;
} slab_page_t;

typedef struct {
    uint32_t span;
    uint32_t requested;
} slab_block_t;

#define SLAB_SMALL_MAX ((PAGE_SIZE - sizeof(slab_page_t) - \
                        sizeof(slab_block_t)) & ~7ULL)

static slab_page_t *slab_small_pages;
static slab_page_t *slab_small_tail;
static slab_page_t *slab_small_spare;
static uint64_t slab_pages_allocated;
static int slab_initialized;
static volatile int slab_lock;

static int slab_virtual_dirty;
static volatile int slab_flush_active;
static int slab_fresh_only;
static uint64_t slab_primary_bump = SLAB_PRIMARY_START;
static uint64_t slab_primary_scan = SLAB_PRIMARY_START;
static uint64_t slab_overflow_bump = SLAB_OVERFLOW_START;
static uint64_t slab_overflow_scan = SLAB_OVERFLOW_START;

int slab_allocator_locked(void) {
    return __atomic_load_n(&slab_lock, __ATOMIC_RELAXED) != 0;
}

static inline void slab_lock_acquire(uint64_t *eflags_out) {
    uint64_t eflags;

    for (;;) {
        __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
        if (__sync_lock_test_and_set(&slab_lock, 1) == 0) {
            *eflags_out = eflags;
            return;
        }
        if (eflags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        smp_tlb_flush_poll();
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void slab_lock_release(uint64_t eflags) {
    __sync_lock_release(&slab_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static int slab_region_contains(uint64_t virt, uint64_t start,
                                uint64_t size) {
    return virt >= start && virt < start + size;
}

static uint64_t slab_find_virtual_run(uint64_t start, uint64_t size,
                                      uint64_t *scan, uint64_t pages) {
    uint64_t limit;
    uint64_t candidate;
    uint64_t page;
    uint64_t checked;
    uint64_t total;
    int available;

    total = size / PAGE_SIZE;
    if (pages == 0 || pages > total) return 0;
    if (slab_virtual_dirty || slab_flush_active) return 0;
    limit = start + size;
    candidate = *scan;
    checked = 0;
    while (checked < total) {
        if (candidate < start || candidate + pages * PAGE_SIZE > limit)
            candidate = start;
        available = 1;
        for (page = 0; page < pages; page++) {
            if (vmm_get_phys_in_pml4(vmm_get_kernel_cr3(),
                                     candidate + page * PAGE_SIZE)) {
                available = 0;
                candidate += (page + 1) * PAGE_SIZE;
                checked += page + 1;
                break;
            }
        }
        if (available) {
            *scan = candidate + pages * PAGE_SIZE;
            if (*scan >= limit) *scan = start;
            return candidate;
        }
    }
    return 0;
}

static uint64_t slab_virtual_bump(uint64_t pages) {
    uint64_t bytes;
    uint64_t virt;

    if (pages == 0 || pages > UINT64_MAX / PAGE_SIZE) return 0;
    bytes = pages * PAGE_SIZE;
    if (slab_primary_bump <= SLAB_PRIMARY_START + SLAB_PRIMARY_SIZE &&
        bytes <= SLAB_PRIMARY_START + SLAB_PRIMARY_SIZE -
                 slab_primary_bump) {
        virt = slab_primary_bump;
        slab_primary_bump += bytes;
        return virt;
    }
    if (slab_overflow_bump <= SLAB_OVERFLOW_START + SLAB_OVERFLOW_SIZE &&
        bytes <= SLAB_OVERFLOW_START + SLAB_OVERFLOW_SIZE -
                 slab_overflow_bump) {
        virt = slab_overflow_bump;
        slab_overflow_bump += bytes;
        return virt;
    }
    return 0;
}

static uint64_t slab_virtual_alloc(uint64_t pages) {
    uint64_t virt;

    if (pages == 0 || pages > UINT64_MAX / PAGE_SIZE) return 0;
    if (slab_virtual_dirty || slab_flush_active)
        return slab_fresh_only ? slab_virtual_bump(pages) : 0;
    if (pages <= SLAB_PRIMARY_SIZE / PAGE_SIZE) {
        virt = slab_find_virtual_run(SLAB_PRIMARY_START,
                                     slab_primary_bump -
                                     SLAB_PRIMARY_START,
                                     &slab_primary_scan, pages);
        if (virt) return virt;
    }
    if (pages <= (slab_overflow_bump - SLAB_OVERFLOW_START) / PAGE_SIZE) {
        virt = slab_find_virtual_run(SLAB_OVERFLOW_START,
                                     slab_overflow_bump -
                                     SLAB_OVERFLOW_START,
                                     &slab_overflow_scan, pages);
        if (virt) return virt;
    }
    return slab_virtual_bump(pages);
}

static int slab_map_pages(uint64_t virt, uint64_t pages) {
    void *phys;
    uint64_t mapped;
    uint64_t address;

    mapped = 0;
    while (mapped < pages) {
        phys = pmm_alloc_page();
        if (!phys) break;
        address = virt + mapped * PAGE_SIZE;
        vmm_map_page(address, (uint64_t)phys, 3);
        if (vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), address) !=
            (uint64_t)phys) {
            pfa_free((uint64_t)phys);
            break;
        }
        mapped++;
    }
    if (mapped == pages) return 1;
    while (mapped > 0) {
        mapped--;
        address = virt + mapped * PAGE_SIZE;
        phys = (void *)vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), address);
        vmm_unmap_page(address);
        if (phys) pfa_free((uint64_t)phys);
    }
    slab_virtual_dirty = 1;
    return 0;
}

static void slab_unmap_pages(uint64_t virt, uint64_t pages) {
    uint64_t page;
    uint64_t address;
    uint64_t phys;

    for (page = 0; page < pages; page++) {
        address = virt + page * PAGE_SIZE;
        phys = vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), address);
        vmm_unmap_page(address);
        if (phys) pfa_free(phys);
    }
    slab_virtual_dirty = 1;
}

static void slab_small_remove(slab_page_t *page);

int slab_flush_stale(void) {
    uint64_t eflags;
    int result;

    if (__sync_lock_test_and_set(&slab_flush_active, 1)) return 1;
    slab_lock_acquire(&eflags);
    if (slab_small_spare) {
        slab_small_remove(slab_small_spare);
        slab_small_spare->magic = 0;
        slab_unmap_pages((uint64_t)slab_small_spare, 1);
        slab_pages_allocated--;
        slab_small_spare = NULL;
    }
    if (!slab_virtual_dirty) {
        slab_lock_release(eflags);
        __sync_lock_release(&slab_flush_active);
        return 0;
    }
    slab_virtual_dirty = 0;
    slab_lock_release(eflags);
    result = smp_tlb_flush_all_sync();
    slab_lock_acquire(&eflags);
    if (result < 0) slab_virtual_dirty = 1;
    slab_lock_release(eflags);
    __sync_lock_release(&slab_flush_active);
    return result;
}

static void slab_small_add(slab_page_t *page) {
    page->prev = slab_small_tail;
    page->next = NULL;
    if (slab_small_tail)
        slab_small_tail->next = page;
    else
        slab_small_pages = page;
    slab_small_tail = page;
}

static void slab_small_remove(slab_page_t *page) {
    if (page->prev)
        page->prev->next = page->next;
    else
        slab_small_pages = page->next;
    if (page->next)
        page->next->prev = page->prev;
    else
        slab_small_tail = page->prev;
    page->next = NULL;
    page->prev = NULL;
}

static slab_page_t *slab_small_create(int *backing_failed) {
    slab_page_t *page;
    slab_block_t *block;
    uint64_t virt;

    virt = slab_virtual_alloc(1);
    if (!virt) return NULL;
    if (!slab_map_pages(virt, 1)) {
        *backing_failed = 1;
        return NULL;
    }
    memset((void *)virt, 0, PAGE_SIZE);
    page = (slab_page_t *)virt;
    page->magic = SLAB_MAGIC;
    page->pages = 1;
    page->kind = SLAB_KIND_SMALL;
    block = (slab_block_t *)(virt + sizeof(slab_page_t));
    block->span = PAGE_SIZE - sizeof(slab_page_t);
    block->requested = 0;
    page->max_free = block->span;
    slab_small_add(page);
    slab_pages_allocated++;
    return page;
}

static slab_block_t *slab_small_find(slab_page_t *page, uint32_t needed) {
    slab_block_t *block;
    uint64_t address;
    uint64_t end;

    address = (uint64_t)page + sizeof(slab_page_t);
    end = (uint64_t)page + PAGE_SIZE;
    while (address + sizeof(slab_block_t) <= end) {
        block = (slab_block_t *)address;
        if (block->span < sizeof(slab_block_t) ||
            block->span > end - address) return NULL;
        if (!block->requested && block->span >= needed) return block;
        address += block->span;
    }
    return NULL;
}

static void slab_small_coalesce(slab_page_t *page);

static void *slab_small_allocate(size_t size, int *backing_failed) {
    slab_page_t *page;
    slab_block_t *block;
    slab_block_t *remainder;
    uint32_t payload;
    uint32_t needed;
    uint32_t remaining;

    payload = ((uint32_t)size + 7u) & ~7u;
    needed = payload + sizeof(slab_block_t);
    page = slab_small_pages;
    block = NULL;
    while (page) {
        block = page->max_free >= needed ? slab_small_find(page, needed) : NULL;
        if (block) break;
        page = page->next;
    }
    if (!block) {
        page = slab_small_create(backing_failed);
        if (!page) return NULL;
        block = slab_small_find(page, needed);
        if (!block) return NULL;
    }
    remaining = block->span - needed;
    if (remaining >= sizeof(slab_block_t) + 8) {
        remainder = (slab_block_t *)((uint8_t *)block + needed);
        remainder->span = remaining;
        remainder->requested = 0;
        block->span = needed;
    }
    block->requested = (uint32_t)size;
    page->live_count++;
    page->payload_size += size;
    if (page == slab_small_spare) slab_small_spare = NULL;
    slab_small_coalesce(page);
    return (uint8_t *)block + sizeof(slab_block_t);
}

static void *slab_extent_allocate(size_t size, int *backing_failed) {
    slab_page_t *extent;
    uint32_t *head;
    uint32_t *tail;
    uint8_t *result;
    uint64_t total;
    uint64_t pages;
    uint64_t virt;

    if (size > UINT64_MAX - sizeof(slab_page_t) -
               SLAB_EXTENT_HEAD_SIZE - SLAB_EXTENT_TAIL_SIZE) return NULL;
    total = size + sizeof(slab_page_t) + SLAB_EXTENT_HEAD_SIZE +
            SLAB_EXTENT_TAIL_SIZE;
    pages = total / PAGE_SIZE;
    if (total % PAGE_SIZE) pages++;
    virt = slab_virtual_alloc(pages);
    if (!virt) return NULL;
    if (!slab_map_pages(virt, pages)) {
        *backing_failed = 1;
        return NULL;
    }
    extent = (slab_page_t *)virt;
    memset(extent, 0, sizeof(*extent));
    extent->magic = SLAB_MAGIC;
    extent->payload_size = size;
    extent->pages = (uint32_t)pages;
    extent->live_count = 1;
    extent->kind = SLAB_KIND_EXTENT;
    head = (uint32_t *)((uint8_t *)extent + sizeof(*extent));
    result = (uint8_t *)head + SLAB_EXTENT_HEAD_SIZE;
    tail = (uint32_t *)(result + size);
    *head = HEAP_CANARY_HEAD;
    *tail = HEAP_CANARY_TAIL;
    slab_pages_allocated += pages;
    return result;
}

static slab_block_t *slab_small_block_for_ptr(slab_page_t *page, void *ptr) {
    slab_block_t *block;
    uint64_t address;
    uint64_t end;

    address = (uint64_t)page + sizeof(slab_page_t);
    end = (uint64_t)page + PAGE_SIZE;
    while (address + sizeof(slab_block_t) <= end) {
        block = (slab_block_t *)address;
        if (block->span < sizeof(slab_block_t) ||
            block->span > end - address) return NULL;
        if ((uint8_t *)block + sizeof(slab_block_t) == (uint8_t *)ptr)
            return block;
        address += block->span;
    }
    return NULL;
}

static void slab_small_coalesce(slab_page_t *page) {
    slab_block_t *block;
    slab_block_t *next;
    uint64_t address;
    uint64_t next_address;
    uint64_t end;

    address = (uint64_t)page + sizeof(slab_page_t);
    end = (uint64_t)page + PAGE_SIZE;
    page->max_free = 0;
    while (address + sizeof(slab_block_t) <= end) {
        block = (slab_block_t *)address;
        if (block->span < sizeof(slab_block_t) ||
            block->span > end - address) return;
        next_address = address + block->span;
        if (next_address < end) {
            next = (slab_block_t *)next_address;
            if (next->span < sizeof(slab_block_t) ||
                next->span > end - next_address) return;
            if (!block->requested && !next->requested) {
                block->span += next->span;
                continue;
            }
        }
        if (!block->requested && block->span > page->max_free)
            page->max_free = block->span;
        address = next_address;
    }
}

void KERNEL_EARLY_INIT slab_init(void) {
    slab_small_pages = NULL;
    slab_small_tail = NULL;
    slab_small_spare = NULL;
    slab_pages_allocated = 0;
    slab_lock = 0;
    slab_virtual_dirty = 0;
    slab_flush_active = 0;
    slab_fresh_only = 0;
    slab_initialized = 1;
    KERNEL_INIT_LOG("Compact allocator initialized (shared blocks to %u bytes, extents to 65536)\n",
                    (unsigned int)SLAB_SMALL_MAX);
}

void *slab_alloc(size_t size) {
    void *result;
    uint64_t eflags;
    int flush_result;
    int retry;
    int backing_failed;

    if (!slab_initialized || size == 0 || size > SLAB_MAX_SIZE) return NULL;
    for (;;) {
        slab_lock_acquire(&eflags);
        backing_failed = 0;
        if (size <= SLAB_SMALL_MAX)
            result = slab_small_allocate(size, &backing_failed);
        else
            result = slab_extent_allocate(size, &backing_failed);
        retry = !result && !backing_failed &&
            (slab_virtual_dirty || slab_flush_active);
        slab_lock_release(eflags);
        if (!retry) return result;
        flush_result = slab_flush_stale();
        if (flush_result < 0) {
            slab_lock_acquire(&eflags);
            slab_fresh_only = 1;
            if (size <= SLAB_SMALL_MAX)
                result = slab_small_allocate(size, &backing_failed);
            else
                result = slab_extent_allocate(size, &backing_failed);
            slab_fresh_only = 0;
            slab_lock_release(eflags);
            return result;
        }
        smp_tlb_flush_poll();
        __asm__ volatile ("pause" ::: "memory");
    }
}

void *slab_page_alloc(size_t size) {
    uint64_t pages;
    uint64_t virt;
    uint64_t eflags;
    int flush_result;
    int retry;

    if (!slab_initialized || size == 0 ||
        size > UINT64_MAX - PAGE_SIZE + 1) return NULL;
    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (;;) {
        slab_lock_acquire(&eflags);
        virt = slab_virtual_alloc(pages);
        if (virt && slab_map_pages(virt, pages)) {
            slab_pages_allocated += pages;
            slab_lock_release(eflags);
            return (void *)(uintptr_t)virt;
        }
        retry = !virt && (slab_virtual_dirty || slab_flush_active);
        slab_lock_release(eflags);
        if (!retry) return NULL;
        flush_result = slab_flush_stale();
        if (flush_result < 0) {
            slab_lock_acquire(&eflags);
            slab_fresh_only = 1;
            virt = slab_virtual_alloc(pages);
            if (virt && slab_map_pages(virt, pages)) {
                slab_pages_allocated += pages;
                slab_fresh_only = 0;
                slab_lock_release(eflags);
                return (void *)(uintptr_t)virt;
            }
            slab_fresh_only = 0;
            slab_lock_release(eflags);
            return NULL;
        }
        smp_tlb_flush_poll();
        __asm__ volatile ("pause" ::: "memory");
    }
}

void slab_page_free(void *ptr, size_t size) {
    uint64_t address;
    uint64_t region_end;
    uint64_t pages;
    uint64_t page;
    uint64_t eflags;

    if (!ptr || !slab_initialized || size == 0 ||
        size > UINT64_MAX - PAGE_SIZE + 1) return;
    address = (uint64_t)(uintptr_t)ptr;
    if (address & (PAGE_SIZE - 1)) return;
    if (!slab_region_contains(address, SLAB_PRIMARY_START,
                              SLAB_PRIMARY_SIZE) &&
        !slab_region_contains(address, SLAB_OVERFLOW_START,
                              SLAB_OVERFLOW_SIZE)) return;
    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > UINT64_MAX / PAGE_SIZE ||
        address > UINT64_MAX - pages * PAGE_SIZE) return;
    region_end = slab_region_contains(address, SLAB_PRIMARY_START,
                                      SLAB_PRIMARY_SIZE) ?
                 SLAB_PRIMARY_START + SLAB_PRIMARY_SIZE :
                 SLAB_OVERFLOW_START + SLAB_OVERFLOW_SIZE;
    if (address + pages * PAGE_SIZE > region_end) return;
    slab_lock_acquire(&eflags);
    for (page = 0; page < pages; page++) {
        if (!vmm_get_phys_in_pml4(vmm_get_kernel_cr3(),
                                  address + page * PAGE_SIZE)) {
            slab_lock_release(eflags);
            return;
        }
    }
    if (pages > slab_pages_allocated) {
        slab_lock_release(eflags);
        return;
    }
    slab_pages_allocated -= pages;
    slab_unmap_pages(address, pages);
    slab_lock_release(eflags);
}

void slab_free(void *ptr, void *caller) {
    slab_page_t *page;
    slab_block_t *block;
    uint64_t page_address;
    uint64_t pages;
    uint64_t size;
    uint64_t eflags;
    uint32_t *head;
    uint32_t *tail;

    if (!ptr || !slab_initialized) return;
    page_address = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    if (!slab_region_contains(page_address, SLAB_PRIMARY_START,
                              SLAB_PRIMARY_SIZE) &&
        !slab_region_contains(page_address, SLAB_OVERFLOW_START,
                              SLAB_OVERFLOW_SIZE)) return;
    if (!vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), page_address)) return;
    page = (slab_page_t *)page_address;
    if (page->magic != SLAB_MAGIC) return;
    slab_lock_acquire(&eflags);
    if (page->kind == SLAB_KIND_EXTENT) {
        if ((uint8_t *)page + sizeof(*page) + SLAB_EXTENT_HEAD_SIZE !=
                (uint8_t *)ptr ||
            page->live_count != 1 || page->pages == 0) {
            slab_lock_release(eflags);
            return;
        }
        size = page->payload_size;
        head = (uint32_t *)((uint8_t *)page + sizeof(*page));
        tail = (uint32_t *)((uint8_t *)ptr + size);
        if (*head != HEAP_CANARY_HEAD || *tail != HEAP_CANARY_TAIL) {
            printf("slab_free: extent corruption ptr=%p caller=%p\n",
                   ptr, caller);
            slab_lock_release(eflags);
            return;
        }
        pages = page->pages;
        page->magic = 0;
        slab_pages_allocated -= pages;
        slab_unmap_pages(page_address, pages);
        slab_lock_release(eflags);
        return;
    }
    if (page->kind != SLAB_KIND_SMALL) {
        slab_lock_release(eflags);
        return;
    }
    block = slab_small_block_for_ptr(page, ptr);
    if (!block || !block->requested) {
        slab_lock_release(eflags);
        return;
    }
    size = block->requested;
    block->requested = 0;
    page->live_count--;
    page->payload_size -= size;
    if (page->live_count == 0 && !slab_small_spare) {
        block = (slab_block_t *)(page_address + sizeof(slab_page_t));
        block->span = PAGE_SIZE - sizeof(slab_page_t);
        block->requested = 0;
        page->payload_size = 0;
        page->max_free = block->span;
        slab_small_spare = page;
        slab_lock_release(eflags);
        return;
    }
    if (page->live_count == 0) {
        slab_small_remove(page);
        page->magic = 0;
        slab_pages_allocated--;
        slab_unmap_pages(page_address, 1);
        slab_lock_release(eflags);
        return;
    }
    slab_small_coalesce(page);
    slab_lock_release(eflags);
}

int slab_owns(void *ptr) {
    slab_page_t *page;
    uint64_t address;

    if (!ptr || !slab_initialized) return 0;
    address = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    if (!slab_region_contains(address, SLAB_PRIMARY_START,
                              SLAB_PRIMARY_SIZE) &&
        !slab_region_contains(address, SLAB_OVERFLOW_START,
                              SLAB_OVERFLOW_SIZE)) return 0;
    if (!vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), address)) return 0;
    page = (slab_page_t *)address;
    return page->magic == SLAB_MAGIC;
}

size_t slab_alloc_size(void *ptr) {
    slab_page_t *page;
    slab_block_t *block;
    uint64_t address;

    if (!ptr || !slab_initialized) return 0;
    address = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    if (!vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), address)) return 0;
    page = (slab_page_t *)address;
    if (page->magic != SLAB_MAGIC) return 0;
    if (page->kind == SLAB_KIND_EXTENT) {
        if ((uint8_t *)page + sizeof(*page) + SLAB_EXTENT_HEAD_SIZE !=
            (uint8_t *)ptr) return 0;
        return page->payload_size;
    }
    if (page->kind != SLAB_KIND_SMALL) return 0;
    block = slab_small_block_for_ptr(page, ptr);
    if (!block || !block->requested) return 0;
    return block->requested;
}

int slab_resize(void *ptr, size_t new_size) {
    slab_page_t *page;
    slab_block_t *block;
    slab_block_t *next;
    slab_block_t *remainder;
    uint64_t address;
    uint64_t total;
    uint64_t pages;
    uint64_t old_pages;
    uint64_t eflags;
    uint32_t needed;
    uint32_t span;
    uint32_t old_size;
    uint32_t *head;
    uint32_t *tail;

    if (!ptr || !slab_initialized || new_size == 0 ||
        new_size > SLAB_MAX_SIZE) return 0;
    address = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    if (!slab_region_contains(address, SLAB_PRIMARY_START,
                              SLAB_PRIMARY_SIZE) &&
        !slab_region_contains(address, SLAB_OVERFLOW_START,
                              SLAB_OVERFLOW_SIZE)) return 0;
    slab_lock_acquire(&eflags);
    if (!vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), address)) goto fail;
    page = (slab_page_t *)address;
    if (page->magic != SLAB_MAGIC) goto fail;
    if (page->kind == SLAB_KIND_SMALL) {
        if (new_size > SLAB_SMALL_MAX) goto fail;
        block = slab_small_block_for_ptr(page, ptr);
        if (!block || !block->requested) goto fail;
        needed = ((uint32_t)new_size + 7u) & ~7u;
        needed += sizeof(*block);
        span = block->span;
        if (needed > span) {
            if ((uint64_t)block + span >= address + PAGE_SIZE) goto fail;
            next = (slab_block_t *)((uint8_t *)block + span);
            if (next->requested || next->span < sizeof(*next) ||
                next->span > address + PAGE_SIZE - (uint64_t)next ||
                needed > span + next->span) goto fail;
            span += next->span;
        }
        old_size = block->requested;
        block->span = span;
        if (span - needed >= sizeof(*block) + 8) {
            remainder = (slab_block_t *)((uint8_t *)block + needed);
            remainder->span = span - needed;
            remainder->requested = 0;
            block->span = needed;
        }
        block->requested = (uint32_t)new_size;
        page->payload_size = page->payload_size - old_size + new_size;
        slab_small_coalesce(page);
        slab_lock_release(eflags);
        return 1;
    }
    if (page->kind != SLAB_KIND_EXTENT || page->live_count != 1 ||
        page->pages == 0 ||
        (uint8_t *)page + sizeof(*page) + SLAB_EXTENT_HEAD_SIZE !=
            (uint8_t *)ptr) goto fail;
    if (new_size <= SLAB_SMALL_MAX) goto fail;
    total = new_size + sizeof(*page) + SLAB_EXTENT_HEAD_SIZE +
            SLAB_EXTENT_TAIL_SIZE;
    pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > page->pages) goto fail;
    head = (uint32_t *)((uint8_t *)page + sizeof(*page));
    tail = (uint32_t *)((uint8_t *)ptr + page->payload_size);
    if (*head != HEAP_CANARY_HEAD || *tail != HEAP_CANARY_TAIL) goto fail;
    old_pages = page->pages;
    page->payload_size = (uint32_t)new_size;
    page->pages = (uint16_t)pages;
    tail = (uint32_t *)((uint8_t *)ptr + new_size);
    *tail = HEAP_CANARY_TAIL;
    if (pages < old_pages) {
        slab_pages_allocated -= old_pages - pages;
        slab_unmap_pages(address + pages * PAGE_SIZE, old_pages - pages);
    }
    slab_lock_release(eflags);
    return 1;
fail:
    slab_lock_release(eflags);
    return 0;
}

size_t slab_max_size(void) {
    return SLAB_MAX_SIZE;
}

uint64_t slab_get_total_pages(void) {
    return slab_pages_allocated;
}

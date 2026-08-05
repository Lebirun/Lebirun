#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <string.h>

static uint64_t demand_base = 0;
static uint64_t demand_max_pages = 0;
static uint64_t demand_reserved_end = 0;
#define DEMAND_INLINE_HEAP_SIZE 0x00200000
#define DEMAND_INLINE_PAGES (DEMAND_INLINE_HEAP_SIZE / PAGE_SIZE)
static uint8_t demand_committed_bitmap[DEMAND_INLINE_PAGES / 8];
#define DEMAND_EXTENSION_BITS (PAGE_SIZE * 8)
#define DEMAND_EXTENSION_COUNT \
    (((HEAP_MAX_SIZE_CAP / PAGE_SIZE) - DEMAND_INLINE_PAGES + \
      DEMAND_EXTENSION_BITS - 1) / DEMAND_EXTENSION_BITS)
static uint8_t *demand_committed_extensions[DEMAND_EXTENSION_COUNT];
static int demand_initialized = 0;
static volatile int demand_lock = 0;

extern void *pmm_alloc_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern void vmm_map_page_pae(uint64_t virt_addr, uint64_t phys_addr,
                             uint64_t flags);
extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));

static inline void demand_lock_acquire(uint64_t *eflags_out) {
    uint64_t eflags;

    for (;;) {
        __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
        if (__sync_lock_test_and_set(&demand_lock, 1) == 0) {
            *eflags_out = eflags;
            return;
        }
        if (eflags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void demand_lock_release(uint64_t eflags) {
    __sync_lock_release(&demand_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static uint64_t *demand_get_pte(uint64_t page_virt);

static uint64_t demand_page_index(uint64_t page_virt) {
    return (page_virt - demand_base) / PAGE_SIZE;
}

static uint8_t *demand_bitmap_byte(uint64_t page_idx) {
    uint64_t extension_idx;
    uint64_t extension_bit;

    if (page_idx < DEMAND_INLINE_PAGES)
        return &demand_committed_bitmap[page_idx / 8];
    page_idx -= DEMAND_INLINE_PAGES;
    extension_idx = page_idx / DEMAND_EXTENSION_BITS;
    extension_bit = page_idx % DEMAND_EXTENSION_BITS;
    if (extension_idx >= DEMAND_EXTENSION_COUNT) return NULL;
    if (!demand_committed_extensions[extension_idx]) return NULL;
    return &demand_committed_extensions[extension_idx][extension_bit / 8];
}

static int demand_ensure_bitmap_capacity(uint64_t page_count) {
    uint64_t extension_count;
    uint64_t extension_idx;
    uint64_t phys;

    if (page_count <= DEMAND_INLINE_PAGES) return 0;
    extension_count = (page_count - DEMAND_INLINE_PAGES +
                       DEMAND_EXTENSION_BITS - 1) /
                      DEMAND_EXTENSION_BITS;
    if (extension_count > DEMAND_EXTENSION_COUNT) return -1;
    for (extension_idx = 0; extension_idx < extension_count;
         extension_idx++) {
        if (demand_committed_extensions[extension_idx]) continue;
        phys = (uint64_t)pmm_alloc_low_page();
        if (!phys) phys = (uint64_t)pmm_alloc_page();
        if (!phys) return -1;
        if (pt_ensure_phys_mapped(phys) < 0) {
            pfa_free(phys);
            return -1;
        }
        pmm_zero_page_phys(phys);
        demand_committed_extensions[extension_idx] =
            (uint8_t *)(phys + KERNEL_VMA);
    }
    return 0;
}

static int demand_test_committed(uint64_t page_idx) {
    uint8_t *bitmap_byte;

    bitmap_byte = demand_bitmap_byte(page_idx);
    if (!bitmap_byte) return 0;
    return (*bitmap_byte & (1u << (page_idx % 8))) != 0;
}

static void demand_set_committed(uint64_t page_idx) {
    uint8_t *bitmap_byte;

    bitmap_byte = demand_bitmap_byte(page_idx);
    if (!bitmap_byte) return;
    *bitmap_byte |= (uint8_t)(1u << (page_idx % 8));
}

static void demand_clear_committed(uint64_t page_idx) {
    uint8_t *bitmap_byte;

    bitmap_byte = demand_bitmap_byte(page_idx);
    if (!bitmap_byte) return;
    *bitmap_byte &= (uint8_t)~(1u << (page_idx % 8));
}

void KERNEL_EARLY_INIT demand_paging_init(void) {
    uint64_t heap_max_size;

    demand_base = HEAP_START;
    heap_max_size = kernel_heap.max_addr - kernel_heap.start_addr;
    demand_max_pages = heap_max_size / PAGE_SIZE;
    memset(demand_committed_bitmap, 0, sizeof(demand_committed_bitmap));
    memset(demand_committed_extensions, 0,
           sizeof(demand_committed_extensions));
    demand_reserved_end = demand_base;
    demand_initialized = 1;
    KERNEL_INIT_LOG("Demand paging initialized: base=0x%016lX max_pages=%lu\n",
           demand_base, demand_max_pages);
}

int demand_reserve_range(uint64_t virt_start, uint64_t size) {
    uint64_t eflags;
    uint64_t end;
    uint64_t limit;
    uint64_t page_count;
    
    if (!demand_initialized) return -1;
    
    if (size == 0) return 0;
    limit = demand_base + demand_max_pages * PAGE_SIZE;
    if (virt_start < demand_base || virt_start >= limit) return -1;
    if (size > limit - virt_start) return -1;
    if (size > UINT64_MAX - virt_start - (PAGE_SIZE - 1)) return -1;
    
    end = (virt_start + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end > limit) return -1;
    demand_lock_acquire(&eflags);
    if (virt_start > demand_reserved_end) {
        demand_lock_release(eflags);
        return -1;
    }
    page_count = (end - demand_base) / PAGE_SIZE;
    if (demand_ensure_bitmap_capacity(page_count) < 0) {
        demand_lock_release(eflags);
        return -1;
    }
    if (end > demand_reserved_end) demand_reserved_end = end;
    demand_lock_release(eflags);
    return 0;
}

int demand_is_reserved(uint64_t virt_addr) {
    uint64_t eflags;
    int result;
    
    if (!demand_initialized) return 0;
    
    if (virt_addr < demand_base) return 0;
    demand_lock_acquire(&eflags);
    result = virt_addr < demand_reserved_end;
    demand_lock_release(eflags);
    
    return result;
}

int demand_commit_page(uint64_t virt_addr) {
    uint64_t eflags;
    uint64_t page_virt;
    uint64_t page_idx;
    void *phys_page;
    
    if (!demand_initialized) return -1;
    
    page_virt = virt_addr & ~(PAGE_SIZE - 1);
    
    if (page_virt < demand_base) return -1;
    if (page_virt >= demand_base + (demand_max_pages * PAGE_SIZE)) return -1;
    page_idx = demand_page_index(page_virt);
    
    demand_lock_acquire(&eflags);
    if (page_virt >= demand_reserved_end) {
        demand_lock_release(eflags);
        return -1;
    }
    if (demand_test_committed(page_idx)) {
        demand_lock_release(eflags);
        return 0;
    }
    
    phys_page = pmm_alloc_low_page();
    if (!phys_page) {
        phys_page = pmm_alloc_page();
    }
    if (!phys_page) {
        demand_lock_release(eflags);
        printf("demand_commit_page: Failed to allocate physical page\n");
        return -1;
    }
    
    vmm_map_page_pae(page_virt, (uint64_t)phys_page, 3);
    
    memset((void *)page_virt, 0, PAGE_SIZE);
    demand_set_committed(page_idx);
    
    demand_lock_release(eflags);
    
    
    return 0;
}

void demand_mark_committed(uint64_t virt_addr) {
    uint64_t page_virt;
    uint64_t page_idx;
    uint64_t eflags;

    if (!demand_initialized) return;
    page_virt = virt_addr & ~(PAGE_SIZE - 1);
    if (page_virt < demand_base) return;
    if (page_virt >= demand_base + demand_max_pages * PAGE_SIZE) return;
    page_idx = demand_page_index(page_virt);
    demand_lock_acquire(&eflags);
    if (demand_ensure_bitmap_capacity(page_idx + 1) < 0) {
        demand_lock_release(eflags);
        return;
    }
    if (page_virt + PAGE_SIZE > demand_reserved_end)
        demand_reserved_end = page_virt + PAGE_SIZE;
    demand_set_committed(page_idx);
    demand_lock_release(eflags);
}

int demand_page_fault_handler(uint64_t fault_addr, uint64_t err_code) {
    uint64_t page_virt;
    
    if (!demand_initialized) return 0;
    
    if (err_code & 0x1) return 0;
    
    page_virt = fault_addr & ~(PAGE_SIZE - 1);
    
    if (page_virt < demand_base) return 0;
    if (page_virt >= demand_base + (demand_max_pages * PAGE_SIZE)) return 0;
    
    if (!demand_is_reserved(page_virt)) return 0;
    
    if (demand_commit_page(page_virt) == 0) {
        return 1;
    }
    
    return 0;
}

uint64_t demand_get_committed_pages(void) {
    uint64_t eflags;
    uint64_t count;
    uint64_t page_idx;
    
    if (!demand_initialized) return 0;
    
    count = 0;
    demand_lock_acquire(&eflags);
    
    for (page_idx = 0;
         page_idx < (demand_reserved_end - demand_base) / PAGE_SIZE;
         page_idx++) {
        if (demand_test_committed(page_idx)) count++;
    }
    
    demand_lock_release(eflags);
    return count;
}

uint64_t demand_get_reserved_pages(void) {
    uint64_t eflags;
    uint64_t count;
    
    if (!demand_initialized) return 0;
    
    demand_lock_acquire(&eflags);
    count = (demand_reserved_end - demand_base) / PAGE_SIZE;
    demand_lock_release(eflags);
    return count;
}

static uint64_t *demand_get_pte(uint64_t page_virt) {
    uint64_t pdpt_idx;
    uint64_t pdpte;
    uint64_t *pd;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t pde;
    uint64_t *pt64;
    uint64_t *kv_pdpt;

    kv_pdpt = (uint64_t *)((uintptr_t)boot_pdpt_high + KERNEL_VMA);
    pdpt_idx = (page_virt >> 30) & 0x1FF;
    pdpte = kv_pdpt[pdpt_idx];
    if (!(pdpte & 1)) return NULL;
    pd = (uint64_t *)((pdpte & ~0xFFFULL) + KERNEL_VMA);
    pd_idx = (page_virt >> 21) & 0x1FF;
    pt_idx = (page_virt >> 12) & 0x1FF;
    pde = pd[pd_idx];
    if (!(pde & 1) || (pde & 0x80)) return NULL;
    pt64 = (uint64_t *)((pde & ~0xFFFULL) + KERNEL_VMA);
    return &pt64[pt_idx];
}

int demand_decommit_range(uint64_t virt_start, uint64_t virt_end) {
    uint64_t eflags;
    uint64_t page_virt;
    uint64_t page_idx;
    uint64_t start;
    uint64_t end;
    uint64_t *pte_ptr;
    uint64_t pte;
    uint64_t phys;
    int changed;
    int flush_result;

    if (!demand_initialized) return -1;
    start = virt_start & ~(PAGE_SIZE - 1);
    end = (virt_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end < start) return -1;
    if (start < demand_base) return -1;
    if (end > demand_base + (demand_max_pages * PAGE_SIZE)) return -1;
    if (start == end) return 0;

    demand_lock_acquire(&eflags);
    changed = 0;
    for (page_virt = start; page_virt < end; page_virt += PAGE_SIZE) {
        page_idx = demand_page_index(page_virt);
        if (!demand_test_committed(page_idx)) continue;
        pte_ptr = demand_get_pte(page_virt);
        if (!pte_ptr) {
            demand_clear_committed(page_idx);
            continue;
        }
        pte = *pte_ptr;
        if (pte & 1) {
            *pte_ptr = pte & ~1ULL;
            __asm__ volatile("invlpg (%0)" : : "r"(page_virt) : "memory");
            changed = 1;
        }
    }
    flush_result = changed ? smp_tlb_flush_all_sync() : 0;
    for (page_virt = start; page_virt < end; page_virt += PAGE_SIZE) {
        page_idx = demand_page_index(page_virt);
        if (!demand_test_committed(page_idx)) continue;
        pte_ptr = demand_get_pte(page_virt);
        if (!pte_ptr) {
            demand_clear_committed(page_idx);
            continue;
        }
        pte = *pte_ptr;
        if (!(pte & ~0xFFFULL)) {
            demand_clear_committed(page_idx);
            continue;
        }
        if (flush_result < 0) {
            if (!(pte & 1) && (pte & ~0xFFFULL)) *pte_ptr = pte | 1ULL;
            continue;
        }
        phys = pte & ~0xFFFULL;
        *pte_ptr = 0;
        demand_clear_committed(page_idx);
        if (phys >= 0x1000) pfa_free(phys);
    }
    if (flush_result == 0 && end == demand_reserved_end)
        demand_reserved_end = start;
    demand_lock_release(eflags);
    return flush_result;
}

uint64_t demand_get_bitmap_bytes(void) {
    uint64_t bytes;
    uint64_t extension_idx;

    bytes = sizeof(demand_committed_bitmap);
    for (extension_idx = 0; extension_idx < DEMAND_EXTENSION_COUNT;
         extension_idx++) {
        if (demand_committed_extensions[extension_idx]) bytes += PAGE_SIZE;
    }
    return bytes;
}

uint64_t demand_get_bitmap_extension_pages(void) {
    uint64_t pages;
    uint64_t extension_idx;

    pages = 0;
    for (extension_idx = 0; extension_idx < DEMAND_EXTENSION_COUNT;
         extension_idx++) {
        if (demand_committed_extensions[extension_idx]) pages++;
    }
    return pages;
}

int demand_decommit_page(uint64_t virt_addr) {
    uint64_t page_virt;

    page_virt = virt_addr & ~(PAGE_SIZE - 1);
    return demand_decommit_range(page_virt, page_virt + PAGE_SIZE);
}

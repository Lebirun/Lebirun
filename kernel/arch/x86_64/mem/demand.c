#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <string.h>

static uint8_t *demand_reserved_bitmap = NULL;
static uint8_t *demand_committed_bitmap = NULL;
static uint64_t demand_bitmap_bytes = 0;
static uint64_t demand_base = 0;
static uint64_t demand_max_pages = 0;
static int demand_initialized = 0;
static volatile int demand_lock = 0;

extern void *pmm_alloc_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);

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

static inline uint64_t page_to_index(uint64_t virt) {
    return (virt - demand_base) / PAGE_SIZE;
}

static inline void set_reserved_bit(uint64_t idx) {
    demand_reserved_bitmap[idx / 8] |= (1 << (idx % 8));
}

static inline void clear_reserved_bit(uint64_t idx) {
    demand_reserved_bitmap[idx / 8] &= ~(1 << (idx % 8));
}

static inline int test_reserved_bit(uint64_t idx) {
    return (demand_reserved_bitmap[idx / 8] & (1 << (idx % 8))) != 0;
}

static inline void set_committed_bit(uint64_t idx) {
    demand_committed_bitmap[idx / 8] |= (1 << (idx % 8));
}

static inline void clear_committed_bit(uint64_t idx) {
    demand_committed_bitmap[idx / 8] &= ~(1 << (idx % 8));
}

static inline int test_committed_bit(uint64_t idx) {
    return (demand_committed_bitmap[idx / 8] & (1 << (idx % 8))) != 0;
}

void demand_paging_init(void) {
    uint64_t heap_max_size;

    demand_base = HEAP_START;
    heap_max_size = kernel_heap.max_addr - kernel_heap.start_addr;
    demand_max_pages = heap_max_size / PAGE_SIZE;

    demand_bitmap_bytes = (demand_max_pages + 7) / 8;
    if (demand_bitmap_bytes < 64)
        demand_bitmap_bytes = 64;

    demand_reserved_bitmap = (uint8_t *)kmalloc(demand_bitmap_bytes);
    demand_committed_bitmap = (uint8_t *)kmalloc(demand_bitmap_bytes);
    if (!demand_reserved_bitmap || !demand_committed_bitmap) {
        printf("Demand paging: failed to allocate bitmaps (%lu bytes)\n", demand_bitmap_bytes);
        return;
    }
    memset(demand_reserved_bitmap, 0, demand_bitmap_bytes);
    memset(demand_committed_bitmap, 0, demand_bitmap_bytes);

    demand_initialized = 1;
    printf("Demand paging initialized: base=0x%016lX max_pages=%lu bitmap=%lu bytes\n",
           demand_base, demand_max_pages, demand_bitmap_bytes);
}

int demand_reserve_range(uint64_t virt_start, uint64_t size) {
    uint64_t eflags;
    uint64_t start_page;
    uint64_t end_page;
    uint64_t i;
    
    if (!demand_initialized) return -1;
    
    if (virt_start < demand_base) return -1;
    if (virt_start + size > demand_base + (demand_max_pages * PAGE_SIZE)) return -1;
    
    start_page = (virt_start - demand_base) / PAGE_SIZE;
    end_page = ((virt_start + size + PAGE_SIZE - 1) - demand_base) / PAGE_SIZE;
    
    if (end_page > demand_max_pages) return -1;
    
    demand_lock_acquire(&eflags);
    
    for (i = start_page; i < end_page; i++) {
        set_reserved_bit(i);
    }
    
    demand_lock_release(eflags);
    return 0;
}

int demand_is_reserved(uint64_t virt_addr) {
    uint64_t idx;
    uint64_t eflags;
    int result;
    
    if (!demand_initialized) return 0;
    
    if (virt_addr < demand_base) return 0;
    if (virt_addr >= demand_base + (demand_max_pages * PAGE_SIZE)) return 0;
    
    idx = page_to_index(virt_addr & ~(PAGE_SIZE - 1));
    
    demand_lock_acquire(&eflags);
    result = test_reserved_bit(idx);
    demand_lock_release(eflags);
    
    return result;
}

int demand_commit_page(uint64_t virt_addr) {
    uint64_t idx;
    uint64_t eflags;
    uint64_t page_virt;
    void *phys_page;
    
    if (!demand_initialized) return -1;
    
    page_virt = virt_addr & ~(PAGE_SIZE - 1);
    
    if (page_virt < demand_base) return -1;
    if (page_virt >= demand_base + (demand_max_pages * PAGE_SIZE)) return -1;
    
    idx = page_to_index(page_virt);
    
    demand_lock_acquire(&eflags);
    
    if (!test_reserved_bit(idx)) {
        demand_lock_release(eflags);
        return -1;
    }
    
    if (test_committed_bit(idx)) {
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
    
    {
        extern void vmm_map_page_pae(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
        vmm_map_page_pae(page_virt, (uint64_t)phys_page, 3);
    }
    
    memset((void *)page_virt, 0, PAGE_SIZE);
    
    set_committed_bit(idx);
    
    demand_lock_release(eflags);
    
    
    return 0;
}

void demand_mark_committed(uint64_t virt_addr) {
    uint64_t page_virt;
    uint64_t idx;
    uint64_t eflags;

    if (!demand_initialized) return;

    page_virt = virt_addr & ~(PAGE_SIZE - 1);
    if (page_virt < demand_base) return;
    if (page_virt >= demand_base + (demand_max_pages * PAGE_SIZE)) return;

    idx = page_to_index(page_virt);

    demand_lock_acquire(&eflags);
    set_reserved_bit(idx);
    set_committed_bit(idx);
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
    uint64_t i;
    
    if (!demand_initialized) return 0;
    
    count = 0;
    demand_lock_acquire(&eflags);
    
    for (i = 0; i < demand_max_pages; i++) {
        if (test_committed_bit(i)) count++;
    }
    
    demand_lock_release(eflags);
    return count;
}

uint64_t demand_get_reserved_pages(void) {
    uint64_t eflags;
    uint64_t count;
    uint64_t i;
    
    if (!demand_initialized) return 0;
    
    count = 0;
    demand_lock_acquire(&eflags);
    
    for (i = 0; i < demand_max_pages; i++) {
        if (test_reserved_bit(i)) count++;
    }
    
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
    extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));
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
    uint64_t idx;
    uint64_t eflags;
    uint64_t page_virt;
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
        idx = page_to_index(page_virt);
        if (!test_committed_bit(idx)) continue;
        pte_ptr = demand_get_pte(page_virt);
        if (!pte_ptr) {
            clear_committed_bit(idx);
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
        idx = page_to_index(page_virt);
        if (!test_committed_bit(idx)) continue;
        pte_ptr = demand_get_pte(page_virt);
        if (!pte_ptr) {
            clear_committed_bit(idx);
            continue;
        }
        pte = *pte_ptr;
        if (flush_result < 0) {
            if (!(pte & 1) && (pte & ~0xFFFULL)) *pte_ptr = pte | 1ULL;
            continue;
        }
        phys = pte & ~0xFFFULL;
        *pte_ptr = 0;
        clear_committed_bit(idx);
        if (phys >= 0x1000) pfa_free(phys);
    }
    demand_lock_release(eflags);
    return flush_result;
}

int demand_decommit_page(uint64_t virt_addr) {
    uint64_t page_virt;

    page_virt = virt_addr & ~(PAGE_SIZE - 1);
    return demand_decommit_range(page_virt, page_virt + PAGE_SIZE);
}

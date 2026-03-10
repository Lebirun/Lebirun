#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

#define DEMAND_BITMAP_PAGES ((HEAP_MAX_SIZE / PAGE_SIZE) / 8)

static uint8_t demand_reserved_bitmap[DEMAND_BITMAP_PAGES];
static uint8_t demand_committed_bitmap[DEMAND_BITMAP_PAGES];
static uint32_t demand_base = 0;
static uint32_t demand_max_pages = 0;
static int demand_initialized = 0;
static volatile int demand_lock = 0;

extern uint32_t pae_enabled;
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);

static inline void demand_lock_acquire(uint32_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&demand_lock, 1)) {
    }
}

static inline void demand_lock_release(uint32_t eflags) {
    __sync_lock_release(&demand_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static inline uint32_t page_to_index(uint32_t virt) {
    return (virt - demand_base) / PAGE_SIZE;
}

static inline void set_reserved_bit(uint32_t idx) {
    demand_reserved_bitmap[idx / 8] |= (1 << (idx % 8));
}

static inline void clear_reserved_bit(uint32_t idx) {
    demand_reserved_bitmap[idx / 8] &= ~(1 << (idx % 8));
}

static inline int test_reserved_bit(uint32_t idx) {
    return (demand_reserved_bitmap[idx / 8] & (1 << (idx % 8))) != 0;
}

static inline void set_committed_bit(uint32_t idx) {
    demand_committed_bitmap[idx / 8] |= (1 << (idx % 8));
}

static inline void clear_committed_bit(uint32_t idx) {
    demand_committed_bitmap[idx / 8] &= ~(1 << (idx % 8));
}

static inline int test_committed_bit(uint32_t idx) {
    return (demand_committed_bitmap[idx / 8] & (1 << (idx % 8))) != 0;
}

void demand_paging_init(void) {
    demand_base = HEAP_START;
    demand_max_pages = HEAP_MAX_SIZE / PAGE_SIZE;
    
    memset(demand_reserved_bitmap, 0, sizeof(demand_reserved_bitmap));
    memset(demand_committed_bitmap, 0, sizeof(demand_committed_bitmap));
    
    demand_initialized = 1;
    printf("Demand paging initialized: base=0x%08X max_pages=%u\n", 
           demand_base, demand_max_pages);
}

int demand_reserve_range(uint32_t virt_start, uint32_t size) {
    uint32_t eflags;
    uint32_t start_page;
    uint32_t end_page;
    uint32_t i;
    
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

int demand_is_reserved(uint32_t virt_addr) {
    uint32_t idx;
    uint32_t eflags;
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

int demand_commit_page(uint32_t virt_addr) {
    uint32_t idx;
    uint32_t eflags;
    uint32_t page_virt;
    void *phys_page;
    uint32_t *pd;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pt;
    uint32_t pt_addr;
    void *pt_page;
    
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
    
    if (pae_enabled) {
        extern void vmm_map_page_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
        vmm_map_page_pae(page_virt, (uint32_t)phys_page, 3);
    } else {
        pd_idx = page_virt >> 22;
        pt_idx = (page_virt >> 12) & 0x3FF;
        pd = (uint32_t *)0xFFFFF000;
        
        if (!(pd[pd_idx] & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                pfa_free((uint32_t)phys_page);
                demand_lock_release(eflags);
                return -1;
            }
            pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | 3;
            pt_addr = 0xFFC00000 + (pd_idx << 12);
            __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
            pt = (uint32_t *)pt_addr;
            memset(pt, 0, PAGE_SIZE);
        }
        
        pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
        pt[pt_idx] = ((uint32_t)phys_page & ~0xFFF) | 3;
        __asm__ volatile("invlpg (%0)" : : "r"(page_virt) : "memory");
    }
    
    memset((void *)page_virt, 0, PAGE_SIZE);
    
    set_committed_bit(idx);
    
    demand_lock_release(eflags);
    
    DEBUG_MEMORY("demand_commit_page: committed 0x%08X -> phys 0x%08X\n", 
                 page_virt, (uint32_t)phys_page);
    
    return 0;
}

void demand_mark_committed(uint32_t virt_addr) {
    uint32_t page_virt;
    uint32_t idx;
    uint32_t eflags;

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

int demand_page_fault_handler(uint32_t fault_addr, uint32_t err_code) {
    uint32_t page_virt;
    
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

uint32_t demand_get_committed_pages(void) {
    uint32_t eflags;
    uint32_t count;
    uint32_t i;
    
    if (!demand_initialized) return 0;
    
    count = 0;
    demand_lock_acquire(&eflags);
    
    for (i = 0; i < demand_max_pages; i++) {
        if (test_committed_bit(i)) count++;
    }
    
    demand_lock_release(eflags);
    return count;
}

uint32_t demand_get_reserved_pages(void) {
    uint32_t eflags;
    uint32_t count;
    uint32_t i;
    
    if (!demand_initialized) return 0;
    
    count = 0;
    demand_lock_acquire(&eflags);
    
    for (i = 0; i < demand_max_pages; i++) {
        if (test_reserved_bit(i)) count++;
    }
    
    demand_lock_release(eflags);
    return count;
}

int demand_decommit_page(uint32_t virt_addr) {
    uint32_t idx;
    uint32_t eflags;
    uint32_t page_virt;
    uint32_t *pd;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pt;
    uint32_t phys;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pt64;
    uint32_t pt_phys_val;
    uint64_t pte;
    uint32_t pae_phys;
    extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));

    if (!demand_initialized) return -1;

    page_virt = virt_addr & ~(PAGE_SIZE - 1);

    if (page_virt < demand_base) return -1;
    if (page_virt >= demand_base + (demand_max_pages * PAGE_SIZE)) return -1;

    idx = page_to_index(page_virt);

    demand_lock_acquire(&eflags);

    if (!test_committed_bit(idx)) {
        demand_lock_release(eflags);
        return 0;
    }

    if (pae_enabled) {
        pae_pd_idx = (page_virt >> 21) & 0x1FF;
        pae_pt_idx = (page_virt >> 12) & 0x1FF;

        pde = boot_pd_high[pae_pd_idx];
        if (!(pde & 1)) {
            clear_committed_bit(idx);
            demand_lock_release(eflags);
            return 0;
        }

        pt_phys_val = (uint32_t)(pde & ~0xFFFULL);
        pt64 = (uint64_t *)(pt_phys_val + 0xC0000000);
        pte = pt64[pae_pt_idx];

        if (pte & 1) {
            pae_phys = (uint32_t)(pte & ~0xFFFULL);
            pt64[pae_pt_idx] = 0;
            __asm__ volatile("invlpg (%0)" : : "r"(page_virt) : "memory");
            if (pae_phys >= 0x1000) {
                pfa_free(pae_phys);
            }
        }

        clear_committed_bit(idx);
        demand_lock_release(eflags);
        return 0;
    }

    pd = (uint32_t *)0xFFFFF000;
    pd_idx = page_virt >> 22;
    pt_idx = (page_virt >> 12) & 0x3FF;

    if (!(pd[pd_idx] & 1)) {
        clear_committed_bit(idx);
        demand_lock_release(eflags);
        return 0;
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    phys = pt[pt_idx] & ~0xFFF;

    if (pt[pt_idx] & 1) {
        pt[pt_idx] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(page_virt) : "memory");
        if (phys >= 0x1000) {
            pfa_free(phys);
        }
    }

    clear_committed_bit(idx);
    demand_lock_release(eflags);

    return 0;
}

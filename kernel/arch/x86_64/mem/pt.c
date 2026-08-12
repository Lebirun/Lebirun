#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <lebirun/spinlock.h>
#include <string.h>

extern uint64_t boot_pml4[] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt_low[] __attribute__((aligned(4096)));
extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));
extern uint64_t boot_pd_low[] __attribute__((aligned(4096)));

#define kv_pml4 ((uint64_t *)((uintptr_t)boot_pml4 + KERNEL_VMA))
#define kv_pdpt_high ((uint64_t *)((uintptr_t)boot_pdpt_high + KERNEL_VMA))
#define kv_pdpt_low  ((uint64_t *)((uintptr_t)boot_pdpt_low + KERNEL_VMA))

void pt_sync_kernel_mappings(void);
int pt_ensure_phys_mapped(uint64_t phys_addr);

static spinlock_t vmm_pae_lock = {0};
static volatile uint64_t vmm_pae_saved_eflags = 0;

static inline void vmm_pae_lock_acquire(void) {
    uint64_t eflags;

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli" ::: "memory");
    while (!spin_trylock(&vmm_pae_lock)) {
        cpu_relax();
    }
    vmm_pae_saved_eflags = eflags;
}

static inline void vmm_pae_lock_release(void) {
    uint64_t eflags = vmm_pae_saved_eflags;
    spin_unlock(&vmm_pae_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

static uint64_t pt_heap_pt_count = 0;
uint64_t pt_vmm_pt_count = 0;
static int low_identity_active = 1;

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void *pmm_alloc_early_page(void);
extern uint8_t *pfa_bitmap;
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);

static void *pt_alloc_pt_page(void) {
    if (!pfa_bitmap) return pmm_alloc_early_page();
    return pmm_alloc_low_page();
}

#define PT_TEMP_ZERO_VIRT TEMP_SLOT(7)

static void pt_zero_page(uint64_t phys_addr) {
    uint64_t eflags;

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");
    temp_map_raw(PT_TEMP_ZERO_VIRT, phys_addr);
    memset((void *)PT_TEMP_ZERO_VIRT, 0, PAGE_SIZE);
    temp_unmap_raw(PT_TEMP_ZERO_VIRT);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static void pt_write_pte_raw(uint64_t pt_phys, uint64_t pte_idx, uint64_t value) {
    uint64_t eflags;
    uint64_t *pt;

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");
    temp_map_raw(PT_TEMP_ZERO_VIRT, pt_phys);
    pt = (uint64_t *)PT_TEMP_ZERO_VIRT;
    pt[pte_idx] = value;
    temp_unmap_raw(PT_TEMP_ZERO_VIRT);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static uint64_t *pt_get_kernel_pd(uint64_t virt_addr) {
    uint64_t pdpt_idx;
    uint64_t pdpte;
    uint64_t pd_phys;

    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pdpte = kv_pdpt_high[pdpt_idx];
    if (!(pdpte & 1)) {
        return NULL;
    }
    pd_phys = pdpte & ~0xFFFULL;
    return (uint64_t *)(pd_phys + KERNEL_VMA);
}

static int pt_huge_mapping_matches(uint64_t pde, uint64_t virt_addr,
                                   uint64_t phys_addr, uint64_t flags) {
    uint64_t mapped_phys;
    uint64_t page_offset;

    if (!(pde & 1) || !(pde & 0x80)) return 0;
    if ((flags & (0xFFFULL | (1ULL << 63))) != 3) return 0;
    if ((pde & (0x1EULL | 0x100ULL | 0x1000ULL | (1ULL << 63))) != 2)
        return 0;
    page_offset = virt_addr & (0x200000ULL - 1);
    mapped_phys = (pde & 0x000FFFFFFFE00000ULL) + page_offset;
    if ((mapped_phys & ~(PAGE_SIZE - 1)) !=
        (phys_addr & ~(PAGE_SIZE - 1))) return 0;
    return 1;
}

static uint64_t *pt_ensure_kernel_pd(uint64_t virt_addr) {
    uint64_t pdpt_idx;
    uint64_t pdpte;
    uint64_t pd_phys;
    void *page;

    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pdpte = kv_pdpt_high[pdpt_idx];
    if (pdpte & 1) {
        pd_phys = pdpte & ~0xFFFULL;
        return (uint64_t *)(pd_phys + KERNEL_VMA);
    }
    page = pt_alloc_pt_page();
    if (!page) page = pmm_alloc_page();
    if (!page) return NULL;
    pd_phys = (uint64_t)page;
    if (pt_ensure_phys_mapped(pd_phys) < 0) return NULL;
    memset((void *)(pd_phys + KERNEL_VMA), 0, PAGE_SIZE);
    kv_pdpt_high[pdpt_idx] = (pd_phys & ~0xFFFULL) | 3;
    __asm__ volatile(
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        : : : "rax", "memory"
    );
    return (uint64_t *)(pd_phys + KERNEL_VMA);
}

static int pt_split_huge_page(uint64_t *pd, uint64_t pd_idx) {
    uint64_t pde;
    uint64_t huge_phys;
    uint64_t pt_phys;
    uint64_t i;
    void *pt_page;
    uint64_t leaf_flags;
    uint64_t table_flags;
    uint64_t eflags;
    volatile uint64_t *w;

    pde = pd[pd_idx];
    if (!(pde & 1)) return 0;
    if (!(pde & 0x80)) return 0;

    huge_phys = pde & 0x000FFFFFFFE00000ULL;
    leaf_flags = pde & (0xFFFULL | VMM_PTE_NX);
    leaf_flags &= ~(0x80ULL | 0x1000ULL);
    if (pde & 0x1000) leaf_flags |= 0x80;
    table_flags = VMM_PTE_PRESENT | VMM_PTE_WRITE |
                  (pde & VMM_PTE_USER);

    pt_page = pt_alloc_pt_page();
    if (!pt_page) pt_page = pmm_alloc_page();
    if (!pt_page) return -1;
    pt_phys = (uint64_t)pt_page;

    pt_zero_page(pt_phys);

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");
    temp_map_raw(PT_TEMP_ZERO_VIRT, pt_phys);
    w = (volatile uint64_t *)PT_TEMP_ZERO_VIRT;
    for (i = 0; i < 512; i++) {
        w[i] = (huge_phys + i * PAGE_SIZE) | leaf_flags;
    }
    temp_unmap_raw(PT_TEMP_ZERO_VIRT);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    pd[pd_idx] = (pt_phys & ~0xFFFULL) | table_flags;
    __asm__ volatile(
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        : : : "rax", "memory"
    );

    if (pt_ensure_phys_mapped(pt_phys) < 0) return -1;

    pt_vmm_pt_count++;

    return 0;
}

int pt_ensure_phys_mapped(uint64_t phys_addr) {
    uint64_t virt;
    uint64_t pde_idx;
    uint64_t pte_idx;
    uint64_t pde;
    void *pt_page;
    uint64_t pt_phys;
    uint64_t *pt;
    uint64_t *pd;
    int pt_self_mapped;

    phys_addr &= ~(PAGE_SIZE - 1);
    virt = phys_addr + KERNEL_VMA;
    pde_idx = (virt >> 21) & 0x1FF;
    pte_idx = (virt >> 12) & 0x1FF;
    pd = pt_ensure_kernel_pd(virt);
    if (!pd) return -1;
    pde = pd[pde_idx];

    if (pde & 0x80) {
        if (pt_huge_mapping_matches(pde, virt, phys_addr, 3)) return 0;
        if (pt_split_huge_page(pd, pde_idx) < 0) return -1;
        pde = pd[pde_idx];
    }

    if (!(pde & 1)) {
        pt_page = pt_alloc_pt_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) return -1;

        pt_phys = (uint64_t)pt_page;

        pt_zero_page(pt_phys);

        pt_write_pte_raw(pt_phys, pte_idx, ((uint64_t)phys_addr) | 3);

        pt_self_mapped = 0;
        {
            uint64_t pt_pde_idx;
            pt_pde_idx = ((pt_phys + KERNEL_VMA) >> 21) & 0x1FF;
            if (pt_pde_idx == pde_idx) {
                uint64_t pt_pte_idx;
                pt_pte_idx = ((pt_phys + KERNEL_VMA) >> 12) & 0x1FF;
                pt_write_pte_raw(pt_phys, pt_pte_idx, ((uint64_t)pt_phys) | 3);
                pt_self_mapped = 1;
            }
        }

        pd[pde_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
        pt_sync_kernel_mappings();
        __asm__ volatile(
            "mov %%cr3, %%rax\n\t"
            "mov %%rax, %%cr3\n\t"
            : : : "rax", "memory"
        );

        if (!pt_self_mapped) {
            uint64_t pt_pde_idx;
            uint64_t *pt_pd;
            pt_pde_idx = ((pt_phys + KERNEL_VMA) >> 21) & 0x1FF;
            pt_pd = pt_get_kernel_pd(pt_phys + KERNEL_VMA);
            if (pt_pd && !(pt_pd[pt_pde_idx] & 1)) {
                pt_ensure_phys_mapped(pt_phys);
            }
        }

        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
        pt_vmm_pt_count++;
        return 0;
    }

    pt_phys = (uint64_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + KERNEL_VMA);
    if (!(pt[pte_idx] & 1)) {
        pt[pte_idx] = ((uint64_t)phys_addr) | 3;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    return 0;
}

void pt_sync_kernel_mappings(void) {
}

static uint64_t *pt_get_pd_for_pdpt(uint64_t virt_addr) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pdpte;
    uint64_t pd_phys;

    pml4_idx = (virt_addr >> 39) & 0x1FF;
    if (pml4_idx == 511) {
        return pt_ensure_kernel_pd(virt_addr);
    }
    if (pml4_idx == 0) {
        if (!low_identity_active) return NULL;
        pdpt_idx = (virt_addr >> 30) & 0x1FF;
        pdpte = kv_pdpt_low[pdpt_idx];
        if (pdpte & 1) {
            pd_phys = pdpte & ~0xFFFULL;
            return (uint64_t *)(pd_phys + KERNEL_VMA);
        }
    }
    return NULL;
}

int KERNEL_INIT pt_reclaim_low_identity(void) {
    uint64_t eflags;
    uint64_t start;
    int flush_result;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(eflags) :: "memory");
    kv_pml4[0] = 0;
    low_identity_active = 0;
    __asm__ volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        : : : "rax", "memory"
    );
    if (eflags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    flush_result = smp_tlb_flush_all_sync();
    if (flush_result < 0) return -1;
    start = (uint64_t)(uintptr_t)boot_pdpt_low;
    pfa_reclaim_kernel_range(start, start + PAGE_SIZE);
    return 0;
}

void vmm_map_page_pae(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t pt_pd_idx;
    uint64_t pt_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pt_phys;
    uint64_t new_pte;

    vmm_pae_lock_acquire();

    pt_pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= KERNEL_VMA) {
        flags &= ~0x4;
    }

    pd = pt_get_pd_for_pdpt(virt_addr);
    if (!pd) {
        vmm_pae_lock_release();
        printf("vmm_map_page_pae: Failed to get PD (v=0x%016lX p=0x%016lX)\n",
               virt_addr, phys_addr);
        return;
    }

    pde = pd[pt_pd_idx];

    if (pde & 0x80) {
        if (pt_huge_mapping_matches(pde, virt_addr, phys_addr, flags)) {
            vmm_pae_lock_release();
            return;
        }
        if (pt_split_huge_page(pd, pt_pd_idx) < 0) {
            vmm_pae_lock_release();
            printf("vmm_map_page_pae: Failed to split huge page\n");
            return;
        }
        pde = pd[pt_pd_idx];
    }

    if (!(pde & 1)) {
        void *pt_page;
        uint64_t id_pde_idx;
        uint64_t id_pte_idx;
        pt_page = pt_alloc_pt_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            vmm_pae_lock_release();
            printf("vmm_map_page_pae: Failed to alloc page table\n");
            return;
        }
        pt_phys = (uint64_t)pt_page;
        pt_zero_page(pt_phys);

        pd[pt_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) |
                        VMM_PTE_PRESENT | VMM_PTE_WRITE |
                        (flags & VMM_PTE_USER);
        pt_sync_kernel_mappings();

        id_pde_idx = ((pt_phys + KERNEL_VMA) >> 21) & 0x1FF;
        id_pte_idx = ((pt_phys + KERNEL_VMA) >> 12) & 0x1FF;
        if (id_pde_idx == pt_pd_idx) {
            pt_write_pte_raw(pt_phys, id_pte_idx, ((uint64_t)pt_phys) | 3);
        } else {
            if (pt_ensure_phys_mapped(pt_phys) < 0) {
                vmm_pae_lock_release();
                printf("vmm_map_page_pae: Failed to map page table page\n");
                return;
            }
        }
        __asm__ volatile(
            "mov %%cr3, %%rax\n\t"
            "mov %%rax, %%cr3\n\t"
            : : : "rax", "memory"
        );
        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
        pt_vmm_pt_count++;
    } else {
        pt_phys = (uint64_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
        if ((flags & 0x4) && !(pd[pt_pd_idx] & 0x4)) {
            pd[pt_pd_idx] |= 0x4;
        }
    }

    new_pte = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    if (pt[pt_pt_idx] != new_pte) {
        pt[pt_pt_idx] = new_pte;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }

    vmm_pae_lock_release();
}

int vmm_map_huge_page_pae(uint64_t virt_addr, uint64_t phys_addr,
                          uint64_t flags) {
    uint64_t pd_idx;
    uint64_t pde;
    uint64_t new_pde;
    uint64_t huge_flags;
    uint64_t *pd;

    if (virt_addr < KERNEL_VMA) return -1;
    if (((virt_addr | phys_addr) & (VMM_HUGE_PAGE_SIZE - 1)) != 0)
        return -1;
    if (phys_addr >= PHYSICAL_ADDRESS_LIMIT) return -1;

    vmm_pae_lock_acquire();

    flags &= ~VMM_PTE_USER;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pd = pt_get_pd_for_pdpt(virt_addr);
    if (!pd) {
        vmm_pae_lock_release();
        return -1;
    }

    pde = pd[pd_idx];
    if ((pde & VMM_PTE_PRESENT) && !(pde & 0x80)) {
        vmm_pae_lock_release();
        return -1;
    }

    huge_flags = flags & (0xFFFULL | VMM_PTE_NX);
    huge_flags &= ~(0x80ULL | 0x1000ULL);
    if (flags & 0x80) huge_flags |= 0x1000;
    new_pde = (phys_addr & 0x000FFFFFFFE00000ULL) |
              huge_flags | 0x80;
    if (pde != new_pde) {
        pd[pd_idx] = new_pde;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }

    vmm_pae_lock_release();
    return 0;
}

void vmm_map_page_early_pae(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t pt_pd_idx;
    uint64_t pt_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pt_phys;

    vmm_pae_lock_acquire();

    pt_pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= KERNEL_VMA) {
        flags &= ~0x4;
    }

    pd = pt_get_pd_for_pdpt(virt_addr);
    if (!pd) {
        vmm_pae_lock_release();
        return;
    }

    pde = pd[pt_pd_idx];

    if (pde & 0x80) {
        if (pt_split_huge_page(pd, pt_pd_idx) < 0) {
            vmm_pae_lock_release();
            return;
        }
        pde = pd[pt_pd_idx];
    }

    if (!(pde & 1)) {
        vmm_pae_lock_release();
        return;
    }

    pt_phys = (uint64_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + KERNEL_VMA);
    pt[pt_pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");

    vmm_pae_lock_release();
}

void vmm_map_range_alloc_pae(uint64_t virt_addr, uint64_t size, uint64_t flags) {
    uint64_t start;
    uint64_t end;
    uint64_t v;
    uint64_t pt_pd_idx;
    uint64_t pt_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pt_phys;
    void *phys_page;

    if (size == 0) return;
    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= KERNEL_VMA) {
        flags &= ~0x4;
    }

    vmm_pae_lock_acquire();

    for (v = start; v < end; v += PAGE_SIZE) {
        pt_pd_idx = (v >> 21) & 0x1FF;
        pt_pt_idx = (v >> 12) & 0x1FF;

        pd = pt_get_pd_for_pdpt(v);
        if (!pd) {
            printf("vmm_map_range_alloc_pae: Failed to get PD (v=0x%016lX)\n", v);
            vmm_pae_lock_release();
            return;
        }
        pde = pd[pt_pd_idx];

        if (pde & 0x80) {
            if (pt_split_huge_page(pd, pt_pd_idx) < 0) {
                printf("vmm_map_range_alloc_pae: Failed to split huge page\n");
                vmm_pae_lock_release();
                return;
            }
            pde = pd[pt_pd_idx];
        }

        if (!(pde & 1)) {
            void *pt_page;
            uint64_t id_pde_idx;
            uint64_t id_pte_idx;
            pt_page = pt_alloc_pt_page();
            if (!pt_page) pt_page = pmm_alloc_page();
            if (!pt_page) {
                printf("vmm_map_range_alloc_pae: Failed to alloc page table\n");
                vmm_pae_lock_release();
                return;
            }
            pt_phys = (uint64_t)pt_page;
            pt_zero_page(pt_phys);

            pd[pt_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) |
                            VMM_PTE_PRESENT | VMM_PTE_WRITE |
                            (flags & VMM_PTE_USER);
            pt_sync_kernel_mappings();

            id_pde_idx = ((pt_phys + KERNEL_VMA) >> 21) & 0x1FF;
            id_pte_idx = ((pt_phys + KERNEL_VMA) >> 12) & 0x1FF;
            if (id_pde_idx == pt_pd_idx) {
                pt_write_pte_raw(pt_phys, id_pte_idx, ((uint64_t)pt_phys) | 3);
            } else {
                if (pt_ensure_phys_mapped(pt_phys) < 0) {
                    printf("vmm_map_range_alloc_pae: Failed to map page table page\n");
                    vmm_pae_lock_release();
                    return;
                }
            }
            __asm__ volatile(
                "mov %%cr3, %%rax\n\t"
                "mov %%rax, %%cr3\n\t"
                : : : "rax", "memory"
            );
            pt = (uint64_t *)(pt_phys + KERNEL_VMA);
            pt_vmm_pt_count++;
        } else {
            pt_phys = (uint64_t)(pde & ~0xFFFULL);
            pt = (uint64_t *)(pt_phys + KERNEL_VMA);
            if ((flags & 0x4) && !(pd[pt_pd_idx] & 0x4)) {
                pd[pt_pd_idx] |= 0x4;
            }
        }

        if (!(pt[pt_pt_idx] & 1)) {
            phys_page = pmm_alloc_page();
            if (!phys_page) {
                printf("vmm_map_range_alloc_pae: Failed to alloc phys page\n");
                vmm_pae_lock_release();
                return;
            }
            pt[pt_pt_idx] = ((uint64_t)(uint64_t)phys_page & ~0xFFFULL) | (flags & 0xFFF);
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        } else {
            pt[pt_pt_idx] = (pt[pt_pt_idx] & ~0xFFFULL) | (flags & 0xFFF);
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        }
    }

    vmm_pae_lock_release();
}

int heap_map_page_pae(uint64_t virt_addr) {
    uint64_t pt_pd_idx;
    uint64_t pt_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t *split_pt;
    void *phys_page;
    uint64_t pt_phys;

    vmm_pae_lock_acquire();

    pt_pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_pt_idx = (virt_addr >> 12) & 0x1FF;

    pd = pt_get_kernel_pd(virt_addr);
    if (!pd) {
        pd = pt_ensure_kernel_pd(virt_addr);
        if (!pd) {
            printf("heap_map_page_pae: Failed to get PD\n");
            vmm_pae_lock_release();
            return -1;
        }
    }

    pde = pd[pt_pd_idx];

    if (pde & 0x80) {
        if (pt_split_huge_page(pd, pt_pd_idx) < 0) {
            printf("heap_map_page_pae: Failed to split huge page\n");
            vmm_pae_lock_release();
            return -1;
        }
        pde = pd[pt_pd_idx];
        if (pde & 1) {
            split_pt = (uint64_t *)((pde & ~0xFFFULL) + KERNEL_VMA);
            split_pt[pt_pt_idx] = 0;
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
    }

    if (!(pde & 1)) {
        void *pt_page;
        uint64_t id_pde_idx;
        uint64_t id_pte_idx;
        pt_page = pt_alloc_pt_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            printf("heap_map_page_pae: Failed to alloc page table\n");
            vmm_pae_lock_release();
            return -1;
        }
        pt_phys = (uint64_t)pt_page;
        pt_zero_page(pt_phys);

        pd[pt_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
        pt_sync_kernel_mappings();

        id_pde_idx = ((pt_phys + KERNEL_VMA) >> 21) & 0x1FF;
        id_pte_idx = ((pt_phys + KERNEL_VMA) >> 12) & 0x1FF;
        if (id_pde_idx == pt_pd_idx) {
            pt_write_pte_raw(pt_phys, id_pte_idx, ((uint64_t)pt_phys) | 3);
        } else {
            if (pt_ensure_phys_mapped(pt_phys) < 0) {
                printf("heap_map_page_pae: Failed to map page table page\n");
                vmm_pae_lock_release();
                return -1;
            }
        }
        __asm__ volatile(
            "mov %%cr3, %%rax\n\t"
            "mov %%rax, %%cr3\n\t"
            : : : "rax", "memory"
        );
        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
        pt_heap_pt_count++;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    } else {
        pt_phys = (uint64_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
    }

    if (pt[pt_pt_idx] & 1) {
        pt[pt_pt_idx] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }

    phys_page = pmm_alloc_page();
    if (!phys_page) {
        printf("heap_map_page_pae: Failed to alloc phys page\n");
        vmm_pae_lock_release();
        return -1;
    }
    pt[pt_pt_idx] = ((uint64_t)(uint64_t)phys_page & ~0xFFFULL) | 3;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");

    vmm_pae_lock_release();
    return 0;
}

void vmm_unmap_page_pae(uint64_t virt_addr) {
    uint64_t pt_pd_idx;
    uint64_t pt_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pt_phys;

    vmm_pae_lock_acquire();

    pt_pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_pt_idx = (virt_addr >> 12) & 0x1FF;

    pd = pt_get_pd_for_pdpt(virt_addr);
    if (!pd) {
        vmm_pae_lock_release();
        return;
    }

    pde = pd[pt_pd_idx];
    if (pde & 0x80) {
        if (pt_split_huge_page(pd, pt_pd_idx) < 0) {
            vmm_pae_lock_release();
            return;
        }
        pde = pd[pt_pd_idx];
    }
    if (!(pde & 1)) {
        vmm_pae_lock_release();
        return;
    }

    pt_phys = (uint64_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + KERNEL_VMA);
    pt[pt_pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");

    vmm_pae_lock_release();
}

uint64_t pt_get_heap_pt_count(void) {
    return pt_heap_pt_count;
}

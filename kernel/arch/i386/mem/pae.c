#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <kernel/spinlock.h>
#include <string.h>

extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));
extern uint64_t boot_pd_low[] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt[] __attribute__((aligned(32)));
extern uint32_t pae_enabled;

void pae_sync_kernel_mappings(void);

static spinlock_t vmm_pae_lock = {0};
static volatile uint32_t vmm_pae_saved_eflags = 0;

static inline void vmm_pae_lock_acquire(void) {
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli" ::: "memory");
    spin_lock(&vmm_pae_lock);
    vmm_pae_saved_eflags = eflags;
}

static inline void vmm_pae_lock_release(void) {
    uint32_t eflags = vmm_pae_saved_eflags;
    spin_unlock(&vmm_pae_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

#define PAE_PDPT_POOL_SIZE 128

static uint64_t pae_pdpt_pool[PAE_PDPT_POOL_SIZE][4] __attribute__((aligned(32)));
static uint64_t *pae_pd_pool[PAE_PDPT_POOL_SIZE][4];
static uint8_t pae_pdpt_pool_used[PAE_PDPT_POOL_SIZE];

#define PAE_VMM_PT_INIT_SIZE 512

static uint64_t *pae_heap_page_tables[16];
static uint32_t pae_heap_pt_count = 0;
static uint64_t *pae_vmm_pt_static[PAE_VMM_PT_INIT_SIZE];
uint64_t **pae_vmm_page_tables = pae_vmm_pt_static;
uint32_t pae_vmm_pt_count = 0;
uint32_t pae_vmm_pt_capacity = PAE_VMM_PT_INIT_SIZE;
static int pae_vmm_pt_is_dynamic = 0;

static uint64_t *pae_mid_pd[2];

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);
extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);

static int pae_vmm_pt_grow(void) {
    return -1;
}

#define PAE_TEMP_ZERO_VIRT 0xF7007000u

static void pae_zero_page(uint32_t phys_addr) {
    uint32_t eflags;
    volatile uint32_t *w;
    uint32_t i;

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");
    temp_map_raw(PAE_TEMP_ZERO_VIRT, phys_addr);
    w = (volatile uint32_t *)PAE_TEMP_ZERO_VIRT;
    for (i = 0; i < PAGE_SIZE / 4; i++) {
        w[i] = 0;
    }
    temp_unmap_raw(PAE_TEMP_ZERO_VIRT);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

static void pae_write_pte_raw(uint32_t pt_phys, uint32_t pte_idx, uint64_t value) {
    uint32_t eflags;
    uint64_t *pt;

    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");
    temp_map_raw(PAE_TEMP_ZERO_VIRT, pt_phys);
    pt = (uint64_t *)PAE_TEMP_ZERO_VIRT;
    pt[pte_idx] = value;
    temp_unmap_raw(PAE_TEMP_ZERO_VIRT);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

int pae_ensure_phys_mapped(uint32_t phys_addr) {
    uint32_t virt;
    uint32_t pde_idx;
    uint32_t pte_idx;
    uint64_t pde;
    void *pt_page;
    uint32_t pt_phys;
    uint32_t pt_slot;
    uint64_t *pt;
    int pt_self_mapped;

    phys_addr &= ~(PAGE_SIZE - 1);
    virt = phys_addr + 0xC0000000;
    pde_idx = (virt >> 21) & 0x1FF;
    pte_idx = (virt >> 12) & 0x1FF;
    pde = boot_pd_high[pde_idx];

    if (!(pde & 1)) {
        pt_page = pmm_alloc_low_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) return -1;

        if (pae_vmm_pt_count >= pae_vmm_pt_capacity) {
            if (pae_vmm_pt_grow() < 0) return -1;
        }

        pt_phys = (uint32_t)pt_page;

        pae_zero_page(pt_phys);

        pae_write_pte_raw(pt_phys, pte_idx, ((uint64_t)phys_addr) | 3);

        pt_self_mapped = 0;
        {
            uint32_t pt_pde_idx;
            pt_pde_idx = ((pt_phys + 0xC0000000) >> 21) & 0x1FF;
            if (pt_pde_idx == pde_idx) {
                uint32_t pt_pte_idx;
                pt_pte_idx = ((pt_phys + 0xC0000000) >> 12) & 0x1FF;
                pae_write_pte_raw(pt_phys, pt_pte_idx, ((uint64_t)pt_phys) | 3);
                pt_self_mapped = 1;
            }
        }

        boot_pd_high[pde_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
        pae_sync_kernel_mappings();
        __asm__ volatile(
            "mov %%cr3, %%eax\n\t"
            "mov %%eax, %%cr3\n\t"
            : : : "eax", "memory"
        );

        if (!pt_self_mapped) {
            uint32_t pt_pde_idx;
            pt_pde_idx = ((pt_phys + 0xC0000000) >> 21) & 0x1FF;
            if (!(boot_pd_high[pt_pde_idx] & 1)) {
                pae_ensure_phys_mapped(pt_phys);
            }
        }

        pt = (uint64_t *)(pt_phys + 0xC0000000);
        pt_slot = pae_vmm_pt_count++;
        pae_vmm_page_tables[pt_slot] = pt;
        return 0;
    }

    pt_phys = (uint32_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + 0xC0000000);
    if (!(pt[pte_idx] & 1)) {
        pt[pte_idx] = ((uint64_t)phys_addr) | 3;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    return 0;
}

void pae_sync_kernel_mappings(void) {
    uint32_t slot;
    uint32_t j;
    uint64_t entry;
    
    if (!pae_enabled) return;
    
    for (slot = 0; slot < PAE_PDPT_POOL_SIZE; slot++) {
        if (!pae_pdpt_pool_used[slot]) continue;
        if (!pae_pd_pool[slot][3]) continue;
        for (j = 0; j < 512; j++) {
            entry = boot_pd_high[j];
            if (entry & 1) {
                pae_pd_pool[slot][3][j] = entry & ~0x4ULL;
            } else {
                pae_pd_pool[slot][3][j] = 0;
            }
        }
    }
}

static uint64_t *pae_get_pd_for_pdpt(uint32_t pdpt_idx) {
    uint32_t pd_phys;
    void *page;

    if (pdpt_idx == 3) {
        return boot_pd_high;
    }
    if (pdpt_idx == 0) {
        return boot_pd_low;
    }
    if (pdpt_idx >= 4) {
        return NULL;
    }
    if (pae_mid_pd[pdpt_idx - 1]) {
        return pae_mid_pd[pdpt_idx - 1];
    }
    page = pmm_alloc_low_page();
    if (!page) page = pmm_alloc_page();
    if (!page) return NULL;
    pd_phys = (uint32_t)page;
    if (pae_ensure_phys_mapped(pd_phys) < 0) return NULL;
    pae_mid_pd[pdpt_idx - 1] = (uint64_t *)(pd_phys + 0xC0000000);
    memset(pae_mid_pd[pdpt_idx - 1], 0, PAGE_SIZE);
    boot_pdpt[pdpt_idx] = ((uint64_t)pd_phys & ~0xFFFULL) | 1;
    __asm__ volatile(
        "mov %%cr3, %%eax\n\t"
        "mov %%eax, %%cr3\n\t"
        : : : "eax", "memory"
    );
    return pae_mid_pd[pdpt_idx - 1];
}

void vmm_map_page_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;

    vmm_pae_lock_acquire();

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pd = pae_get_pd_for_pdpt(pdpt_idx);
    if (!pd) {
        vmm_pae_lock_release();
        printf("vmm_map_page_pae: Failed to get PD for pdpt_idx %u (virt=0x%08X)\n", pdpt_idx, virt_addr);
        return;
    }

    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        void *pt_page;
        uint32_t id_pde_idx;
        uint32_t id_pte_idx;
        if (pae_vmm_pt_count >= pae_vmm_pt_capacity) {
            if (pae_vmm_pt_grow() < 0) {
                vmm_pae_lock_release();
                printf("vmm_map_page_pae: Out of VMM page tables\n");
                return;
            }
        }
        pt_page = pmm_alloc_low_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            vmm_pae_lock_release();
            printf("vmm_map_page_pae: Failed to alloc page table\n");
            return;
        }
        pt_phys = (uint32_t)pt_page;
        pae_zero_page(pt_phys);

        pd[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | (flags | 3);
        pae_sync_kernel_mappings();

        id_pde_idx = ((pt_phys + 0xC0000000) >> 21) & 0x1FF;
        id_pte_idx = ((pt_phys + 0xC0000000) >> 12) & 0x1FF;
        if (id_pde_idx == pae_pd_idx) {
            pae_write_pte_raw(pt_phys, id_pte_idx, ((uint64_t)pt_phys) | 3);
        } else {
            if (pae_ensure_phys_mapped(pt_phys) < 0) {
                vmm_pae_lock_release();
                printf("vmm_map_page_pae: Failed to map page table page\n");
                return;
            }
        }
        __asm__ volatile(
            "mov %%cr3, %%eax\n\t"
            "mov %%eax, %%cr3\n\t"
            : : : "eax", "memory"
        );
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        pt_slot = pae_vmm_pt_count++;
        pae_vmm_page_tables[pt_slot] = pt;
    } else {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        if ((flags & 0x4) && !(pd[pae_pd_idx] & 0x4)) {
            pd[pae_pd_idx] |= 0x4;
        }
    }

    pt[pae_pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");

    vmm_pae_lock_release();
}

void vmm_map_page_early_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pd = pae_get_pd_for_pdpt(pdpt_idx);
    if (!pd) {
        return;
    }

    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        return;
    }

    pt_phys = (uint32_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + 0xC0000000);
    pt[pae_pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_map_range_alloc_pae(uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;
    uint32_t v;
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;
    void *phys_page;

    if (size == 0) return;
    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        pdpt_idx = (v >> 30) & 0x3;
        pae_pd_idx = (v >> 21) & 0x1FF;
        pae_pt_idx = (v >> 12) & 0x1FF;

        pd = pae_get_pd_for_pdpt(pdpt_idx);
        if (!pd) {
            printf("vmm_map_range_alloc_pae: Failed to get PD (v=0x%08X)\n", v);
            return;
        }
        pde = pd[pae_pd_idx];
        if (!(pde & 1)) {
            void *pt_page;
            uint32_t id_pde_idx;
            uint32_t id_pte_idx;
            if (pae_vmm_pt_count >= pae_vmm_pt_capacity) {
                if (pae_vmm_pt_grow() < 0) {
                    printf("vmm_map_range_alloc_pae: Out of VMM page tables\n");
                    return;
                }
            }
            pt_page = pmm_alloc_low_page();
            if (!pt_page) pt_page = pmm_alloc_page();
            if (!pt_page) {
                printf("vmm_map_range_alloc_pae: Failed to alloc page table\n");
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pae_zero_page(pt_phys);

            pd[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | (flags | 3);
            pae_sync_kernel_mappings();

            id_pde_idx = ((pt_phys + 0xC0000000) >> 21) & 0x1FF;
            id_pte_idx = ((pt_phys + 0xC0000000) >> 12) & 0x1FF;
            if (id_pde_idx == pae_pd_idx) {
                pae_write_pte_raw(pt_phys, id_pte_idx, ((uint64_t)pt_phys) | 3);
            } else {
                if (pae_ensure_phys_mapped(pt_phys) < 0) {
                    printf("vmm_map_range_alloc_pae: Failed to map page table page\n");
                    return;
                }
            }
            __asm__ volatile(
                "mov %%cr3, %%eax\n\t"
                "mov %%eax, %%cr3\n\t"
                : : : "eax", "memory"
            );
            pt = (uint64_t *)(pt_phys + 0xC0000000);
            pt_slot = pae_vmm_pt_count++;
            pae_vmm_page_tables[pt_slot] = pt;
        } else {
            pt_phys = (uint32_t)(pde & ~0xFFFULL);
            pt = (uint64_t *)(pt_phys + 0xC0000000);
            if ((flags & 0x4) && !(pd[pae_pd_idx] & 0x4)) {
                pd[pae_pd_idx] |= 0x4;
            }
        }

        if (!(pt[pae_pt_idx] & 1)) {
            phys_page = pmm_alloc_page();
            if (!phys_page) {
                printf("vmm_map_range_alloc_pae: Failed to alloc phys page\n");
                return;
            }
            pt[pae_pt_idx] = ((uint64_t)(uint32_t)phys_page & ~0xFFFULL) | (flags & 0xFFF);
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        } else {
            pt[pae_pt_idx] = (pt[pae_pt_idx] & ~0xFFFULL) | (flags & 0xFFF);
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        }
    }
}

int heap_map_page_pae(uint32_t virt_addr) {
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint32_t pt_slot;
    uint64_t pde;
    uint64_t *pt;
    void *phys_page;
    uint32_t pt_phys;

    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    pde = boot_pd_high[pae_pd_idx];
    if (!(pde & 1)) {
        void *pt_page;
        uint32_t id_pde_idx;
        uint32_t id_pte_idx;
        if (pae_heap_pt_count >= 16) {
            printf("heap_map_page_pae: Out of heap page tables\n");
            return -1;
        }
        pt_page = pmm_alloc_low_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            printf("heap_map_page_pae: Failed to alloc page table\n");
            return -1;
        }
        pt_phys = (uint32_t)pt_page;
        pae_zero_page(pt_phys);

        boot_pd_high[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
        pae_sync_kernel_mappings();

        id_pde_idx = ((pt_phys + 0xC0000000) >> 21) & 0x1FF;
        id_pte_idx = ((pt_phys + 0xC0000000) >> 12) & 0x1FF;
        if (id_pde_idx == pae_pd_idx) {
            pae_write_pte_raw(pt_phys, id_pte_idx, ((uint64_t)pt_phys) | 3);
        } else {
            if (pae_ensure_phys_mapped(pt_phys) < 0) {
                printf("heap_map_page_pae: Failed to map page table page\n");
                return -1;
            }
        }
        __asm__ volatile(
            "mov %%cr3, %%eax\n\t"
            "mov %%eax, %%cr3\n\t"
            : : : "eax", "memory"
        );
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        pt_slot = pae_heap_pt_count++;
        pae_heap_page_tables[pt_slot] = pt;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    } else {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
    }

    if (!(pt[pae_pt_idx] & 1)) {
        phys_page = pmm_alloc_page();
        if (!phys_page) {
            printf("heap_map_page_pae: Failed to alloc phys page\n");
            return -1;
        }
        pt[pae_pt_idx] = ((uint64_t)(uint32_t)phys_page & ~0xFFFULL) | 3;
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return 0;
}

void vmm_unmap_page_pae(uint32_t virt_addr) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (pdpt_idx == 3 && pae_pd_idx < 4) {
        return;
    }

    pd = pae_get_pd_for_pdpt(pdpt_idx);
    if (!pd) {
        return;
    }

    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        return;
    }

    pt_phys = (uint32_t)(pde & ~0xFFFULL);
    pt = (uint64_t *)(pt_phys + 0xC0000000);
    pt[pae_pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

uint32_t vmm_create_pae_structure(void) {
    uint32_t slot;
    uint32_t pdpt_phys;
    uint32_t pd_phys[4];
    uint64_t *pdpt;
    uint64_t *new_pd;
    uint32_t i;
    uint32_t j;
    void *page;

    for (slot = 0; slot < PAE_PDPT_POOL_SIZE; slot++) {
        if (!pae_pdpt_pool_used[slot]) break;
    }
    if (slot >= PAE_PDPT_POOL_SIZE) {
        printf("vmm_create_pae_structure: PDPT pool exhausted\n");
        return 0;
    }

    for (i = 0; i < 4; i++) {
        page = pmm_alloc_low_page();
        if (!page) page = pmm_alloc_page();
        if (!page) {
            uint32_t k;
            for (k = 0; k < i; k++) {
                pfa_free((uint32_t)(uintptr_t)((uint8_t *)pae_pd_pool[slot][k] - 0xC0000000));
                pae_pd_pool[slot][k] = NULL;
            }
            printf("vmm_create_pae_structure: Failed to alloc PD page\n");
            return 0;
        }
        if (pae_ensure_phys_mapped((uint32_t)page) < 0) {
            uint32_t k;
            for (k = 0; k < i; k++) {
                pfa_free((uint32_t)(uintptr_t)((uint8_t *)pae_pd_pool[slot][k] - 0xC0000000));
                pae_pd_pool[slot][k] = NULL;
            }
            printf("vmm_create_pae_structure: Failed to map PD page\n");
            return 0;
        }
        pae_pd_pool[slot][i] = (uint64_t *)((uint32_t)page + 0xC0000000);
    }

    pae_pdpt_pool_used[slot] = 1;

    pdpt_phys = (uint32_t)((uintptr_t)&pae_pdpt_pool[slot] - 0xC0000000);
    for (i = 0; i < 4; i++) {
        pd_phys[i] = (uint32_t)((uintptr_t)pae_pd_pool[slot][i] - 0xC0000000);
    }

    pdpt = pae_pdpt_pool[slot];
    for (i = 0; i < 4; i++) {
        pdpt[i] = ((uint64_t)pd_phys[i] & ~0xFFFULL) | 1;
    }

    for (i = 0; i < 4; i++) {
        new_pd = pae_pd_pool[slot][i];
        memset(new_pd, 0, PAGE_SIZE);

        if (i == 3) {
            uint64_t entry;
            for (j = 0; j < 512; j++) {
                entry = boot_pd_high[j];
                if (entry & 1) {
                    new_pd[j] = entry & ~0x4ULL;
                }
            }
        }
    }

    return pdpt_phys;
}

uint8_t pae_pdpt_pool_used_get(uint32_t slot) {
    return pae_pdpt_pool_used[slot];
}

void pae_pdpt_pool_used_set(uint32_t slot, uint8_t value) {
    pae_pdpt_pool_used[slot] = value;
}

uint64_t *pae_pdpt_pool_get(uint32_t slot) {
    return pae_pdpt_pool[slot];
}

uint64_t *pae_pd_pool_get(uint32_t slot, uint32_t pdpt_idx) {
    return pae_pd_pool[slot][pdpt_idx];
}

int pae_pd_pool_alloc_slot(uint32_t slot) {
    uint32_t i;
    void *page;

    if (slot >= PAE_PDPT_POOL_SIZE) return -1;
    for (i = 0; i < 4; i++) {
        if (pae_pd_pool[slot][i]) continue;
        page = pmm_alloc_low_page();
        if (!page) page = pmm_alloc_page();
        if (!page) return -1;
        if (pae_ensure_phys_mapped((uint32_t)page) < 0) return -1;
        pae_pd_pool[slot][i] = (uint64_t *)((uint32_t)page + 0xC0000000);
    }
    return 0;
}

void pae_pd_pool_free_slot(uint32_t slot) {
    uint32_t i;
    uint32_t phys;

    if (slot >= PAE_PDPT_POOL_SIZE) return;
    for (i = 0; i < 4; i++) {
        if (pae_pd_pool[slot][i]) {
            phys = (uint32_t)((uintptr_t)pae_pd_pool[slot][i] - 0xC0000000);
            pfa_free(phys);
            pae_pd_pool[slot][i] = NULL;
        }
    }
}

uint32_t pae_pdpt_pool_size(void) {
    return PAE_PDPT_POOL_SIZE;
}

uint32_t pae_get_heap_pt_count(void) {
    return pae_heap_pt_count;
}

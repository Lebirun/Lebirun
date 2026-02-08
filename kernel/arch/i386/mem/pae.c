#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));
extern uint32_t pae_enabled;

#define PAE_PDPT_POOL_SIZE 4

static uint64_t pae_pdpt_pool[PAE_PDPT_POOL_SIZE][4] __attribute__((aligned(32)));
static uint64_t *pae_pd_pool[PAE_PDPT_POOL_SIZE][4];
static uint8_t pae_pdpt_pool_used[PAE_PDPT_POOL_SIZE];

static uint64_t *pae_heap_page_tables[4];
static uint32_t pae_heap_pt_count = 0;
uint64_t *pae_vmm_page_tables[32];
uint32_t pae_vmm_pt_count = 0;

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);
extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);

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

void vmm_map_page_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pae_pd_idx = (virt_addr >> 21) & 0x1FF;
    pae_pt_idx = (virt_addr >> 12) & 0x1FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    if (pdpt_idx == 3) {
        pd = boot_pd_high;
    } else {
        printf("vmm_map_page_pae: Low address mapping not supported yet (virt=0x%08X)\n", virt_addr);
        return;
    }

    pde = pd[pae_pd_idx];
    if (!(pde & 1)) {
        void *pt_page;
        if (pae_vmm_pt_count >= 32) {
            printf("vmm_map_page_pae: Out of VMM page tables\n");
            return;
        }
        pt_page = pmm_alloc_low_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            printf("vmm_map_page_pae: Failed to alloc page table\n");
            return;
        }
        pt_slot = pae_vmm_pt_count++;
        pt = (uint64_t *)((uint32_t)pt_page + 0xC0000000);
        pae_vmm_page_tables[pt_slot] = pt;
        memset(pt, 0, PAGE_SIZE);
        pt_phys = (uint32_t)pt_page;
        pd[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | (flags | 3);
        pae_sync_kernel_mappings();
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    } else {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        if ((flags & 0x4) && !(pd[pae_pd_idx] & 0x4)) {
            pd[pae_pd_idx] |= 0x4;
        }
    }

    pt[pae_pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
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

    if (pdpt_idx != 3) {
        return;
    }

    pd = boot_pd_high;
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

        if (pdpt_idx != 3) {
            printf("vmm_map_range_alloc_pae: Low address not supported (v=0x%08X)\n", v);
            return;
        }

        pd = boot_pd_high;
        pde = pd[pae_pd_idx];
        if (!(pde & 1)) {
            void *pt_page;
            if (pae_vmm_pt_count >= 32) {
                printf("vmm_map_range_alloc_pae: Out of VMM page tables\n");
                return;
            }
            pt_page = pmm_alloc_low_page();
            if (!pt_page) pt_page = pmm_alloc_page();
            if (!pt_page) {
                printf("vmm_map_range_alloc_pae: Failed to alloc page table\n");
                return;
            }
            pt_slot = pae_vmm_pt_count++;
            pt = (uint64_t *)((uint32_t)pt_page + 0xC0000000);
            pae_vmm_page_tables[pt_slot] = pt;
            memset(pt, 0, PAGE_SIZE);
            pt_phys = (uint32_t)pt_page;
            pd[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | (flags | 3);
            pae_sync_kernel_mappings();
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
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
        if (pae_heap_pt_count >= 4) {
            printf("heap_map_page_pae: Out of heap page tables\n");
            return -1;
        }
        pt_page = pmm_alloc_low_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            printf("heap_map_page_pae: Failed to alloc page table\n");
            return -1;
        }
        pt_slot = pae_heap_pt_count++;
        pt = (uint64_t *)((uint32_t)pt_page + 0xC0000000);
        pae_heap_page_tables[pt_slot] = pt;
        memset(pt, 0, PAGE_SIZE);
        pt_phys = (uint32_t)pt_page;
        boot_pd_high[pae_pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
        pae_sync_kernel_mappings();
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

    if (pdpt_idx != 3) {
        return;
    }

    pd = boot_pd_high;
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

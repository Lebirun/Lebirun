#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>


extern uint64_t kernel_pd_phys;
extern uint64_t kernel_pdpt_phys;
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void *pmm_alloc_early_page(void);
extern uint8_t *pfa_bitmap;
extern int pt_ensure_phys_mapped(uint64_t phys_addr);

#define TEMP_MAP_PAGES 8u

static uint64_t pt_temp_pt_ready = 0;
static uint64_t *pt_temp_pt = NULL;
static uint64_t pt_temp_pd_idx = 0;

uint64_t pt_temp_pt_ready_check(void) {
    return pt_temp_pt_ready;
}

extern void pt_sync_kernel_mappings(void);
extern uint64_t **pt_vmm_page_tables;
extern uint64_t pt_vmm_pt_count;
extern uint64_t pt_vmm_pt_capacity;
extern uint64_t boot_pd_high[];

#define kv_pd_high ((uint64_t *)((uintptr_t)boot_pd_high + KERNEL_VMA))

void pt_init_temp_mapping(void) {
    uint64_t pd_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pt_phys;
    uint64_t pt_slot;

    if (pt_temp_pt_ready) return;

    pd_idx = (TEMP_MAP_BASE >> 21) & 0x1FF;
    pd = kv_pd_high;
    pde = pd[pd_idx];

    if ((pde & 1) && !(pde & 0x80)) {
        pt_phys = (uint64_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
        pt_temp_pt = pt;
        pt_temp_pd_idx = pd_idx;
        pt_temp_pt_ready = 1;
        return;
    }

    if (pt_vmm_pt_count >= pt_vmm_pt_capacity) {
        printf("pt_init_temp_mapping: out of vmm page tables\n");
        return;
    }

    {
        void *pt_page;
        if (pfa_bitmap)
            pt_page = pmm_alloc_page();
        else
            pt_page = pmm_alloc_early_page();
        if (!pt_page) {
            printf("pt_init_temp_mapping: failed to alloc page table\n");
            return;
        }
        pt_phys = (uint64_t)pt_page;
        pt = (uint64_t *)(pt_phys + KERNEL_VMA);
        memset(pt, 0, PAGE_SIZE);
        pt_slot = pt_vmm_pt_count++;
        pt_vmm_page_tables[pt_slot] = pt;
    }
    pd[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
    pt_sync_kernel_mappings();

    pt_temp_pt = pt;
    pt_temp_pd_idx = pd_idx;
    pt_temp_pt_ready = 1;
}

void temp_map_raw_pae(uint64_t temp_virt, uint64_t phys_addr) {
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t *pt;

    pd_idx = (temp_virt >> 21) & 0x1FF;
    pt_idx = (temp_virt >> 12) & 0x1FF;

    if (!pt_temp_pt_ready) {
        pt_init_temp_mapping();
    }
    if (!pt_temp_pt_ready || pd_idx != pt_temp_pd_idx) {
        return;
    }

    pt = pt_temp_pt;
    pt[pt_idx] = (phys_addr & ~0xFFFULL) | 3;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");
}

void temp_unmap_raw_pae(uint64_t temp_virt) {
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t *pt;

    pd_idx = (temp_virt >> 21) & 0x1FF;
    pt_idx = (temp_virt >> 12) & 0x1FF;

    if (!pt_temp_pt_ready || pd_idx != pt_temp_pd_idx) {
        return;
    }

    pt = pt_temp_pt;
    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");
}

void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr) {
    temp_map_raw_pae(temp_virt, (uint64_t)phys_addr);
}

void temp_unmap_raw(uint64_t temp_virt) {
    temp_unmap_raw_pae(temp_virt);
}

void vmm_temp_map_raw(uint64_t temp_virt, uint64_t phys_addr) {
    temp_map_raw_pae(temp_virt, phys_addr);
}

void vmm_temp_unmap_raw(uint64_t temp_virt) {
    temp_unmap_raw(temp_virt);
}

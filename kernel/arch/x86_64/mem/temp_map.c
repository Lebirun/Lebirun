#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <string.h>


extern uint64_t kernel_pd_phys;
extern uint64_t kernel_pdpt_phys;
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_mapped_page(void);
extern void *pmm_alloc_low_page(void);
extern void *pmm_alloc_early_page(void);
extern uint8_t *pfa_bitmap;
extern int pt_ensure_phys_mapped(uint64_t phys_addr);

static uint64_t pt_temp_pt_ready = 0;

uint64_t pt_temp_pt_ready_check(void) {
    return pt_temp_pt_ready;
}

extern void pt_sync_kernel_mappings(void);
extern uint64_t pt_vmm_pt_count;
extern uint64_t boot_pd_high[];

#define kv_pd_high ((uint64_t *)((uintptr_t)boot_pd_high + KERNEL_VMA))

static uint64_t *pt_temp_table(uint64_t temp_virt) {
    uint64_t pde;

    if (temp_virt < TEMP_MAP_BASE || temp_virt >= TEMP_MAP_END) return NULL;
    pde = kv_pd_high[(temp_virt >> 21) & 0x1FF];
    if (!(pde & 1) || (pde & 0x80)) return NULL;
    return (uint64_t *)((pde & ~0xFFFULL) + KERNEL_VMA);
}

int pt_temp_prepare_cpu(int cpu) {
    uint64_t virt;
    uint64_t pt_phys;
    void *pt_page;

    virt = TEMP_MAP_BASE + (uint64_t)cpu * TEMP_MAP_PAGES_PER_CPU * PAGE_SIZE;
    if (virt >= TEMP_MAP_END) return -1;
    if (pt_temp_table(virt)) return 0;
    pt_page = pfa_bitmap ? pmm_alloc_mapped_page() : pmm_alloc_early_page();
    if (!pt_page) {
        printf("pt_temp_prepare_cpu: failed to alloc page table\n");
        return -1;
    }
    pt_phys = (uint64_t)pt_page;
    memset((void *)(pt_phys + KERNEL_VMA), 0, PAGE_SIZE);
    pt_vmm_pt_count++;
    kv_pd_high[(virt >> 21) & 0x1FF] = (pt_phys & ~0xFFFULL) | 3;
    pt_sync_kernel_mappings();
    __asm__ volatile ("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");
    return 0;
}

void pt_init_temp_mapping(void) {
    if (pt_temp_pt_ready) return;
    if (pt_temp_prepare_cpu(0) == 0) pt_temp_pt_ready = 1;
}

void temp_map_raw_pae(uint64_t temp_virt, uint64_t phys_addr) {
    uint64_t *pt;
    uint64_t old;

    if (!pt_temp_pt_ready) pt_init_temp_mapping();
    pt = pt_temp_table(temp_virt);
    if (!pt) return;
    old = pt[(temp_virt >> 12) & 0x1FF];
    pt[(temp_virt >> 12) & 0x1FF] = (phys_addr & ~0xFFFULL) | 3;
    if (old & 1)
        __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");
}

void temp_unmap_raw_pae(uint64_t temp_virt) {
    uint64_t *pt;

    pt = pt_temp_table(temp_virt);
    if (!pt) return;
    pt[(temp_virt >> 12) & 0x1FF] = 0;
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

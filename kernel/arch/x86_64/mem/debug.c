#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <string.h>

extern uint64_t boot_pd_high[];
extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);

#define kv_pd_high ((uint64_t *)((uintptr_t)boot_pd_high + KERNEL_VMA))

void vmm_debug_page(uint64_t virt_addr) {
    (void)virt_addr;
}

void dump_pml4_for_virt(uint64_t virt_addr) {
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t pde;
    uint64_t pte;
    uint64_t pt_phys;
    uint64_t temp_virt;

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;
    if (pdpt_idx != 3) {
        printf("dump_pml4_for_virt: virt=0x%016lX not in kernel space\n", virt_addr);
        return;
    }
    pde = kv_pd_high[pd_idx];
    if (!(pde & 1)) {
        printf("dump_pml4_for_virt: virt=0x%016lX pd_idx=%lu pd_entry=0x%016llX (not present)\n",
               virt_addr, pd_idx, (unsigned long long)pde);
        return;
    }
    pt_phys = (uint64_t)(pde & ~0xFFFULL);
    {
        uint64_t sf;
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(sf) :: "memory");
        temp_virt = TEMP_SLOT(0);
        temp_map_raw(temp_virt, pt_phys);
        pte = ((uint64_t *)temp_virt)[pt_idx];
        temp_unmap_raw(temp_virt);
        if (sf & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    }
    printf("dump_pml4_for_virt: virt=0x%016lX pd_idx=%lu pt_idx=%lu pd_entry=0x%016llX pt_entry=0x%016llX\n",
           virt_addr, pd_idx, pt_idx, (unsigned long long)pde, (unsigned long long)pte);
}

void vmm_dump_for_pml4(uint64_t pd_phys, uint64_t virt_addr) {
    uint64_t sf;
    if (pd_phys == 0) {
        return;
    }
    uint64_t pd_idx = virt_addr >> 22;
    uint64_t pt_idx = (virt_addr >> 12) & 0x3FF;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(sf) :: "memory");

    uint64_t temp_pd_virt = TEMP_SLOT(0);
    temp_map_raw(temp_pd_virt, pd_phys);
    uint64_t *foreign_pd = (uint64_t *)temp_pd_virt;
    uint64_t pd_entry = foreign_pd[pd_idx];
    if (!(pd_entry & 1)) {
        temp_unmap_raw(temp_pd_virt);
        if (sf & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return;
    }
    uint64_t pt_phys = pd_entry & ~0xFFF;
    temp_unmap_raw(temp_pd_virt);

    uint64_t temp_pt_virt = TEMP_SLOT(1);
    temp_map_raw(temp_pt_virt, pt_phys);
    uint64_t *foreign_pt = (uint64_t *)temp_pt_virt;
    uint64_t pt_entry = foreign_pt[pt_idx];
    temp_unmap_raw(temp_pt_virt);

    if (sf & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    printf("vmm_dump_for_pml4: virt=0x%016lX pd_entry=0x%016lX pt_entry=0x%016lX\n", virt_addr, pd_entry, pt_entry);
}

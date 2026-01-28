#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;
extern uint64_t boot_pd_high[];
extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);

void vmm_debug_page(uint32_t virt_addr) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t pte;

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pae_pd_idx = (virt_addr >> 21) & 0x1FF;
        pae_pt_idx = (virt_addr >> 12) & 0x1FF;
        if (pdpt_idx != 3) {
            if (debugMode && debugLevel >= 5) printf("VMM debug 0x%08X: not in kernel space (PDPT[%u])\n", virt_addr, pdpt_idx);
            return;
        }
        pde = boot_pd_high[pae_pd_idx];
        if (debugMode && debugLevel >= 5) printf("VMM debug 0x%08X: PD[%u]=0x%016llX", virt_addr, pae_pd_idx, (unsigned long long)pde);
        if (pde & 1) {
            uint32_t pt_phys = (uint32_t)(pde & ~0xFFFULL);
            uint32_t temp_virt = 0xF7000000;
            temp_map_raw(temp_virt, pt_phys);
            uint64_t *pt = (uint64_t *)temp_virt;
            pte = pt[pae_pt_idx];
            temp_unmap_raw(temp_virt);
            if (debugMode && debugLevel >= 5) printf(" PT[%u]=0x%016llX", pae_pt_idx, (unsigned long long)pte);
            if (pte & 1) {
                if (debugMode && debugLevel >= 5) printf(" (P%s%s)", (pte & 2) ? "W" : "R", (pte & 4) ? "U" : "S");
            }
        }
        if (debugMode && debugLevel >= 5) printf("\n");
        return;
    }

    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;
    uint32_t *pd = (uint32_t *)0xFFFFF000;
    
    if (debugMode && debugLevel >= 5) printf("VMM debug 0x%08X: PD[%u]=0x%08X", virt_addr, pd_idx, pd[pd_idx]);
    if (pd[pd_idx] & 1) {
        uint32_t *pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
        if (debugMode && debugLevel >= 5) printf(" PT[%u]=0x%08X", pt_idx, pt[pt_idx]);
        if (pt[pt_idx] & 1) {
            if (debugMode && debugLevel >= 5) printf(" (P%s%s)", 
                   (pt[pt_idx] & 2) ? "W" : "R",
                   (pt[pt_idx] & 4) ? "U" : "S");
        }
    }
    if (debugMode && debugLevel >= 5) printf("\n");
}

void dump_pd_pt_for_virt(uint32_t virt_addr) {
    uint32_t pdpt_idx;
    uint32_t pae_pd_idx;
    uint32_t pae_pt_idx;
    uint64_t pde;
    uint64_t pte;

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pae_pd_idx = (virt_addr >> 21) & 0x1FF;
        pae_pt_idx = (virt_addr >> 12) & 0x1FF;
        if (pdpt_idx != 3) {
            printf("dump_pd_pt_for_virt: virt=0x%08X not in kernel space\n", virt_addr);
            return;
        }
        pde = boot_pd_high[pae_pd_idx];
        if (!(pde & 1)) {
            printf("dump_pd_pt_for_virt: virt=0x%08X pd_idx=%u pd_entry=0x%016llX (not present)\n",
                   virt_addr, pae_pd_idx, (unsigned long long)pde);
            return;
        }
        uint32_t pt_phys = (uint32_t)(pde & ~0xFFFULL);
        uint32_t temp_virt = 0xF7000000;
        temp_map_raw(temp_virt, pt_phys);
        pte = ((uint64_t *)temp_virt)[pae_pt_idx];
        temp_unmap_raw(temp_virt);
        printf("dump_pd_pt_for_virt: virt=0x%08X pd_idx=%u pt_idx=%u pd_entry=0x%016llX pt_entry=0x%016llX\n",
               virt_addr, pae_pd_idx, pae_pt_idx, (unsigned long long)pde, (unsigned long long)pte);
        return;
    }

    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;
    uint32_t pd_entry = ((uint32_t *)0xFFFFF000)[pd_idx];
    uint32_t *pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    uint32_t pt_entry = pt[pt_idx];
    printf("dump_pd_pt_for_virt: virt=0x%08X pd_idx=%u pt_idx=%u pd_entry=0x%08X pt_entry=0x%08X\n", virt_addr, pd_idx, pt_idx, pd_entry, pt_entry);
}

void vmm_dump_for_pd(uint32_t pd_phys, uint32_t virt_addr) {
    if (pd_phys == 0) {
        return;
    }
    uint32_t pd_idx = virt_addr >> 22;
    uint32_t pt_idx = (virt_addr >> 12) & 0x3FF;

    uint32_t temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    uint32_t *foreign_pd = (uint32_t *)temp_pd_virt;
    uint32_t pd_entry = foreign_pd[pd_idx];
    if (!(pd_entry & 1)) {
        temp_unmap_raw(temp_pd_virt);
        return;
    }
    uint32_t pt_phys = pd_entry & ~0xFFF;
    temp_unmap_raw(temp_pd_virt);

    uint32_t temp_pt_virt = 0xF7001000;
    temp_map_raw(temp_pt_virt, pt_phys);
    uint32_t *foreign_pt = (uint32_t *)temp_pt_virt;
    uint32_t pt_entry = foreign_pt[pt_idx];
    temp_unmap_raw(temp_pt_virt);

    printf("vmm_dump_for_pd: virt=0x%08X pd_entry=0x%08X pt_entry=0x%08X\n", virt_addr, pd_entry, pt_entry);
}

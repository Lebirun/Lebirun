#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;

extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);
extern void temp_map_raw_pae(uint32_t temp_virt, uint64_t phys_addr);
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);
extern uint32_t pae_pdpt_pool_size(void);
extern uint8_t pae_pdpt_pool_used_get(uint32_t slot);
extern uint64_t *pae_pdpt_pool_get(uint32_t slot);
extern uint64_t *pae_pd_pool_get(uint32_t slot, uint32_t pdpt_idx);
extern uint32_t pfa_alloc(void);

static uint32_t get_page_phys_in_pd(uint32_t pd_phys, uint32_t virt_addr) {
    uint32_t result;

    result = vmm_get_phys_in_pd(pd_phys, virt_addr);
    return result;
}

uint32_t vmm_get_phys_in_pd(uint32_t pd_phys, uint32_t virt_addr) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t temp_pd_virt;
    uint32_t temp_pt_virt;
    uint32_t pd_entry;
    uint32_t pt_phys;
    uint32_t pt_entry;
    uint32_t *foreign_pd;
    uint32_t *foreign_pt;
    uint32_t pdpt_idx;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;
    uint64_t *pd64;
    uint64_t *pt64;
    uint32_t temp_pdpt_virt;
    uint32_t pool_slot;
    int use_direct_access;

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pd_idx = (virt_addr >> 21) & 0x1FF;
        pt_idx = (virt_addr >> 12) & 0x1FF;

        use_direct_access = 0;
        pool_slot = 0;
        for (pool_slot = 0; pool_slot < pae_pdpt_pool_size(); pool_slot++) {
            uint32_t pool_phys = (uint32_t)((uintptr_t)pae_pdpt_pool_get(pool_slot) - 0xC0000000);
            if (pool_phys == pd_phys && pae_pdpt_pool_used_get(pool_slot)) {
                use_direct_access = 1;
                break;
            }
        }

        if (use_direct_access) {
            pdpt = pae_pdpt_pool_get(pool_slot);
            pdpte = pdpt[pdpt_idx];

            if (!(pdpte & 1)) {
                return 0;
            }

            pd64 = pae_pd_pool_get(pool_slot, pdpt_idx);
            pde = pd64[pd_idx];

            if (!(pde & 1)) {
                return 0;
            }

            temp_pt_virt = 0xF7002000;
            temp_map_raw(temp_pt_virt, (uint32_t)(pde & ~0xFFFULL));
            pt64 = (uint64_t *)temp_pt_virt;
            pte = pt64[pt_idx];
            temp_unmap_raw(temp_pt_virt);

            if (!(pte & 1)) {
                return 0;
            }

            return (uint32_t)(pte & ~0xFFFULL);
        }

        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)(temp_pdpt_virt + (pd_phys & 0xFFF));
        pdpte = pdpt[pdpt_idx];
        temp_unmap_raw(temp_pdpt_virt);

        if (!(pdpte & 1)) {
            return 0;
        }

        temp_pd_virt = 0xF7001000;
        temp_map_raw(temp_pd_virt, (uint32_t)(pdpte & ~0xFFFULL));
        pd64 = (uint64_t *)temp_pd_virt;
        pde = pd64[pd_idx];
        temp_unmap_raw(temp_pd_virt);

        if (!(pde & 1)) {
            return 0;
        }

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, (uint32_t)(pde & ~0xFFFULL));
        pt64 = (uint64_t *)temp_pt_virt;
        pte = pt64[pt_idx];
        temp_unmap_raw(temp_pt_virt);

        if (!(pte & 1)) {
            return 0;
        }

        return (uint32_t)(pte & ~0xFFFULL);
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    foreign_pd = (uint32_t *)temp_pd_virt;
    pd_entry = foreign_pd[pd_idx];
    temp_unmap_raw(temp_pd_virt);

    if (!(pd_entry & 1)) {
        return 0;
    }

    pt_phys = pd_entry & ~0xFFF;
    temp_pt_virt = 0xF7001000;
    temp_map_raw(temp_pt_virt, pt_phys);
    foreign_pt = (uint32_t *)temp_pt_virt;
    pt_entry = foreign_pt[pt_idx];
    temp_unmap_raw(temp_pt_virt);

    if (!(pt_entry & 1)) {
        return 0;
    }

    return pt_entry & ~0xFFF;
}

void vmm_map_page_in_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t temp_pd_virt;
    uint32_t temp_pt_virt;
    uint32_t temp_pdpt_virt;
    uint32_t pd_entry;
    uint32_t pt_phys;
    uint32_t *foreign_pd;
    uint32_t *foreign_pt;
    uint32_t cur_cr3;
    void *pt_page;
    uint32_t pdpt_idx;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t *pd64;
    uint64_t pde;
    uint64_t *pt64;
    uint32_t pool_slot;
    int use_direct_access;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    if (pae_enabled) {
        pdpt_idx = (virt_addr >> 30) & 0x3;
        pd_idx = (virt_addr >> 21) & 0x1FF;
        pt_idx = (virt_addr >> 12) & 0x1FF;

        use_direct_access = 0;
        pool_slot = 0;
        for (pool_slot = 0; pool_slot < pae_pdpt_pool_size(); pool_slot++) {
            uint32_t pool_phys = (uint32_t)((uintptr_t)pae_pdpt_pool_get(pool_slot) - 0xC0000000);
            if (pool_phys == pd_phys && pae_pdpt_pool_used_get(pool_slot)) {
                use_direct_access = 1;
                break;
            }
        }

        if (use_direct_access) {
            uint32_t pde_flags;

            pdpt = pae_pdpt_pool_get(pool_slot);
            pdpte = pdpt[pdpt_idx];

            if (!(pdpte & 1)) {
                printf("vmm_map_page_in_pd: PDPT entry not present\n");
                return;
            }

            pd64 = pae_pd_pool_get(pool_slot, pdpt_idx);
            pde = pd64[pd_idx];

            pde_flags = (flags & 0x7) | 1;

            if (!(pde & 1)) {
                pt_page = pmm_alloc_low_page();
                if (!pt_page) {
                    pt_page = pmm_alloc_page();
                }
                if (!pt_page) {
                    printf("vmm_map_page_in_pd: Failed to alloc PAE PT\n");
                    return;
                }
                pt_phys = (uint32_t)pt_page;
                pmm_zero_page_phys(pt_phys);
                pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_flags;
            } else {
                pt_phys = (uint32_t)(pde & ~0xFFFULL);
                if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                    pd64[pd_idx] |= (flags & 0x7);
                }
            }

            temp_pt_virt = 0xF7002000;
            temp_map_raw(temp_pt_virt, pt_phys);
            pt64 = (uint64_t *)temp_pt_virt;
            pt64[pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
            temp_unmap_raw(temp_pt_virt);

            __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
            if (cur_cr3 == pd_phys) {
                __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
            }
            return;
        }

        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)(temp_pdpt_virt + (pd_phys & 0xFFF));
        pdpte = pdpt[pdpt_idx];
        temp_unmap_raw(temp_pdpt_virt);

        if (!(pdpte & 1)) {
            printf("vmm_map_page_in_pd: PDPT entry not present\n");
            return;
        }

        {
            uint32_t pde_flags_nd;

            pde_flags_nd = (flags & 0x7) | 1;

            temp_pd_virt = 0xF7001000;
            temp_map_raw(temp_pd_virt, (uint32_t)(pdpte & ~0xFFFULL));
            pd64 = (uint64_t *)temp_pd_virt;
            pde = pd64[pd_idx];

            if (!(pde & 1)) {
                pt_page = pmm_alloc_low_page();
                if (!pt_page) {
                    pt_page = pmm_alloc_page();
                }
                if (!pt_page) {
                    printf("vmm_map_page_in_pd: Failed to alloc PAE PT\n");
                    temp_unmap_raw(temp_pd_virt);
                    return;
                }
                pt_phys = (uint32_t)pt_page;
                pmm_zero_page_phys(pt_phys);
                pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_flags_nd;
            } else {
                pt_phys = (uint32_t)(pde & ~0xFFFULL);
                if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                    pd64[pd_idx] |= (flags & 0x7);
                }
            }
            temp_unmap_raw(temp_pd_virt);
        }

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, pt_phys);
        pt64 = (uint64_t *)temp_pt_virt;
        pt64[pt_idx] = ((uint64_t)phys_addr & ~0xFFFULL) | (flags & 0xFFF);
        temp_unmap_raw(temp_pt_virt);

        __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == pd_phys) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    foreign_pd = (uint32_t *)temp_pd_virt;
    pd_entry = foreign_pd[pd_idx];

    if (!(pd_entry & 1)) {
            uint32_t pde_32_flags;

            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_page_in_pd: Failed to alloc PT\n");
                temp_unmap_raw(temp_pd_virt);
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pmm_zero_page_phys(pt_phys);
            pde_32_flags = (flags & 0x7) | 1;
            foreign_pd[pd_idx] = (pt_phys & ~0xFFF) | pde_32_flags;
    } else {
            pt_phys = pd_entry & ~0xFFF;
            if ((flags & 0x7) & ~(foreign_pd[pd_idx] & 0x7)) {
                foreign_pd[pd_idx] |= (flags & 0x7);
            }
    }

    temp_unmap_raw(temp_pd_virt);

    temp_pt_virt = 0xF7001000;
    temp_map_raw(temp_pt_virt, pt_phys);
    foreign_pt = (uint32_t *)temp_pt_virt;

    foreign_pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF);

    temp_unmap_raw(temp_pt_virt);

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == pd_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
}

void vmm_map_page_in_pd64(uint32_t pd_phys, uint32_t virt_addr, uint64_t phys_addr, uint32_t flags) {
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t temp_pdpt_virt;
    uint32_t temp_pd_virt;
    uint32_t temp_pt_virt;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t *pd64;
    uint64_t pde;
    uint64_t *pt64;
    void *pt_page;
    uint32_t pt_phys;
    uint32_t cur_cr3;
    uint32_t pool_slot;
    int use_direct_access;

    if (!pae_enabled) {
        vmm_map_page_in_pd(pd_phys, virt_addr, (uint32_t)phys_addr, flags);
        return;
    }

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pdpt_idx = (virt_addr >> 30) & 0x3;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;

    use_direct_access = 0;
    pool_slot = 0;
    for (pool_slot = 0; pool_slot < pae_pdpt_pool_size(); pool_slot++) {
        uint32_t pool_phys = (uint32_t)((uintptr_t)pae_pdpt_pool_get(pool_slot) - 0xC0000000);
        if (pool_phys == pd_phys && pae_pdpt_pool_used_get(pool_slot)) {
            use_direct_access = 1;
            break;
        }
    }

    if (use_direct_access) {
        uint32_t pde_64_flags;

        pdpt = pae_pdpt_pool_get(pool_slot);
        pdpte = pdpt[pdpt_idx];

        if (!(pdpte & 1)) {
            printf("vmm_map_page_in_pd64: PDPT entry not present\n");
            return;
        }

        pd64 = pae_pd_pool_get(pool_slot, pdpt_idx);
        pde = pd64[pd_idx];

        pde_64_flags = (flags & 0x7) | 1;

        if (!(pde & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_page_in_pd64: Failed to alloc PAE PT\n");
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pmm_zero_page_phys(pt_phys);
            pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_64_flags;
        } else {
            pt_phys = (uint32_t)(pde & ~0xFFFULL);
            if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                pd64[pd_idx] |= (flags & 0x7);
            }
        }

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, pt_phys);
        pt64 = (uint64_t *)temp_pt_virt;
        pt64[pt_idx] = (phys_addr & ~0xFFFULL) | (flags & 0xFFF);
        temp_unmap_raw(temp_pt_virt);

        __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == pd_phys) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
        return;
    }

    {
        uint32_t pde_64nd_flags;

        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)(temp_pdpt_virt + (pd_phys & 0xFFF));
        pdpte = pdpt[pdpt_idx];
        temp_unmap_raw(temp_pdpt_virt);

        if (!(pdpte & 1)) {
            printf("vmm_map_page_in_pd64: PDPT entry not present\n");
            return;
        }

        pde_64nd_flags = (flags & 0x7) | 1;

        temp_pd_virt = 0xF7001000;
        temp_map_raw(temp_pd_virt, (uint32_t)(pdpte & ~0xFFFULL));
        pd64 = (uint64_t *)temp_pd_virt;
        pde = pd64[pd_idx];

        if (!(pde & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_page_in_pd64: Failed to alloc PAE PT\n");
                temp_unmap_raw(temp_pd_virt);
                return;
            }
            pt_phys = (uint32_t)pt_page;
            pmm_zero_page_phys(pt_phys);
            pd64[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | pde_64nd_flags;
        } else {
            pt_phys = (uint32_t)(pde & ~0xFFFULL);
            if ((flags & 0x7) & ~(pd64[pd_idx] & 0x7)) {
                pd64[pd_idx] |= (flags & 0x7);
            }
        }
        temp_unmap_raw(temp_pd_virt);

        temp_pt_virt = 0xF7002000;
        temp_map_raw(temp_pt_virt, pt_phys);
        pt64 = (uint64_t *)temp_pt_virt;
        pt64[pt_idx] = (phys_addr & ~0xFFFULL) | (flags & 0xFFF);
        temp_unmap_raw(temp_pt_virt);

        __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == pd_phys) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
    }
}

void vmm_map_range_in_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;
    uint32_t v;
    uint32_t phys_page;

    if (size == 0) return;

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pd: Failed to alloc phys page\n");
            return;
        }
        pmm_zero_page_phys(phys_page);
        vmm_map_page_in_pd(pd_phys, v, phys_page, flags);
    }
}

uint32_t* vmm_map_range_in_pd_tracked(uint32_t pd_phys, uint32_t virt_addr, uint32_t size, uint32_t flags, uint32_t *out_count) {
    uint32_t start;
    uint32_t end;
    uint32_t num_pages;
    uint32_t *pages;
    uint32_t idx;
    uint32_t v;
    uint32_t phys_page;
    uint32_t i;

    if (size == 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    num_pages = (end - start) / PAGE_SIZE;

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }
    
    pages = (uint32_t *)kmalloc(num_pages * sizeof(uint32_t));
    if (!pages) {
        printf("vmm_map_range_in_pd_tracked: kmalloc failed for %u pages (%u bytes)\n", num_pages, num_pages * 4);
        if (out_count) *out_count = 0;
        return NULL;
    }

    idx = 0;
    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pd_tracked: Failed to alloc phys page %u/%u (free=%u)\n", idx, num_pages, pfa_count_free());
            for (i = 0; i < idx; i++) {
                pfa_free(pages[i]);
            }
            kfree(pages);
            if (out_count) *out_count = 0;
            return NULL;
        }
        pmm_zero_page_phys(phys_page);
        pages[idx++] = phys_page;
        vmm_map_page_in_pd(pd_phys, v, phys_page, flags);
    }
    
    if (out_count) *out_count = num_pages;
    return pages;
}

void vmm_copy_to_pd(uint32_t pd_phys, uint32_t dest_virt, const void *src, uint32_t size) {
    const uint8_t *s = (const uint8_t *)src;
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t page_offset = (dest_virt + offset) & 0xFFF;
        uint32_t chunk = PAGE_SIZE - page_offset;
        if (chunk > size - offset) chunk = size - offset;

        uint32_t virt_page = (dest_virt + offset) & ~0xFFF;
        uint32_t page_phys = get_page_phys_in_pd(pd_phys, virt_page);
        if (page_phys == 0) {
            printf("vmm_copy_to_pd: page not mapped for 0x%08X\n", virt_page);
            return;
        }

        temp_map_raw(0xF7003000, page_phys);
        memcpy((void *)(0xF7003000 + page_offset), s + offset, chunk);
        temp_unmap_raw(0xF7003000);

        offset += chunk;
    }
}

void vmm_read_from_pd(uint32_t pd_phys, uint32_t src_virt, void *dest, uint32_t size) {
    uint8_t *d = (uint8_t *)dest;
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t page_offset = (src_virt + offset) & 0xFFF;
        uint32_t chunk = PAGE_SIZE - page_offset;
        if (chunk > size - offset) chunk = size - offset;

        uint32_t virt_page = (src_virt + offset) & ~0xFFF;
        uint32_t page_phys = get_page_phys_in_pd(pd_phys, virt_page);
        if (page_phys == 0) return;

        temp_map_raw(0xF7003000, page_phys);
        memcpy(d + offset, (void *)(0xF7003000 + page_offset), chunk);
        temp_unmap_raw(0xF7003000);

        offset += chunk;
    }
}

#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;
extern uint64_t boot_pd_high[];

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);
extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);
extern void temp_map_raw_pae(uint32_t temp_virt, uint64_t phys_addr);
extern uint32_t pfa_alloc(void);
extern uint64_t pfa_alloc64(void);
extern void pfa_free(uint32_t phys_addr);
extern void pfa_free64(uint64_t phys_addr);
extern uint32_t vmm_create_pae_structure(void);
extern uint32_t pae_pdpt_pool_size(void);
extern uint8_t pae_pdpt_pool_used_get(uint32_t slot);
extern void pae_pdpt_pool_used_set(uint32_t slot, uint8_t value);
extern uint64_t *pae_pdpt_pool_get(uint32_t slot);
extern uint64_t *pae_pd_pool_get(uint32_t slot, uint32_t pdpt_idx);

static inline bool clone_should_log_detail(uint32_t index) {
    return index < 2 || (index & 0x3FF) == 0;
}

static inline bool clone_should_log_sample(uint32_t index) {
    return index == 0;
}

uint32_t vmm_create_page_directory(void) {
    void *pd_page;
    uint32_t pd_phys;
    uint32_t temp_virt;
    uint32_t *new_pd;
    uint32_t *cur_pd;
    uint32_t i;

    if (pae_enabled) {
        return vmm_create_pae_structure();
    }

    pd_page = pmm_alloc_low_page();
    if (!pd_page) {
        pd_page = pmm_alloc_page();
    }
    if (!pd_page) {
        printf("vmm_create_page_directory: Failed to allocate PD page\n");
        return 0;
    }
    pd_phys = (uint32_t)pd_page;
    pmm_zero_page_phys(pd_phys);

    temp_virt = 0xF7000000;
    temp_map_raw(temp_virt, pd_phys);

    new_pd = (uint32_t *)temp_virt;
    memset(new_pd, 0, PAGE_SIZE);

    cur_pd = (uint32_t *)0xFFFFF000;
    for (i = 768; i < 1023; i++) {
        new_pd[i] = cur_pd[i] & ~0x4;
    }

    new_pd[1023] = (pd_phys & ~0xFFF) | 3;

    temp_unmap_raw(temp_virt);

    return pd_phys;
}

void vmm_free_page_directory(uint32_t pd_phys) {
    uint32_t temp_pd_virt;
    uint32_t temp_pdpt_virt;
    uint32_t temp_pt_virt;
    uint32_t *pd;
    uint64_t *pdpt;
    uint64_t *pd64;
    uint64_t *pt64;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    uint32_t slot;
    uint32_t pool_phys;
    uint64_t pde;
    uint32_t pt_phys_val;
    uint64_t pte;
    uint64_t pdpte;
    uint32_t pd_phys_entry;

    if (!pd_phys) return;

    if (pae_enabled) {
        for (slot = 0; slot < pae_pdpt_pool_size(); slot++) {
            pool_phys = (uint32_t)((uintptr_t)pae_pdpt_pool_get(slot) - 0xC0000000);
            if (pool_phys == pd_phys) {
                if (pae_pdpt_pool_used_get(slot)) {
                    if (debugMode && debugLevel >= 3) printf("vmm_free_pd: freeing PAE pool slot %u pd=0x%08X\n", slot, pd_phys);
                    for (i = 0; i < 3; i++) {
                        pd64 = pae_pd_pool_get(slot, i);
                        for (j = 0; j < 512; j++) {
                            pde = pd64[j];
                            if (!(pde & 1)) continue;
                            pt_phys_val = (uint32_t)(pde & ~0xFFFULL);
                            temp_pt_virt = 0xF7002000;
                            temp_map_raw(temp_pt_virt, pt_phys_val);
                            pt64 = (uint64_t *)temp_pt_virt;
                            for (k = 0; k < 512; k++) {
                                pte = pt64[k];
                                if (pte & 1) {
                                    pfa_free64(pte & ~0xFFFULL);
                                }
                            }
                            temp_unmap_raw(temp_pt_virt);
                            pfa_free(pt_phys_val);
                        }
                        memset(pd64, 0, PAGE_SIZE);
                    }
                    pae_pdpt_pool_used_set(slot, 0);
                    return;
                } else {
                    printf("vmm_free_pd: WARNING: PAE pool slot %u already free! pd=0x%08X\n", slot, pd_phys);
                    return;
                }
            }
        }

        printf("vmm_free_pd: PAE pd=0x%08X not in pool, treating as dynamic\n", pd_phys);
        temp_pdpt_virt = 0xF7000000;
        temp_map_raw(temp_pdpt_virt, pd_phys);
        pdpt = (uint64_t *)temp_pdpt_virt;

        for (i = 0; i < 4; i++) {
            pdpte = pdpt[i];
            if (!(pdpte & 1)) continue;
            if (i >= 3) continue;

            pd_phys_entry = (uint32_t)(pdpte & ~0xFFFULL);
            temp_pd_virt = 0xF7001000;
            temp_map_raw(temp_pd_virt, pd_phys_entry);
            pd64 = (uint64_t *)temp_pd_virt;

            for (j = 0; j < 512; j++) {
                pde = pd64[j];
                if (!(pde & 1)) continue;

                pt_phys_val = (uint32_t)(pde & ~0xFFFULL);
                temp_pt_virt = 0xF7002000;
                temp_map_raw(temp_pt_virt, pt_phys_val);
                pt64 = (uint64_t *)temp_pt_virt;

                for (k = 0; k < 512; k++) {
                    pte = pt64[k];
                    if (pte & 1) {
                        pfa_free64(pte & ~0xFFFULL);
                    }
                }
                temp_unmap_raw(temp_pt_virt);
                pfa_free(pt_phys_val);
            }
            temp_unmap_raw(temp_pd_virt);
            pfa_free(pd_phys_entry);
        }
        temp_unmap_raw(temp_pdpt_virt);
        pfa_free(pd_phys);
        return;
    }

    temp_pd_virt = 0xF7000000;
    temp_map_raw(temp_pd_virt, pd_phys);
    pd = (uint32_t *)temp_pd_virt;

    for (i = 0; i < 768; i++) {
        if (pd[i] & 1) {
            pt_phys_val = pd[i] & ~0xFFF;
            pfa_free(pt_phys_val);
        }
    }

    temp_unmap_raw(temp_pd_virt);

    pfa_free(pd_phys);
}

static uint32_t vmm_clone_pae_structure(uint32_t src_pdpt_phys, uint32_t **out_user_pages, uint32_t *out_user_pages_count) {
    uint32_t user_page_capacity;
    uint32_t user_page_count;
    uint32_t *user_pages;
    uint32_t eflags;
    uint32_t orig_cr3;
    uint32_t new_pdpt_phys;
    uint32_t new_pd_phys[4];
    uint32_t temp_src_pdpt;
    uint32_t temp_src_pd;
    uint32_t temp_src_pt;
    uint32_t temp_src_page;
    uint32_t temp_new_page;
    uint64_t *src_pdpt;
    uint64_t *new_pdpt;
    uint64_t *src_pd;
    uint64_t *new_pd;
    uint64_t *src_pt;
    uint64_t *new_pt;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    uint32_t slot;
    uint64_t src_pdpte;
    uint32_t src_pd_phys;
    uint64_t src_pde;
    uint32_t src_pt_phys;
    uint32_t pde_flags;
    void *new_pt_page;
    uint32_t new_pt_phys;
    uint32_t temp_new_pt;
    uint64_t src_pte;
    uint64_t src_page_phys;
    uint32_t pte_flags;
    uint64_t new_page_phys;
    uint64_t entry;
    uint64_t *cleanup_pd;
    uint32_t cleanup_pt_phys;

    user_page_capacity = 512;
    user_page_count = 0;

    user_pages = (uint32_t *)kmalloc(user_page_capacity * sizeof(uint32_t));
    if (!user_pages) {
        printf("vmm_clone_pae: Failed to allocate user_pages array\n");
        return 0;
    }

    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    uint32_t kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    for (slot = 0; slot < pae_pdpt_pool_size(); slot++) {
        if (!pae_pdpt_pool_used_get(slot)) break;
    }
    if (slot >= pae_pdpt_pool_size()) {
        kfree(user_pages);
        if (kernel_cr3 && orig_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        }
        if (eflags & (1 << 9)) __asm__ volatile ("sti");
        return 0;
    }
    pae_pdpt_pool_used_set(slot, 1);

    new_pdpt_phys = (uint32_t)((uintptr_t)pae_pdpt_pool_get(slot) - 0xC0000000);
    if (debugMode && debugLevel >= 3) printf("vmm_clone_pae: allocated slot %u pdpt_phys=0x%08X\n", slot, new_pdpt_phys);
    for (i = 0; i < 4; i++) {
        new_pd_phys[i] = (uint32_t)((uintptr_t)pae_pd_pool_get(slot, i) - 0xC0000000);
    }

    new_pdpt = pae_pdpt_pool_get(slot);
    for (i = 0; i < 4; i++) {
        new_pdpt[i] = ((uint64_t)new_pd_phys[i] & ~0xFFFULL) | 1;
    }

    temp_src_pdpt = 0xF7000000;
    temp_src_pd = 0xF7002000;
    temp_src_pt = 0xF7004000;
    temp_src_page = 0xF7006000;
    temp_new_page = 0xF7007000;

    temp_map_raw(temp_src_pdpt, src_pdpt_phys);
    src_pdpt = (uint64_t *)temp_src_pdpt;

    for (i = 0; i < 3; i++) {
        src_pdpte = src_pdpt[i];
        new_pd = pae_pd_pool_get(slot, i);
        memset(new_pd, 0, PAGE_SIZE);

        if (!(src_pdpte & 1)) continue;

        src_pd_phys = (uint32_t)(src_pdpte & ~0xFFFULL);
        temp_map_raw(temp_src_pd, src_pd_phys);
        src_pd = (uint64_t *)temp_src_pd;

        for (j = 0; j < 512; j++) {
            src_pde = src_pd[j];
            if (!(src_pde & 1)) continue;

            src_pt_phys = (uint32_t)(src_pde & ~0xFFFULL);
            pde_flags = (uint32_t)(src_pde & 0xFFF);

            new_pt_page = pmm_alloc_page();
            if (!new_pt_page) {
                temp_unmap_raw(temp_src_pd);
                temp_unmap_raw(temp_src_pdpt);
                goto cleanup_fail_pae;
            }
            new_pt_phys = (uint32_t)new_pt_page;
            pmm_zero_page_phys(new_pt_phys);

            new_pd[j] = ((uint64_t)new_pt_phys & ~0xFFFULL) | pde_flags;

            temp_new_pt = 0xF7005000;
            temp_map_raw(temp_src_pt, src_pt_phys);
            src_pt = (uint64_t *)temp_src_pt;
            temp_map_raw(temp_new_pt, new_pt_phys);
            new_pt = (uint64_t *)temp_new_pt;

            for (k = 0; k < 512; k++) {
                src_pte = src_pt[k];
                if (!(src_pte & 1)) continue;

                src_page_phys = src_pte & ~0xFFFULL;
                pte_flags = (uint32_t)(src_pte & 0xFFF);

                new_page_phys = pfa_alloc64();
                if (!new_page_phys) {
                    temp_unmap_raw(temp_new_pt);
                    temp_unmap_raw(temp_src_pt);
                    temp_unmap_raw(temp_src_pd);
                    temp_unmap_raw(temp_src_pdpt);
                    goto cleanup_fail_pae;
                }

                temp_map_raw_pae(temp_src_page, src_page_phys);
                temp_map_raw_pae(temp_new_page, new_page_phys);
                memcpy((void *)temp_new_page, (void *)temp_src_page, PAGE_SIZE);
                temp_unmap_raw(temp_src_page);
                temp_unmap_raw(temp_new_page);

                new_pt[k] = (new_page_phys & ~0xFFFULL) | pte_flags;

                if (user_page_count >= user_page_capacity) {
                    temp_unmap_raw(temp_new_pt);
                    temp_unmap_raw(temp_src_pt);
                    temp_unmap_raw(temp_src_pd);
                    temp_unmap_raw(temp_src_pdpt);
                    goto cleanup_fail_pae;
                }
                user_pages[user_page_count++] = (uint32_t)new_page_phys;
            }
            temp_unmap_raw(temp_new_pt);
            temp_unmap_raw(temp_src_pt);
        }
        temp_unmap_raw(temp_src_pd);
    }

    new_pd = pae_pd_pool_get(slot, 3);
    for (j = 0; j < 512; j++) {
        entry = boot_pd_high[j];
        if (entry & 1) {
            new_pd[j] = entry & ~0x4ULL;
        }
    }

    temp_unmap_raw(temp_src_pdpt);

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return new_pdpt_phys;

cleanup_fail_pae:
    for (i = 0; i < user_page_count; i++) {
        pfa_free(user_pages[i]);
    }
    kfree(user_pages);

    for (i = 0; i < 3; i++) {
        cleanup_pd = pae_pd_pool_get(slot, i);
        for (j = 0; j < 512; j++) {
            if (cleanup_pd[j] & 1) {
                cleanup_pt_phys = (uint32_t)(cleanup_pd[j] & ~0xFFFULL);
                pfa_free(cleanup_pt_phys);
            }
        }
    }
    pae_pdpt_pool_used_set(slot, 0);

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return 0;
}

uint32_t vmm_clone_page_directory(uint32_t src_pd_phys, uint32_t **out_user_pages, uint32_t *out_user_pages_count) {
    if (!src_pd_phys) return 0;

    if (pae_enabled) {
        return vmm_clone_pae_structure(src_pd_phys, out_user_pages, out_user_pages_count);
    }

    uint32_t clone_log_count = 0;
    uint32_t clone_sample_count = 0;

    uint32_t user_page_capacity = 512;
    uint32_t user_page_count = 0;
    uint32_t *user_pages = (uint32_t *)kmalloc(user_page_capacity * sizeof(uint32_t));
    if (!user_pages) {
        printf("vmm_clone_page_directory: Failed to allocate user_pages array\n");
        return 0;
    }

    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    
    uint32_t orig_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    uint32_t kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    void *new_pd_page = pmm_alloc_low_page();
    if (!new_pd_page) {
        new_pd_page = pmm_alloc_page();
    }
    if (!new_pd_page) {
        printf("vmm_clone_page_directory: Failed to allocate new PD\n");
        if (kernel_cr3 && orig_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        }
        if (eflags & (1 << 9)) __asm__ volatile ("sti");
        kfree(user_pages);
        return 0;
    }
    uint32_t new_pd_phys = (uint32_t)new_pd_page;

    uint32_t temp_src_pd = 0xF7000000;
    uint32_t temp_new_pd = 0xF7001000;
    uint32_t temp_src_pt = 0xF7002000;
    uint32_t temp_new_pt = 0xF7003000;
    uint32_t temp_src_page = 0xF7004000;
    uint32_t temp_new_page = 0xF7005000;

    temp_map_raw(temp_new_pd, new_pd_phys);
    uint32_t *new_pd = (uint32_t *)temp_new_pd;
    memset(new_pd, 0, PAGE_SIZE);
    temp_unmap_raw(temp_new_pd);

    temp_map_raw(temp_src_pd, src_pd_phys);
    uint32_t *src_pd = (uint32_t *)temp_src_pd;

    for (uint32_t pd_idx = 0; pd_idx < 768; pd_idx++) {
        uint32_t src_pde = src_pd[pd_idx];
        if (!(src_pde & 1)) continue;

        uint32_t src_pt_phys = src_pde & ~0xFFF;
        uint32_t pde_flags = src_pde & 0xFFF;

        void *new_pt_page = pmm_alloc_low_page();
        if (!new_pt_page) {
            new_pt_page = pmm_alloc_page();
        }
        if (!new_pt_page) {
            printf("vmm_clone_page_directory: Failed to allocate PT for pd_idx %u\n", pd_idx);
            temp_unmap_raw(temp_src_pd);
            goto cleanup_fail;
        }
        uint32_t new_pt_phys = (uint32_t)new_pt_page;

        temp_map_raw(temp_new_pd, new_pd_phys);
        ((uint32_t *)temp_new_pd)[pd_idx] = (new_pt_phys & ~0xFFF) | pde_flags;
        temp_unmap_raw(temp_new_pd);

        temp_map_raw(temp_src_pt, src_pt_phys);
        temp_map_raw(temp_new_pt, new_pt_phys);
        uint32_t *src_pt = (uint32_t *)temp_src_pt;
        uint32_t *new_pt = (uint32_t *)temp_new_pt;

        memset(new_pt, 0, PAGE_SIZE);

        for (uint32_t pt_idx = 0; pt_idx < 1024; pt_idx++) {
            uint32_t src_pte = src_pt[pt_idx];
            if (!(src_pte & 1)) continue;

            uint32_t src_page_phys = src_pte & ~0xFFF;
            uint32_t pte_flags = src_pte & 0xFFF;

            uint32_t new_page_phys = pfa_alloc();
            if (!new_page_phys) {
                printf("vmm_clone_page_directory: Failed to allocate page copy\n");
                temp_unmap_raw(temp_src_pt);
                temp_unmap_raw(temp_new_pt);
                temp_unmap_raw(temp_src_pd);
                goto cleanup_fail;
            }

            if (debugMode && debugLevel >= 5 && clone_should_log_detail(clone_log_count)) {
                DPRINTF3("vmm_clone: copying page pd_idx=%u pt_idx=%u virt=0x%08X src_phys=0x%08X -> new_phys=0x%08X flags=0x%03X\n",
                         pd_idx, pt_idx, (pd_idx<<22) | (pt_idx<<12), src_page_phys, new_page_phys, pte_flags);
            }
            clone_log_count++;

            temp_map_raw(temp_src_page, src_page_phys);
            temp_map_raw(temp_new_page, new_page_phys);

            memcpy((void *)temp_new_page, (void *)temp_src_page, PAGE_SIZE);

            if (debugMode && debugLevel >= 5 && clone_should_log_sample(clone_sample_count)) {
                uint32_t sample_before = ((uint32_t *)temp_src_page)[0];
                uint32_t sample_after = ((uint32_t *)temp_new_page)[0];
                DPRINTF3("vmm_clone: page copy sample: src_phys=0x%08X before=0x%08X new_phys=0x%08X after=0x%08X\n",
                         src_page_phys, sample_before, new_page_phys, sample_after);
            }
            clone_sample_count++;

            temp_unmap_raw(temp_src_page);
            temp_unmap_raw(temp_new_page);

            volatile uint32_t *vpt = (volatile uint32_t *)new_pt;
            vpt[pt_idx] = (new_page_phys & ~0xFFF) | pte_flags;
            
            __asm__ volatile ("" ::: "memory");
            if (debugMode && debugLevel >= 5 && clone_should_log_detail(clone_log_count)) {
                uint32_t verify_pte = vpt[pt_idx];
                DPRINTF3("vmm_clone: set new_pt[%u]=0x%08X verify=0x%08X\n", pt_idx, (new_page_phys & ~0xFFF) | pte_flags, verify_pte);
            }
            clone_log_count++;

            if (user_page_count >= user_page_capacity) {
                printf("vmm_clone: user_pages array full (capacity=%u)!\n", user_page_capacity);
                temp_unmap_raw(temp_src_pt);
                temp_unmap_raw(temp_new_pt);
                temp_unmap_raw(temp_src_pd);
                goto cleanup_fail;
            }
            user_pages[user_page_count++] = new_page_phys;
        }

        temp_unmap_raw(temp_src_pt);
        temp_unmap_raw(temp_new_pt);
    }

    for (uint32_t i = 768; i < 1023; i++) {
        temp_map_raw(temp_new_pd, new_pd_phys);
        uint32_t pde = src_pd[i] & ~0x4;
        ((uint32_t *)temp_new_pd)[i] = pde;
        temp_unmap_raw(temp_new_pd);
    }

    temp_unmap_raw(temp_src_pd);

    temp_map_raw(temp_new_pd, new_pd_phys);
    ((uint32_t *)temp_new_pd)[1023] = (new_pd_phys & ~0xFFF) | 3;
    temp_unmap_raw(temp_new_pd);

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    DPRINTF3("vmm_clone_page_directory: cloned pd 0x%08X -> 0x%08X (%u user pages)\n",
           src_pd_phys, new_pd_phys, user_page_count);

    if (debugMode && debugLevel >= 4) {
    for (uint32_t v = 0x00400000; v < 0x00403000; v += PAGE_SIZE) {
        uint32_t pd_idx = v >> 22;
        uint32_t pt_idx = (v >> 12) & 0x3FF;

        temp_map_raw(0xF7000000, src_pd_phys);
        uint32_t src_pde = ((uint32_t *)0xF7000000)[pd_idx];
        if (src_pde & 1) {
            uint32_t src_pt_phys = src_pde & ~0xFFF;
            temp_map_raw(0xF7002000, src_pt_phys);
            uint32_t src_pte = ((uint32_t *)0xF7002000)[pt_idx];
            temp_unmap_raw(0xF7002000);
            DPRINTF2("src: virt=0x%08X pde=0x%08X pte=0x%08X\n", v, src_pde, src_pte);
        } else {
            DPRINTF2("src: virt=0x%08X pde=0x%08X (no pt)\n", v, src_pde);
        }
        temp_unmap_raw(0xF7000000);

        temp_map_raw(0xF7001000, new_pd_phys);
        uint32_t new_pde = ((uint32_t *)0xF7001000)[pd_idx];
        if (new_pde & 1) {
            uint32_t new_pt_phys = new_pde & ~0xFFF;
            temp_map_raw(0xF7003000, new_pt_phys);
            uint32_t new_pte = ((uint32_t *)0xF7003000)[pt_idx];
            temp_unmap_raw(0xF7003000);
            DPRINTF2("new: virt=0x%08X pde=0x%08X pte=0x%08X\n", v, new_pde, new_pte);
        } else {
            DPRINTF2("new: virt=0x%08X pde=0x%08X (no pt)\n", v, new_pde);
        }
        temp_unmap_raw(0xF7001000);
    }
    }

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return new_pd_phys;

cleanup_fail:
    for (uint32_t i = 0; i < user_page_count; i++) {
        pfa_free(user_pages[i]);
    }
    kfree(user_pages);

    temp_map_raw(temp_new_pd, new_pd_phys);
    uint32_t *cleanup_pd = (uint32_t *)temp_new_pd;
    for (uint32_t i = 0; i < 768; i++) {
        if (cleanup_pd[i] & 1) {
            pfa_free(cleanup_pd[i] & ~0xFFF);
        }
    }
    temp_unmap_raw(temp_new_pd);
    pfa_free(new_pd_phys);

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }
    if (eflags & (1 << 9)) __asm__ volatile ("sti");

    return 0;
}

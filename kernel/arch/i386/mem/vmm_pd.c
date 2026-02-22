#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <kernel/smp.h>
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
extern void pfa_ref_inc(uint32_t phys_addr);
extern int pfa_ref_dec(uint32_t phys_addr);
extern uint8_t pfa_ref_get(uint32_t phys_addr);
extern void pfa_cow_release(uint32_t phys_addr);
extern void pfa_cow_release64(uint64_t phys_addr);
extern uint32_t vmm_create_pae_structure(void);
extern uint32_t pae_pdpt_pool_size(void);
extern uint8_t pae_pdpt_pool_used_get(uint32_t slot);
extern void pae_pdpt_pool_used_set(uint32_t slot, uint8_t value);
extern uint64_t *pae_pdpt_pool_get(uint32_t slot);
extern uint64_t *pae_pd_pool_get(uint32_t slot, uint32_t pdpt_idx);
extern int pae_pd_pool_alloc_slot(uint32_t slot);
extern void pae_pd_pool_free_slot(uint32_t slot);

static int cow_handle_fault_pae(uint32_t fault_addr, uint32_t pdpt_phys);

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
    uint32_t *pt32;
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
                    for (i = 0; i < 3; i++) {
                        pd64 = pae_pd_pool_get(slot, i);
                        if (!pd64) continue;
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
                                    pfa_cow_release64(pte & ~0xFFFULL);
                                }
                            }
                            temp_unmap_raw(temp_pt_virt);
                            pfa_free(pt_phys_val);
                        }
                        memset(pd64, 0, PAGE_SIZE);
                    }
                    pae_pd_pool_free_slot(slot);
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
        pdpt = (uint64_t *)(temp_pdpt_virt + (pd_phys & 0xFFF));

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
                        pfa_cow_release64(pte & ~0xFFFULL);
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
            temp_pt_virt = 0xF7002000;
            temp_map_raw(temp_pt_virt, pt_phys_val);
            pt32 = (uint32_t *)temp_pt_virt;
            for (k = 0; k < 1024; k++) {
                if (pt32[k] & 1) {
                    pfa_cow_release(pt32[k] & ~0xFFF);
                }
            }
            temp_unmap_raw(temp_pt_virt);
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
    uint32_t orig_cr3;
    uint32_t kernel_cr3;
    uint32_t new_pdpt_phys;
    uint32_t new_pd_phys[4];
    uint32_t temp_src_pdpt;
    uint32_t temp_src_pd;
    uint32_t temp_src_pt;
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
    uint64_t cow_flags64;
    uint64_t entry;
    uint64_t *cleanup_pd;
    uint32_t cleanup_pt_phys;

    user_page_capacity = 512;
    user_page_count = 0;

    user_pages = (uint32_t *)kmalloc(user_page_capacity * sizeof(uint32_t));
    if (!user_pages) {
        return 0;
    }

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    kernel_cr3 = vmm_get_kernel_cr3();
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
        return 0;
    }
    pae_pdpt_pool_used_set(slot, 1);

    if (pae_pd_pool_alloc_slot(slot) < 0) {
        pae_pdpt_pool_used_set(slot, 0);
        kfree(user_pages);
        if (kernel_cr3 && orig_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        }
        return 0;
    }

    new_pdpt_phys = (uint32_t)((uintptr_t)pae_pdpt_pool_get(slot) - 0xC0000000);
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

    temp_map_raw(temp_src_pdpt, src_pdpt_phys);
    src_pdpt = (uint64_t *)(temp_src_pdpt + (src_pdpt_phys & 0xFFF));

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

            new_pd[j] = ((uint64_t)new_pt_phys & ~0xFFFULL) | pde_flags;

            temp_new_pt = 0xF7005000;
            temp_map_raw(temp_src_pt, src_pt_phys);
            src_pt = (uint64_t *)temp_src_pt;
            temp_map_raw(temp_new_pt, new_pt_phys);
            new_pt = (uint64_t *)temp_new_pt;
            memset(new_pt, 0, PAGE_SIZE);

            for (k = 0; k < 512; k++) {
                src_pte = src_pt[k];
                if (!(src_pte & 1)) continue;

                src_page_phys = src_pte & ~0xFFFULL;
                pte_flags = (uint32_t)(src_pte & 0xFFF);

                if (pte_flags & 0x2) {
                    cow_flags64 = (uint64_t)((pte_flags & ~0x2) | 0x200);
                    src_pt[k] = (src_page_phys & ~0xFFFULL) | cow_flags64;
                    new_pt[k] = (src_page_phys & ~0xFFFULL) | cow_flags64;
                    if (pfa_ref_get((uint32_t)src_page_phys) == 0)
                        pfa_ref_inc((uint32_t)src_page_phys);
                    pfa_ref_inc((uint32_t)src_page_phys);
                } else {
                    new_pt[k] = (src_page_phys & ~0xFFFULL) | (uint64_t)(pte_flags | 0x200);
                    if (pfa_ref_get((uint32_t)src_page_phys) == 0)
                        pfa_ref_inc((uint32_t)src_page_phys);
                    pfa_ref_inc((uint32_t)src_page_phys);
                }

                if (user_page_count >= user_page_capacity) {
                    uint32_t new_cap;
                    uint32_t *new_arr;
                    new_cap = user_page_capacity * 2;
                    new_arr = (uint32_t *)kmalloc(new_cap * sizeof(uint32_t));
                    if (!new_arr) {
                        temp_unmap_raw(temp_new_pt);
                        temp_unmap_raw(temp_src_pt);
                        temp_unmap_raw(temp_src_pd);
                        temp_unmap_raw(temp_src_pdpt);
                        goto cleanup_fail_pae;
                    }
                    memcpy(new_arr, user_pages, user_page_count * sizeof(uint32_t));
                    kfree(user_pages);
                    user_pages = new_arr;
                    user_page_capacity = new_cap;
                }
                user_pages[user_page_count++] = (uint32_t)src_page_phys;
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

    if (orig_cr3 == src_pdpt_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(src_pdpt_phys) : "memory");
    }

    smp_tlb_flush_all();

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    if (kernel_cr3 && orig_cr3 != kernel_cr3 && orig_cr3 != src_pdpt_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    return new_pdpt_phys;

cleanup_fail_pae:
    for (i = 0; i < user_page_count; i++) {
        pfa_cow_release(user_pages[i]);
    }
    kfree(user_pages);

    for (i = 0; i < 3; i++) {
        cleanup_pd = pae_pd_pool_get(slot, i);
        if (!cleanup_pd) continue;
        for (j = 0; j < 512; j++) {
            if (cleanup_pd[j] & 1) {
                cleanup_pt_phys = (uint32_t)(cleanup_pd[j] & ~0xFFFULL);
                pfa_free(cleanup_pt_phys);
            }
        }
    }
    pae_pd_pool_free_slot(slot);
    pae_pdpt_pool_used_set(slot, 0);

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    return 0;
}

uint32_t vmm_clone_page_directory(uint32_t src_pd_phys, uint32_t **out_user_pages, uint32_t *out_user_pages_count) {
    uint32_t user_page_capacity;
    uint32_t user_page_count;
    uint32_t *user_pages;
    uint32_t orig_cr3;
    uint32_t kernel_cr3;
    void *new_pd_page;
    uint32_t new_pd_phys;
    uint32_t temp_src_pd;
    uint32_t temp_new_pd;
    uint32_t temp_src_pt;
    uint32_t temp_new_pt;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t src_pde;
    uint32_t src_pt_phys;
    uint32_t pde_flags;
    void *new_pt_page;
    uint32_t new_pt_phys;
    uint32_t *src_pt;
    uint32_t *new_pt;
    uint32_t src_pte;
    uint32_t src_page_phys;
    uint32_t pte_flags;
    uint32_t cow_flags;
    uint32_t i;
    uint32_t *src_pd;
    uint32_t *new_pd;
    uint32_t *cleanup_pd;

    if (!src_pd_phys) return 0;

    if (pae_enabled) {
        return vmm_clone_pae_structure(src_pd_phys, out_user_pages, out_user_pages_count);
    }

    user_page_capacity = 512;
    user_page_count = 0;
    user_pages = (uint32_t *)kmalloc(user_page_capacity * sizeof(uint32_t));
    if (!user_pages) {
        return 0;
    }

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    new_pd_page = pmm_alloc_low_page();
    if (!new_pd_page) {
        new_pd_page = pmm_alloc_page();
    }
    if (!new_pd_page) {
        if (kernel_cr3 && orig_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        }
        kfree(user_pages);
        return 0;
    }
    new_pd_phys = (uint32_t)new_pd_page;

    temp_src_pd = 0xF7000000;
    temp_new_pd = 0xF7001000;
    temp_src_pt = 0xF7002000;
    temp_new_pt = 0xF7003000;

    temp_map_raw(temp_new_pd, new_pd_phys);
    new_pd = (uint32_t *)temp_new_pd;
    memset(new_pd, 0, PAGE_SIZE);
    temp_unmap_raw(temp_new_pd);

    temp_map_raw(temp_src_pd, src_pd_phys);
    src_pd = (uint32_t *)temp_src_pd;

    for (pd_idx = 0; pd_idx < 768; pd_idx++) {
        src_pde = src_pd[pd_idx];
        if (!(src_pde & 1)) continue;

        src_pt_phys = src_pde & ~0xFFF;
        pde_flags = src_pde & 0xFFF;

        new_pt_page = pmm_alloc_low_page();
        if (!new_pt_page) {
            new_pt_page = pmm_alloc_page();
        }
        if (!new_pt_page) {
            temp_unmap_raw(temp_src_pd);
            goto cleanup_fail;
        }
        new_pt_phys = (uint32_t)new_pt_page;

        temp_map_raw(temp_new_pd, new_pd_phys);
        ((uint32_t *)temp_new_pd)[pd_idx] = (new_pt_phys & ~0xFFF) | pde_flags;
        temp_unmap_raw(temp_new_pd);

        temp_map_raw(temp_src_pt, src_pt_phys);
        temp_map_raw(temp_new_pt, new_pt_phys);
        src_pt = (uint32_t *)temp_src_pt;
        new_pt = (uint32_t *)temp_new_pt;

        memset(new_pt, 0, PAGE_SIZE);

        for (pt_idx = 0; pt_idx < 1024; pt_idx++) {
            src_pte = src_pt[pt_idx];
            if (!(src_pte & 1)) continue;

            src_page_phys = src_pte & ~0xFFF;
            pte_flags = src_pte & 0xFFF;

            if (pte_flags & 0x2) {
                cow_flags = (pte_flags & ~0x2) | 0x200;
                src_pt[pt_idx] = (src_page_phys & ~0xFFF) | cow_flags;
                new_pt[pt_idx] = (src_page_phys & ~0xFFF) | cow_flags;
                if (pfa_ref_get(src_page_phys) == 0)
                    pfa_ref_inc(src_page_phys);
                pfa_ref_inc(src_page_phys);
            } else {
                new_pt[pt_idx] = (src_page_phys & ~0xFFF) | pte_flags | 0x200;
                if (pfa_ref_get(src_page_phys) == 0)
                    pfa_ref_inc(src_page_phys);
                pfa_ref_inc(src_page_phys);
            }

            if (user_page_count >= user_page_capacity) {
                uint32_t new_cap;
                uint32_t *new_arr;
                new_cap = user_page_capacity * 2;
                new_arr = (uint32_t *)kmalloc(new_cap * sizeof(uint32_t));
                if (!new_arr) {
                    temp_unmap_raw(temp_src_pt);
                    temp_unmap_raw(temp_new_pt);
                    temp_unmap_raw(temp_src_pd);
                    goto cleanup_fail;
                }
                memcpy(new_arr, user_pages, user_page_count * sizeof(uint32_t));
                kfree(user_pages);
                user_pages = new_arr;
                user_page_capacity = new_cap;
            }
            user_pages[user_page_count++] = src_page_phys;
        }

        temp_unmap_raw(temp_src_pt);
        temp_unmap_raw(temp_new_pt);
    }

    temp_map_raw(temp_new_pd, new_pd_phys);
    new_pd = (uint32_t *)temp_new_pd;
    for (i = 768; i < 1023; i++) {
        new_pd[i] = src_pd[i] & ~0x4;
    }
    new_pd[1023] = (new_pd_phys & ~0xFFF) | 3;
    temp_unmap_raw(temp_new_pd);

    temp_unmap_raw(temp_src_pd);

    if (orig_cr3 == src_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(src_pd_phys) : "memory");
    }

    smp_tlb_flush_all();

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    if (kernel_cr3 && orig_cr3 != kernel_cr3 && orig_cr3 != src_pd_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    return new_pd_phys;

cleanup_fail:
    for (i = 0; i < user_page_count; i++) {
        pfa_cow_release(user_pages[i]);
    }
    kfree(user_pages);

    temp_map_raw(temp_new_pd, new_pd_phys);
    cleanup_pd = (uint32_t *)temp_new_pd;
    for (i = 0; i < 768; i++) {
        if (cleanup_pd[i] & 1) {
            pfa_free(cleanup_pd[i] & ~0xFFF);
        }
    }
    temp_unmap_raw(temp_new_pd);
    pfa_free(new_pd_phys);

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    return 0;
}

int cow_handle_fault(uint32_t fault_addr, uint32_t pd_phys) {
    uint32_t temp_pd;
    uint32_t temp_pt;
    uint32_t temp_old_page;
    uint32_t temp_new_page;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t pde;
    uint32_t pt_phys;
    uint32_t *pt;
    uint32_t pte;
    uint32_t old_page_phys;
    uint32_t pte_flags;
    uint32_t new_page_phys;
    uint8_t ref;

    if (pae_enabled) {
        return cow_handle_fault_pae(fault_addr, pd_phys);
    }

    pd_idx = fault_addr >> 22;
    pt_idx = (fault_addr >> 12) & 0x3FF;

    temp_pd = 0xF7000000;
    temp_pt = 0xF7001000;
    temp_old_page = 0xF7002000;
    temp_new_page = 0xF7003000;

    temp_map_raw(temp_pd, pd_phys);
    pde = ((uint32_t *)temp_pd)[pd_idx];
    temp_unmap_raw(temp_pd);

    if (!(pde & 1)) return 0;

    pt_phys = pde & ~0xFFF;
    temp_map_raw(temp_pt, pt_phys);
    pt = (uint32_t *)temp_pt;
    pte = pt[pt_idx];

    if (!(pte & 1) || !(pte & 0x200)) {
        temp_unmap_raw(temp_pt);
        return 0;
    }

    old_page_phys = pte & ~0xFFF;
    pte_flags = pte & 0xFFF;

    ref = pfa_ref_get(old_page_phys);
    if (ref <= 1) {
        pt[pt_idx] = (old_page_phys & ~0xFFF) | ((pte_flags | 0x2) & ~0x200);
        temp_unmap_raw(temp_pt);
        if (ref > 0) pfa_ref_dec(old_page_phys);
        __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
        return 1;
    }

    new_page_phys = pfa_alloc();
    if (!new_page_phys) {
        temp_unmap_raw(temp_pt);
        return -1;
    }

    temp_map_raw(temp_old_page, old_page_phys);
    temp_map_raw(temp_new_page, new_page_phys);
    memcpy((void *)temp_new_page, (void *)temp_old_page, PAGE_SIZE);
    temp_unmap_raw(temp_old_page);
    temp_unmap_raw(temp_new_page);

    pt[pt_idx] = (new_page_phys & ~0xFFF) | ((pte_flags | 0x2) & ~0x200);
    temp_unmap_raw(temp_pt);

    pfa_ref_dec(old_page_phys);

    __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");

    return 1;
}

static int cow_handle_fault_pae(uint32_t fault_addr, uint32_t pdpt_phys) {
    uint32_t temp_pdpt;
    uint32_t temp_pd;
    uint32_t temp_pt;
    uint32_t temp_old_page;
    uint32_t temp_new_page;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t *pdpt;
    uint64_t pdpte;
    uint32_t pd_phys;
    uint64_t *pd;
    uint64_t pde;
    uint32_t pt_phys;
    uint64_t *pt;
    uint64_t pte;
    uint32_t old_page_phys;
    uint32_t pte_flags;
    uint32_t new_page_phys;
    uint8_t ref;

    pdpt_idx = fault_addr >> 30;
    pd_idx = (fault_addr >> 21) & 0x1FF;
    pt_idx = (fault_addr >> 12) & 0x1FF;

    temp_pdpt = 0xF7000000;
    temp_pd = 0xF7001000;
    temp_pt = 0xF7002000;
    temp_old_page = 0xF7003000;
    temp_new_page = 0xF7004000;

    temp_map_raw(temp_pdpt, pdpt_phys);
    pdpt = (uint64_t *)(temp_pdpt + (pdpt_phys & 0xFFF));
    pdpte = pdpt[pdpt_idx];
    temp_unmap_raw(temp_pdpt);

    if (!(pdpte & 1)) return 0;

    pd_phys = (uint32_t)(pdpte & ~0xFFFULL);
    temp_map_raw(temp_pd, pd_phys);
    pd = (uint64_t *)temp_pd;
    pde = pd[pd_idx];
    temp_unmap_raw(temp_pd);

    if (!(pde & 1)) return 0;

    pt_phys = (uint32_t)(pde & ~0xFFFULL);
    temp_map_raw(temp_pt, pt_phys);
    pt = (uint64_t *)temp_pt;
    pte = pt[pt_idx];

    if (!(pte & 1) || !(pte & 0x200)) {
        temp_unmap_raw(temp_pt);
        return 0;
    }

    old_page_phys = (uint32_t)(pte & ~0xFFFULL);
    pte_flags = (uint32_t)(pte & 0xFFF);

    ref = pfa_ref_get(old_page_phys);
    if (ref <= 1) {
        pt[pt_idx] = ((uint64_t)old_page_phys & ~0xFFFULL) | (uint64_t)((pte_flags | 0x2) & ~0x200);
        temp_unmap_raw(temp_pt);
        if (ref > 0) pfa_ref_dec(old_page_phys);
        __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
        return 1;
    }

    new_page_phys = pfa_alloc();
    if (!new_page_phys) {
        temp_unmap_raw(temp_pt);
        return -1;
    }

    temp_map_raw(temp_old_page, old_page_phys);
    temp_map_raw(temp_new_page, new_page_phys);
    memcpy((void *)temp_new_page, (void *)temp_old_page, PAGE_SIZE);
    temp_unmap_raw(temp_old_page);
    temp_unmap_raw(temp_new_page);

    pt[pt_idx] = ((uint64_t)new_page_phys & ~0xFFFULL) | (uint64_t)((pte_flags | 0x2) & ~0x200);
    temp_unmap_raw(temp_pt);

    pfa_ref_dec(old_page_phys);

    __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");

    return 1;
}

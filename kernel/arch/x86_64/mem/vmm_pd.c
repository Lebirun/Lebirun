#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/debug.h>
#include <lebirun/smp.h>
#include <string.h>

extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));

extern void *pmm_alloc_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);
extern uint64_t pfa_alloc(void);
extern void pfa_free(uint64_t phys_addr);
extern void pfa_ref_inc(uint64_t phys_addr);
extern int pfa_ref_dec(uint64_t phys_addr);
extern uint8_t pfa_ref_get(uint64_t phys_addr);
extern void pfa_cow_release64(uint64_t phys_addr);

static inline bool clone_should_log_detail(uint64_t index) {
    return index < 2 || (index & 0x3FF) == 0;
}

static inline bool clone_should_log_sample(uint64_t index) {
    return index == 0;
}

uint64_t vmm_create_pml4(void) {
    void *page;
    uint64_t pml4_phys;
    uint64_t temp_virt;
    uint64_t *pml4;
    uint64_t pdpt_high_phys;
    uint64_t saved_flags;

    page = pmm_alloc_page();
    if (!page) {
        printf("vmm_create_pml4: Failed to allocate PML4 page\n");
        return 0;
    }
    pml4_phys = (uint64_t)page;
    pmm_zero_page_phys(pml4_phys);

    pdpt_high_phys = (uint64_t)(uintptr_t)boot_pdpt_high;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    pml4 = (uint64_t *)temp_virt;
    pml4[511] = (pdpt_high_phys & ~0xFFFULL) | 3;
    __asm__ volatile ("" ::: "memory");
    {
        uint64_t verify;
        verify = pml4[511];
        if ((verify & ~0xFFFULL) != (pdpt_high_phys & ~0xFFFULL)) {
            printf("vmm_create_pml4: VERIFY FAIL pml4[511]=0x%016lX expected pdpt=0x%016lX\n",
                   verify, pdpt_high_phys);
        }
    }
    temp_unmap_raw(temp_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    return pml4_phys;
}

void vmm_free_pml4(uint64_t pml4_phys) {
    uint64_t temp_pml4;
    uint64_t temp_pdpt;
    uint64_t temp_pd;
    uint64_t temp_pt;
    uint64_t *pml4;
    uint64_t *pdpt_tbl;
    uint64_t *pd_tbl;
    uint64_t *pt_tbl;
    uint64_t i;
    uint64_t j;
    uint64_t k;
    uint64_t pml4e;
    uint64_t pdpt_phys;
    uint64_t pdpte;
    uint64_t pd_phys_val;
    uint64_t pde;
    uint64_t pt_phys_val;
    uint64_t pte;
    uint64_t saved_flags;

    if (!pml4_phys) return;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_pml4 = TEMP_SLOT(0);
    temp_pdpt = TEMP_SLOT(1);
    temp_pd   = TEMP_SLOT(2);
    temp_pt   = TEMP_SLOT(3);

    temp_map_raw(temp_pml4, pml4_phys);
    pml4 = (uint64_t *)temp_pml4;

    for (i = 0; i < 511; i++) {
        pml4e = pml4[i];
        if (!(pml4e & 1)) continue;
        pdpt_phys = pml4e & ~0xFFFULL;

        temp_map_raw(temp_pdpt, pdpt_phys);
        pdpt_tbl = (uint64_t *)temp_pdpt;

        for (j = 0; j < 512; j++) {
            pdpte = pdpt_tbl[j];
            if (!(pdpte & 1)) continue;
            pd_phys_val = pdpte & ~0xFFFULL;

            temp_map_raw(temp_pd, pd_phys_val);
            pd_tbl = (uint64_t *)temp_pd;

            for (k = 0; k < 512; k++) {
                uint64_t l;
                pde = pd_tbl[k];
                if (!(pde & 1)) continue;
                pt_phys_val = pde & ~0xFFFULL;

                temp_map_raw(temp_pt, pt_phys_val);
                pt_tbl = (uint64_t *)temp_pt;

                for (l = 0; l < 512; l++) {
                    pte = pt_tbl[l];
                    if (pte & 1) {
                        pfa_cow_release64(pte & ~0xFFFULL);
                    }
                }
                temp_unmap_raw(temp_pt);
                pfa_free(pt_phys_val);
            }
            temp_unmap_raw(temp_pd);
            pfa_free(pd_phys_val);
        }
        temp_unmap_raw(temp_pdpt);
        pfa_free(pdpt_phys);
    }

    temp_unmap_raw(temp_pml4);
    pfa_free(pml4_phys);

    if (saved_flags & (1 << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static uint64_t vmm_clone_pml4_impl(uint64_t src_pml4_phys, uint64_t **out_user_pages, uint64_t *out_user_pages_count) {
    uint64_t user_page_capacity;
    uint64_t user_page_count;
    uint64_t *user_pages;
    uint64_t orig_cr3;
    uint64_t kernel_cr3;
    uint64_t new_pml4_phys;
    uint64_t temp_src_pml4;
    uint64_t temp_new_pml4;
    uint64_t temp_src_pdpt = TEMP_SLOT(2);
    uint64_t temp_new_pdpt;
    uint64_t temp_src_pd = TEMP_SLOT(4);
    uint64_t temp_new_pd;
    uint64_t temp_src_pt = TEMP_SLOT(6);
    uint64_t temp_new_pt;    uint64_t *src_pml4;
    uint64_t *new_pml4;
    uint64_t *src_pdpt_tbl;
    uint64_t *new_pdpt_tbl;
    uint64_t *src_pd_tbl;
    uint64_t *new_pd_tbl;
    uint64_t *src_pt_tbl;
    uint64_t *new_pt_tbl;
    uint64_t i;
    uint64_t j;
    uint64_t k;
    uint64_t l;
    uint64_t pml4e;
    uint64_t src_pdpt_phys;
    uint64_t new_pdpt_phys;
    uint64_t pdpte;
    uint64_t src_pd_phys;
    uint64_t new_pd_phys;
    uint64_t pde;
    uint64_t src_pt_phys;
    uint64_t pde_flags;
    void *alloc_page;
    uint64_t new_pt_phys;
    uint64_t src_pte;
    uint64_t src_page_phys;
    uint64_t pte_flags;
    uint64_t cow_flags64;
    uint64_t pdpt_high_phys;
    uint64_t saved_flags;

    user_page_capacity = 512;
    user_page_count = 0;

    user_pages = (uint64_t *)kmalloc(user_page_capacity * sizeof(uint64_t));
    if (!user_pages) return 0;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    alloc_page = pmm_alloc_page();
    if (!alloc_page) {
        kfree(user_pages);
        if (kernel_cr3 && orig_cr3 != kernel_cr3)
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        return 0;
    }
    new_pml4_phys = (uint64_t)alloc_page;
    pmm_zero_page_phys(new_pml4_phys);

    pdpt_high_phys = (uint64_t)(uintptr_t)boot_pdpt_high;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_src_pml4 = TEMP_SLOT(0);
    temp_new_pml4 = TEMP_SLOT(1);

    temp_map_raw(temp_src_pml4, src_pml4_phys);
    src_pml4 = (uint64_t *)temp_src_pml4;
    temp_map_raw(temp_new_pml4, new_pml4_phys);
    new_pml4 = (uint64_t *)temp_new_pml4;

    new_pml4[511] = (pdpt_high_phys & ~0xFFFULL) | 3;
    __asm__ volatile ("" ::: "memory");
    {
        uint64_t verify;
        verify = new_pml4[511];
        if ((verify & ~0xFFFULL) != (pdpt_high_phys & ~0xFFFULL)) {
            printf("vmm_clone_pml4: VERIFY FAIL pml4[511]=0x%016lX expected=0x%016lX\n",
                   verify, pdpt_high_phys);
        }
    }

    for (i = 0; i < 511; i++) {
        pml4e = src_pml4[i];
        if (!(pml4e & 1)) continue;
        src_pdpt_phys = pml4e & ~0xFFFULL;

        alloc_page = pmm_alloc_page();
        if (!alloc_page) goto cleanup_fail;
        new_pdpt_phys = (uint64_t)alloc_page;
        pmm_zero_page_phys(new_pdpt_phys);

        new_pml4[i] = (new_pdpt_phys & ~0xFFFULL) | (pml4e & 0xFFF);

        temp_src_pdpt = TEMP_SLOT(2);
        temp_new_pdpt = TEMP_SLOT(3);
        temp_map_raw(temp_src_pdpt, src_pdpt_phys);
        src_pdpt_tbl = (uint64_t *)temp_src_pdpt;
        temp_map_raw(temp_new_pdpt, new_pdpt_phys);
        new_pdpt_tbl = (uint64_t *)temp_new_pdpt;

        for (j = 0; j < 512; j++) {
            pdpte = src_pdpt_tbl[j];
            if (!(pdpte & 1)) continue;
            src_pd_phys = pdpte & ~0xFFFULL;

            alloc_page = pmm_alloc_page();
            if (!alloc_page) {
                temp_unmap_raw(temp_new_pdpt);
                temp_unmap_raw(temp_src_pdpt);
                goto cleanup_fail;
            }
            new_pd_phys = (uint64_t)alloc_page;
            pmm_zero_page_phys(new_pd_phys);

            new_pdpt_tbl[j] = (new_pd_phys & ~0xFFFULL) | (pdpte & 0xFFF);

            temp_src_pd = TEMP_SLOT(4);
            temp_new_pd = TEMP_SLOT(5);
            temp_map_raw(temp_src_pd, src_pd_phys);
            src_pd_tbl = (uint64_t *)temp_src_pd;
            temp_map_raw(temp_new_pd, new_pd_phys);
            new_pd_tbl = (uint64_t *)temp_new_pd;

            for (k = 0; k < 512; k++) {
                pde = src_pd_tbl[k];
                if (!(pde & 1)) continue;
                src_pt_phys = pde & ~0xFFFULL;
                pde_flags = pde & 0xFFF;

                alloc_page = pmm_alloc_page();
                if (!alloc_page) {
                    temp_unmap_raw(temp_new_pd);
                    temp_unmap_raw(temp_src_pd);
                    temp_unmap_raw(temp_new_pdpt);
                    temp_unmap_raw(temp_src_pdpt);
                    goto cleanup_fail;
                }
                new_pt_phys = (uint64_t)alloc_page;

                new_pd_tbl[k] = (new_pt_phys & ~0xFFFULL) | pde_flags;

                temp_src_pt = TEMP_SLOT(6);
                temp_new_pt = TEMP_SLOT(7);
                temp_map_raw(temp_src_pt, src_pt_phys);
                src_pt_tbl = (uint64_t *)temp_src_pt;
                temp_map_raw(temp_new_pt, new_pt_phys);
                new_pt_tbl = (uint64_t *)temp_new_pt;
                memset(new_pt_tbl, 0, PAGE_SIZE);

                for (l = 0; l < 512; l++) {
                    src_pte = src_pt_tbl[l];
                    if (!(src_pte & 1)) continue;

                    src_page_phys = src_pte & ~0xFFFULL;
                    pte_flags = src_pte & 0xFFF;

                    if (pte_flags & 0x2) {
                        cow_flags64 = (pte_flags & ~0x2) | 0x200;
                        src_pt_tbl[l] = (src_page_phys & ~0xFFFULL) | cow_flags64;
                        new_pt_tbl[l] = (src_page_phys & ~0xFFFULL) | cow_flags64;
                        if (pfa_ref_get(src_page_phys) == 0)
                            pfa_ref_inc(src_page_phys);
                        pfa_ref_inc(src_page_phys);
                    } else {
                        new_pt_tbl[l] = (src_page_phys & ~0xFFFULL) | (pte_flags | 0x200);
                        if (pfa_ref_get(src_page_phys) == 0)
                            pfa_ref_inc(src_page_phys);
                        pfa_ref_inc(src_page_phys);
                    }

                    if (user_page_count >= user_page_capacity) {
                        uint64_t new_cap;
                        uint64_t *new_arr;
                        new_cap = user_page_capacity * 2;
                        new_arr = (uint64_t *)kmalloc(new_cap * sizeof(uint64_t));
                        if (!new_arr) {
                            temp_unmap_raw(temp_new_pt);
                            temp_unmap_raw(temp_src_pt);
                            temp_unmap_raw(temp_new_pd);
                            temp_unmap_raw(temp_src_pd);
                            temp_unmap_raw(temp_new_pdpt);
                            temp_unmap_raw(temp_src_pdpt);
                            goto cleanup_fail;
                        }
                        memcpy(new_arr, user_pages, user_page_count * sizeof(uint64_t));
                        kfree(user_pages);
                        user_pages = new_arr;
                        user_page_capacity = new_cap;
                    }
                    user_pages[user_page_count++] = src_page_phys;
                }
                temp_unmap_raw(temp_new_pt);
                temp_unmap_raw(temp_src_pt);
            }
            temp_unmap_raw(temp_new_pd);
            temp_unmap_raw(temp_src_pd);
        }
        temp_unmap_raw(temp_new_pdpt);
        temp_unmap_raw(temp_src_pdpt);
    }

    temp_unmap_raw(temp_new_pml4);
    temp_unmap_raw(temp_src_pml4);

    if (orig_cr3 == src_pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(src_pml4_phys) : "memory");
    }

    smp_tlb_flush_all();

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;

    if (kernel_cr3 && orig_cr3 != kernel_cr3 && orig_cr3 != src_pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    return new_pml4_phys;

cleanup_fail:
    temp_unmap_raw(temp_new_pml4);
    temp_unmap_raw(temp_src_pml4);

    {
        uint64_t ci, cj, ck, cl;
        uint64_t c_pml4e, c_pdpte, c_pde, c_pte;
        uint64_t c_pdpt_phys, c_pd_phys, c_pt_phys;

        temp_map_raw(temp_new_pml4, new_pml4_phys);
        new_pml4 = (uint64_t *)temp_new_pml4;

        for (ci = 0; ci < 511; ci++) {
            c_pml4e = new_pml4[ci];
            if (!(c_pml4e & 1)) continue;
            c_pdpt_phys = c_pml4e & ~0xFFFULL;

            temp_map_raw(temp_src_pdpt, c_pdpt_phys);
            new_pdpt_tbl = (uint64_t *)temp_src_pdpt;

            for (cj = 0; cj < 512; cj++) {
                c_pdpte = new_pdpt_tbl[cj];
                if (!(c_pdpte & 1)) continue;
                c_pd_phys = c_pdpte & ~0xFFFULL;

                temp_map_raw(temp_src_pd, c_pd_phys);
                new_pd_tbl = (uint64_t *)temp_src_pd;

                for (ck = 0; ck < 512; ck++) {
                    c_pde = new_pd_tbl[ck];
                    if (!(c_pde & 1)) continue;
                    c_pt_phys = c_pde & ~0xFFFULL;

                    temp_map_raw(temp_src_pt, c_pt_phys);
                    new_pt_tbl = (uint64_t *)temp_src_pt;
                    for (cl = 0; cl < 512; cl++) {
                        c_pte = new_pt_tbl[cl];
                        if (c_pte & 1) {
                            pfa_cow_release64(c_pte & ~0xFFFULL);
                        }
                    }
                    temp_unmap_raw(temp_src_pt);
                    pfa_free(c_pt_phys);
                }
                temp_unmap_raw(temp_src_pd);
                pfa_free(c_pd_phys);
            }
            temp_unmap_raw(temp_src_pdpt);
            pfa_free(c_pdpt_phys);
        }
        temp_unmap_raw(temp_new_pml4);
    }
    pfa_free(new_pml4_phys);

    for (i = 0; i < user_page_count; i++) {
        pfa_cow_release64(user_pages[i]);
    }
    kfree(user_pages);

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    return 0;
}

uint64_t vmm_clone_pml4(uint64_t src_pml4_phys, uint64_t **out_user_pages, uint64_t *out_user_pages_count) {
    if (!src_pml4_phys) return 0;
    return vmm_clone_pml4_impl(src_pml4_phys, out_user_pages, out_user_pages_count);
}

int cow_handle_fault(uint64_t fault_addr, uint64_t pml4_phys) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t entry;
    uint64_t pdpt_phys;
    uint64_t pd_phys_val;
    uint64_t pt_phys;
    uint64_t *pt;
    uint64_t pte;
    uint64_t old_page_phys;
    uint64_t pte_flags;
    uint64_t new_page_phys;
    uint8_t ref;
    uint64_t saved_flags;

    pml4_idx = (fault_addr >> 39) & 0x1FF;
    pdpt_idx = (fault_addr >> 30) & 0x1FF;
    pd_idx = (fault_addr >> 21) & 0x1FF;
    pt_idx = (fault_addr >> 12) & 0x1FF;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pml4_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pdpt_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pdpt_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pd_phys_val = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pd_phys_val);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pt_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, pt_phys);
    pt = (uint64_t *)temp_virt;
    pte = pt[pt_idx];

    if (!(pte & 1) || !(pte & 0x200)) {
        temp_unmap_raw(temp_virt);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return 0;
    }

    old_page_phys = pte & ~0xFFFULL;
    pte_flags = pte & 0xFFF;

    ref = pfa_ref_get(old_page_phys);
    if (ref <= 1) {
        pt[pt_idx] = (old_page_phys & ~0xFFFULL) | ((pte_flags | 0x2) & ~0x200);
        temp_unmap_raw(temp_virt);
        if (ref > 0) pfa_ref_dec(old_page_phys);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
        return 1;
    }

    new_page_phys = pfa_alloc();
    if (!new_page_phys) {
        temp_unmap_raw(temp_virt);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return -1;
    }

    {
        uint64_t temp_old;
        uint64_t temp_new;
        temp_old = TEMP_SLOT(2);
        temp_new = TEMP_SLOT(3);
        temp_map_raw(temp_old, old_page_phys);
        temp_map_raw(temp_new, new_page_phys);
        memcpy((void *)temp_new, (void *)temp_old, PAGE_SIZE);
        temp_unmap_raw(temp_old);
        temp_unmap_raw(temp_new);
    }

    pt[pt_idx] = (new_page_phys & ~0xFFFULL) | ((pte_flags | 0x2) & ~0x200);
    temp_unmap_raw(temp_virt);

    pfa_ref_dec(old_page_phys);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");

    return 1;
}

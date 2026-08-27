#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <lebirun/task.h>
#include <string.h>

extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));

extern void *pmm_alloc_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);
extern uint64_t pfa_alloc(void);
extern void pfa_free(uint64_t phys_addr);
extern void pfa_ref_inc(uint64_t phys_addr);
extern uint64_t pfa_ref_dec(uint64_t phys_addr);
extern uint64_t pfa_ref_get(uint64_t phys_addr);
extern void pfa_cow_release64(uint64_t phys_addr);

#define VMM_PHYS_MASK 0x000FFFFFFFFFF000ULL

static inline bool clone_should_log_detail(uint64_t index) {
    return index < 2 || (index & 0x3FF) == 0;
}

static inline bool clone_should_log_sample(uint64_t index) {
    return index == 0;
}

static uint64_t vmm_table_read(uint64_t table_phys, uint64_t index) {
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t entry;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[index];
    temp_unmap_raw(temp_virt);
    return entry;
}

static void vmm_table_write(uint64_t table_phys, uint64_t index,
                            uint64_t entry) {
    uint64_t temp_virt;
    uint64_t *table;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    table[index] = entry;
    __asm__ volatile ("" ::: "memory");
    temp_unmap_raw(temp_virt);
}

static void vmm_table_copy_from(uint64_t table_phys, uint64_t *copy) {
    uint64_t temp_virt;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    memcpy(copy, (void *)temp_virt, PAGE_SIZE);
    temp_unmap_raw(temp_virt);
}

static void vmm_table_copy_to(uint64_t table_phys, const uint64_t *copy) {
    uint64_t temp_virt;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    memcpy((void *)temp_virt, copy, PAGE_SIZE);
    temp_unmap_raw(temp_virt);
}

uint64_t vmm_create_pml4(void) {
    void *page;
    uint64_t pml4_phys;
    uint64_t temp_virt;
    uint64_t *pml4;
    uint64_t pdpt_high_phys;
    uint64_t saved_flags;

    page = pmm_alloc_page();
    if (!page) return 0;
    pml4_phys = (uint64_t)page;
    pmm_zero_page_phys(pml4_phys);

    pdpt_high_phys = (uint64_t)(uintptr_t)boot_pdpt_high;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    pml4 = (uint64_t *)temp_virt;
    pml4[511] = (pdpt_high_phys & VMM_PHYS_MASK) | 3;
    __asm__ volatile ("" ::: "memory");
    temp_unmap_raw(temp_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    return pml4_phys;
}

static void vmm_free_pml4_entries(uint64_t pml4_phys, uint64_t pml4_entries, int release_leaf_refs) {
    uint64_t i;
    uint64_t j;
    uint64_t k;
    uint64_t l;
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

    for (i = 0; i < pml4_entries; i++) {
        pml4e = vmm_table_read(pml4_phys, i);
        if (!(pml4e & 1)) continue;
        pdpt_phys = pml4e & VMM_PHYS_MASK;

        for (j = 0; j < 512; j++) {
            pdpte = vmm_table_read(pdpt_phys, j);
            if (!(pdpte & 1)) continue;
            pd_phys_val = pdpte & VMM_PHYS_MASK;

            for (k = 0; k < 512; k++) {
                pde = vmm_table_read(pd_phys_val, k);
                if (!(pde & 1)) continue;
                pt_phys_val = pde & VMM_PHYS_MASK;

                for (l = 0; l < 512; l++) {
                    pte = vmm_table_read(pt_phys_val, l);
                    if ((pte & 1) && release_leaf_refs && !(pte & VMM_PTE_NOFREE)) {
                        pfa_cow_release64(pte & VMM_PHYS_MASK);
                    }
                }
                pfa_free(pt_phys_val);
            }
            pfa_free(pd_phys_val);
        }
        pfa_free(pdpt_phys);
    }

    pfa_free(pml4_phys);

    if (saved_flags & (1 << 9))
        __asm__ volatile ("sti" ::: "memory");
}

void vmm_free_pml4(uint64_t pml4_phys) {
    vmm_free_pml4_entries(pml4_phys, 511, 1);
    pfa_ref_gc();
}

static uint64_t vmm_clone_pml4_impl(uint64_t src_pml4_phys, uint64_t **out_user_pages, uint64_t *out_user_pages_count) {
    uint64_t user_page_capacity;
    uint64_t user_page_count;
    uint64_t *user_pages;
    uint64_t orig_cr3;
    uint64_t kernel_cr3;
    uint64_t new_pml4_phys;
    uint64_t *src_pt_copy;
    uint64_t *new_pt_copy;
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
    uint64_t new_cap;
    uint64_t *new_arr;
    uint64_t undo;
    int shared_ref_failed;

    user_page_capacity = 32;
    user_page_count = 0;

    user_pages = (uint64_t *)kmalloc(user_page_capacity * sizeof(uint64_t));
    if (!user_pages) return 0;
    src_pt_copy = (uint64_t *)slab_page_alloc(PAGE_SIZE);
    if (!src_pt_copy) {
        kfree(user_pages);
        return 0;
    }
    new_pt_copy = (uint64_t *)slab_page_alloc(PAGE_SIZE);
    if (!new_pt_copy) {
        slab_page_free(src_pt_copy, PAGE_SIZE);
        kfree(user_pages);
        return 0;
    }

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    alloc_page = pmm_alloc_page();
    if (!alloc_page) {
        slab_page_free(new_pt_copy, PAGE_SIZE);
        slab_page_free(src_pt_copy, PAGE_SIZE);
        kfree(user_pages);
        if (kernel_cr3 && orig_cr3 != kernel_cr3)
            __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
        return 0;
    }
    new_pml4_phys = (uint64_t)alloc_page;
    pmm_zero_page_phys(new_pml4_phys);

    pdpt_high_phys = (uint64_t)(uintptr_t)boot_pdpt_high;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    vmm_table_write(new_pml4_phys, 511,
                    (pdpt_high_phys & VMM_PHYS_MASK) | 3);

    for (i = 0; i < 511; i++) {
        pml4e = vmm_table_read(src_pml4_phys, i);
        if (!(pml4e & 1)) continue;
        src_pdpt_phys = pml4e & VMM_PHYS_MASK;

        alloc_page = pmm_alloc_page();
        if (!alloc_page) goto cleanup_fail;
        new_pdpt_phys = (uint64_t)alloc_page;
        pmm_zero_page_phys(new_pdpt_phys);

        vmm_table_write(new_pml4_phys, i,
                        (new_pdpt_phys & VMM_PHYS_MASK) |
                        (pml4e & 0x8000000000000FFFULL));

        for (j = 0; j < 512; j++) {
            pdpte = vmm_table_read(src_pdpt_phys, j);
            if (!(pdpte & 1)) continue;
            src_pd_phys = pdpte & VMM_PHYS_MASK;

            alloc_page = pmm_alloc_page();
            if (!alloc_page) goto cleanup_fail;
            new_pd_phys = (uint64_t)alloc_page;
            pmm_zero_page_phys(new_pd_phys);

            vmm_table_write(new_pdpt_phys, j,
                            (new_pd_phys & VMM_PHYS_MASK) |
                            (pdpte & 0x8000000000000FFFULL));

            for (k = 0; k < 512; k++) {
                pde = vmm_table_read(src_pd_phys, k);
                if (!(pde & 1)) continue;
                src_pt_phys = pde & VMM_PHYS_MASK;
                pde_flags = pde & 0x8000000000000FFFULL;

                alloc_page = pmm_alloc_page();
                if (!alloc_page) goto cleanup_fail;
                new_pt_phys = (uint64_t)alloc_page;
                pmm_zero_page_phys(new_pt_phys);

                vmm_table_write(new_pd_phys, k,
                                (new_pt_phys & VMM_PHYS_MASK) | pde_flags);

                vmm_table_copy_from(src_pt_phys, src_pt_copy);
                memset(new_pt_copy, 0, PAGE_SIZE);
                shared_ref_failed = 0;

                for (l = 0; l < 512; l++) {
                    src_pte = src_pt_copy[l];
                    if (!(src_pte & 1)) continue;

                    src_page_phys = src_pte & VMM_PHYS_MASK;
                    pte_flags = src_pte & 0x8000000000000FFFULL;

                    if (user_page_count >= user_page_capacity) {
                        new_cap = user_page_capacity * 2;
                        new_arr = (uint64_t *)krealloc(
                            user_pages, new_cap * sizeof(uint64_t));
                        if (!new_arr) {
                            shared_ref_failed = 1;
                            break;
                        }
                        user_pages = new_arr;
                        user_page_capacity = new_cap;
                    }

                    if (pte_flags & VMM_PTE_NOFREE) {
                        new_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) | pte_flags;
                    } else if (pte_flags & VMM_PTE_SHARED) {
                        new_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) |
                                         pte_flags;
                        if (pfa_ref_share(src_page_phys) != 0) {
                            shared_ref_failed = 1;
                            break;
                        }
                    } else if (pte_flags & 0x2) {
                        cow_flags64 = (pte_flags & ~0x2) | VMM_PTE_COW;
                        src_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) | cow_flags64;
                        new_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) | cow_flags64;
                        if (pfa_ref_share(src_page_phys) != 0) {
                            shared_ref_failed = 1;
                            break;
                        }
                    } else {
                        new_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) | pte_flags;
                        if (pfa_ref_share(src_page_phys) != 0) {
                            shared_ref_failed = 1;
                            break;
                        }
                    }

                    user_pages[user_page_count++] = src_page_phys;
                }
                if (shared_ref_failed) {
                    for (undo = 0; undo < l; undo++) {
                        src_pte = new_pt_copy[undo];
                        if (!(src_pte & 1)) continue;
                        if (src_pte & VMM_PTE_NOFREE) continue;
                        pfa_cow_release64(src_pte & VMM_PHYS_MASK);
                    }
                    goto cleanup_fail;
                }
                vmm_table_copy_to(src_pt_phys, src_pt_copy);
                vmm_table_copy_to(new_pt_phys, new_pt_copy);
            }
        }
    }

    if (orig_cr3 == src_pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(src_pml4_phys) : "memory");
    }

    smp_tlb_flush_all_sync();

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;
    slab_page_free(new_pt_copy, PAGE_SIZE);
    slab_page_free(src_pt_copy, PAGE_SIZE);

    if (kernel_cr3 && orig_cr3 != kernel_cr3 && orig_cr3 != src_pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    return new_pml4_phys;

cleanup_fail:
    vmm_free_pml4_entries(new_pml4_phys, 511, 1);

    slab_page_free(new_pt_copy, PAGE_SIZE);
    slab_page_free(src_pt_copy, PAGE_SIZE);
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
    uint64_t ref;
    uint64_t remaining_ref;
    int tracked;
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
    pdpt_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pdpt_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pd_phys_val = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pd_phys_val);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pt_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, pt_phys);
    pt = (uint64_t *)temp_virt;
    pte = pt[pt_idx];

    if (!(pte & 1) || !(pte & VMM_PTE_COW)) {
        temp_unmap_raw(temp_virt);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return 0;
    }

    old_page_phys = pte & VMM_PHYS_MASK;
    pte_flags = pte & 0x8000000000000FFFULL;

    ref = pfa_ref_get(old_page_phys);
    if (ref <= 1) {
        pt[pt_idx] = (old_page_phys & VMM_PHYS_MASK) | ((pte_flags | 0x2) & ~VMM_PTE_COW);
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

    pt[pt_idx] = (new_page_phys & VMM_PHYS_MASK) | ((pte_flags | 0x2) & ~VMM_PTE_COW);
    temp_unmap_raw(temp_virt);

    tracked = 0;
    if (current_task && current_task->pml4_phys == pml4_phys) {
        tracked = task_replace_user_page(current_task, old_page_phys, new_page_phys);
    }
    if (tracked != 0) {
        temp_virt = TEMP_SLOT(1);
        temp_map_raw(temp_virt, pt_phys);
        pt = (uint64_t *)temp_virt;
        pt[pt_idx] = (old_page_phys & VMM_PHYS_MASK) | pte_flags;
        temp_unmap_raw(temp_virt);
        pfa_free(new_page_phys);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return -1;
    }

    remaining_ref = pfa_ref_dec(old_page_phys);
    if (remaining_ref == 0) {
        pfa_free(old_page_phys);
    }

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");

    return 1;
}

uint64_t vmm_create_vring_pml4(void) {
    void *page;
    uint64_t pml4_phys;

    page = pmm_alloc_page();
    if (!page) return 0;
    pml4_phys = (uint64_t)page;
    pmm_zero_page_phys(pml4_phys);
    return pml4_phys;
}

void vmm_free_vring_pml4(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    vmm_free_pml4_entries(pml4_phys, 512, 0);
}

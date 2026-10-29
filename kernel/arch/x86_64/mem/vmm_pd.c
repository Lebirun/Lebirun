#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <lebirun/spinlock.h>
#include <lebirun/task.h>
#include <lebirun/vring.h>
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
#define CLONE_ENOMEM 12

extern spinlock_t vmm_user_lock;
#define CLONE_EAGAIN 11
#define CLONE_EINVAL 22

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
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[index];
    temp_unmap_raw(temp_virt);
    if (saved_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
    return entry;
}

static void vmm_table_write(uint64_t table_phys, uint64_t index,
                            uint64_t entry) {
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    table[index] = entry;
    __asm__ volatile ("" ::: "memory");
    temp_unmap_raw(temp_virt);
    if (saved_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static void vmm_table_copy_from(uint64_t table_phys, uint64_t *copy) {
    uint64_t temp_virt;
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    memcpy(copy, (void *)temp_virt, PAGE_SIZE);
    temp_unmap_raw(temp_virt);
    if (saved_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static void vmm_table_copy_to(uint64_t table_phys, const uint64_t *copy) {
    uint64_t temp_virt;
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, table_phys);
    memcpy((void *)temp_virt, copy, PAGE_SIZE);
    temp_unmap_raw(temp_virt);
    if (saved_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
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
    uint64_t temporary;
    uint64_t saved_flags;
    uint64_t *leaf;

    if (!pml4_phys) return;

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

                __asm__ volatile ("pushfq; popq %0; cli"
                                  : "=r"(saved_flags) :: "memory");
                temporary = TEMP_SLOT(4);
                temp_map_raw(temporary, pt_phys_val);
                leaf = (uint64_t *)temporary;
                for (l = 0; l < 512; l++) {
                    pte = leaf[l];
                    if ((pte & 1) && release_leaf_refs && !(pte & VMM_PTE_NOFREE)) {
                        pfa_cow_release64(pte & VMM_PHYS_MASK);
                    }
                }
                temp_unmap_raw(temporary);
                if (saved_flags & (1ULL << 9))
                    __asm__ volatile ("sti" ::: "memory");
                pfa_free(pt_phys_val);
            }
            pfa_free(pd_phys_val);
        }
        pfa_free(pdpt_phys);
    }

    pfa_free(pml4_phys);
}

void vmm_free_pml4(uint64_t pml4_phys) {
    vmm_free_pml4_entries(pml4_phys, 511, 1);
    pfa_ref_gc();
}

static uint64_t vmm_clone_pml4_impl(uint64_t src_pml4_phys, uint64_t **out_user_pages, uint64_t *out_user_pages_count, int *out_error) {
    uint64_t user_page_capacity;
    uint64_t user_page_count;
    uint64_t *user_pages;
    uint64_t shared_cap;
    uint64_t shared_n;
    uint64_t *shared_j;
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
    uint64_t new_cap;
    uint64_t *new_arr;
    uint64_t active_cr3;
    uint64_t undo;
    uint64_t saved_flags;
    int shared_ref_failed;
    int error;

    user_page_capacity = 32;
    user_page_count = 0;
    user_pages = NULL;
    shared_cap = 32;
    shared_n = 0;
    shared_j = NULL;
    src_pt_copy = NULL;
    new_pt_copy = NULL;
    new_pml4_phys = 0;
    error = -CLONE_ENOMEM;

    user_pages = (uint64_t *)kmalloc(user_page_capacity * sizeof(uint64_t));
    if (!user_pages) goto cleanup_fail;
    shared_j = (uint64_t *)kmalloc(shared_cap * sizeof(uint64_t));
    if (!shared_j) goto cleanup_fail;
    src_pt_copy = (uint64_t *)slab_page_alloc(PAGE_SIZE);
    if (!src_pt_copy) goto cleanup_fail;
    new_pt_copy = (uint64_t *)slab_page_alloc(PAGE_SIZE);
    if (!new_pt_copy) goto cleanup_fail;

    alloc_page = pmm_alloc_page();
    if (!alloc_page) goto cleanup_fail;
    new_pml4_phys = (uint64_t)alloc_page;
    pmm_zero_page_phys(new_pml4_phys);

    pdpt_high_phys = (uint64_t)(uintptr_t)boot_pdpt_high;

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

                __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
                spin_lock(&vmm_user_lock);
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

                    if (!(pte_flags & VMM_PTE_NOFREE)) {
                        if (shared_n >= shared_cap) {
                            new_cap = shared_cap * 2;
                            new_arr = (uint64_t *)krealloc(
                                shared_j, new_cap * sizeof(uint64_t));
                            if (!new_arr) {
                                shared_ref_failed = 1;
                                break;
                            }
                            shared_j = new_arr;
                            shared_cap = new_cap;
                        }
                        if (pfa_ref_share(src_page_phys) != 0) {
                            shared_ref_failed = 1;
                            break;
                        }
                        shared_j[shared_n++] = src_page_phys;
                    }

                    if (!(pte_flags & (VMM_PTE_NOFREE | VMM_PTE_SHARED)) &&
                        (pte_flags & 0x2)) {
                        cow_flags64 = (pte_flags & ~0x2) | VMM_PTE_COW;
                        src_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) |
                                         cow_flags64;
                        pte_flags = cow_flags64;
                    }
                    new_pt_copy[l] = (src_page_phys & VMM_PHYS_MASK) |
                                     pte_flags;
                    user_pages[user_page_count++] = src_page_phys;
                }
                if (!shared_ref_failed)
                    vmm_table_copy_to(src_pt_phys, src_pt_copy);
                spin_unlock(&vmm_user_lock);
                if (saved_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
                if (shared_ref_failed)
                    goto cleanup_fail;
                vmm_table_copy_to(new_pt_phys, new_pt_copy);
            }
        }
    }

    __asm__ volatile ("mov %%cr3, %0" : "=r"(active_cr3));
    if ((active_cr3 & VMM_PHYS_MASK) ==
        (src_pml4_phys & VMM_PHYS_MASK))
        __asm__ volatile ("mov %0, %%cr3"
                          : : "r"(active_cr3) : "memory");
    if (smp_tlb_flush_all_sync() < 0) {
        error = -CLONE_EAGAIN;
        goto cleanup_fail;
    }

    if (out_user_pages) *out_user_pages = user_pages;
    else kfree(user_pages);
    if (out_user_pages_count) *out_user_pages_count = user_page_count;
    slab_page_free(new_pt_copy, PAGE_SIZE);
    slab_page_free(src_pt_copy, PAGE_SIZE);
    if (shared_j) kfree(shared_j);

    return new_pml4_phys;

cleanup_fail:
    if (out_error) *out_error = error;
    for (undo = 0; undo < shared_n; undo++)
        pfa_cow_release64(shared_j[undo]);
    if (new_pml4_phys) vmm_free_pml4_entries(new_pml4_phys, 511, 0);

    if (new_pt_copy) slab_page_free(new_pt_copy, PAGE_SIZE);
    if (src_pt_copy) slab_page_free(src_pt_copy, PAGE_SIZE);
    if (user_pages) kfree(user_pages);
    if (shared_j) kfree(shared_j);

    return 0;
}

uint64_t vmm_clone_pml4(uint64_t src_pml4_phys, uint64_t **out_user_pages, uint64_t *out_user_pages_count, int *out_error) {
    if (out_user_pages) *out_user_pages = NULL;
    if (out_user_pages_count) *out_user_pages_count = 0;
    if (out_error) *out_error = 0;
    if (!src_pml4_phys || (src_pml4_phys & ~VMM_PHYS_MASK)) {
        if (out_error) *out_error = -CLONE_EINVAL;
        return 0;
    }
    return vmm_clone_pml4_impl(src_pml4_phys, out_user_pages, out_user_pages_count, out_error);
}

static uint64_t cow_walk(uint64_t pml4_phys, uint64_t addr, uint64_t *pt_out,
                         uint64_t *effective) {
    uint64_t table_phys;
    uint64_t entry;
    int level;

    table_phys = pml4_phys;
    *effective = VMM_PTE_WRITE | VMM_PTE_USER;
    for (level = 0; level < 3; level++) {
        entry = vmm_table_read(table_phys, (addr >> (39 - level * 9)) & 0x1FF);
        if (!(entry & 1) || (level == 2 && (entry & 0x80))) return 0;
        *effective &= entry;
        table_phys = entry & VMM_PHYS_MASK;
    }
    *pt_out = table_phys;
    entry = vmm_table_read(table_phys, (addr >> 12) & 0x1FF);
    *effective &= entry;
    return entry;
}

int cow_handle_fault(uint64_t fault_addr, uint64_t pml4_phys) {
    uint64_t pt_phys;
    uint64_t pte;
    uint64_t effective;
    uint64_t old_page_phys;
    uint64_t pte_flags;
    uint64_t new_page_phys;
    uint64_t copied_from;
    uint64_t ref;
    uint64_t remaining_ref;
    uint64_t saved_flags;
    uint64_t pt_idx;
    int tracked;

    pt_idx = (fault_addr >> 12) & 0x1FF;
    new_page_phys = 0;
    copied_from = 0;
    for (;;) {
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
        spin_lock(&vmm_user_lock);
        pte = cow_walk(pml4_phys, fault_addr, &pt_phys, &effective);
        if (!(pte & 1) || !(pte & VMM_PTE_COW)) {
            spin_unlock(&vmm_user_lock);
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            if (new_page_phys) pfa_free(new_page_phys);
            return (pte & 1) && (effective & (VMM_PTE_WRITE | VMM_PTE_USER)) ==
                   (VMM_PTE_WRITE | VMM_PTE_USER) ? 1 : 0;
        }
        old_page_phys = pte & VMM_PHYS_MASK;
        pte_flags = pte & 0x8000000000000FFFULL;
        ref = pfa_ref_get(old_page_phys);
        if (ref <= 1) {
            vmm_table_write(pt_phys, pt_idx, old_page_phys |
                            ((pte_flags | 0x2) & ~VMM_PTE_COW));
            if (ref > 0) pfa_ref_dec(old_page_phys);
            spin_unlock(&vmm_user_lock);
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            if (new_page_phys) pfa_free(new_page_phys);
            __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
            return 1;
        }
        if (new_page_phys && copied_from == old_page_phys) break;
        spin_unlock(&vmm_user_lock);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        if (!new_page_phys) {
            new_page_phys = pfa_alloc();
            if (!new_page_phys) return -1;
        }
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
        temp_map_raw(TEMP_SLOT(2), old_page_phys);
        temp_map_raw(TEMP_SLOT(3), new_page_phys);
        memcpy((void *)TEMP_SLOT(3), (void *)TEMP_SLOT(2), PAGE_SIZE);
        temp_unmap_raw(TEMP_SLOT(2));
        temp_unmap_raw(TEMP_SLOT(3));
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        copied_from = old_page_phys;
    }

    vmm_table_write(pt_phys, pt_idx, new_page_phys |
                    ((pte_flags | 0x2) & ~VMM_PTE_COW));
    tracked = 0;
    if (current_task && current_task->pml4_phys == pml4_phys)
        tracked = task_replace_user_page(current_task, old_page_phys,
                                         new_page_phys);
    if (tracked != 0) {
        vmm_table_write(pt_phys, pt_idx, pte);
        spin_unlock(&vmm_user_lock);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        pfa_free(new_page_phys);
        return -1;
    }
    remaining_ref = pfa_ref_dec(old_page_phys);
    spin_unlock(&vmm_user_lock);
    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
    vmm_user_tlb_sync(pml4_phys);
    if (remaining_ref == 0) pfa_free(old_page_phys);
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

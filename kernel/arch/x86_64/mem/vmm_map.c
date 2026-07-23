#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <string.h>

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);
extern void *pmm_alloc_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern uint64_t pfa_alloc(void);

#define VMM_PHYS_MASK 0x000FFFFFFFFFF000ULL

static int vmm_table_empty(uint64_t table_phys) {
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t i;
    int empty;

    empty = 1;
    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    for (i = 0; i < 512; i++) {
        if (table[i] & 1) {
            empty = 0;
            break;
        }
    }
    temp_unmap_raw(temp_virt);
    return empty;
}

static uint64_t vmm_user_pml4_limit(void) {
    return (KERNEL_VMA >> 39) & 0x1FF;
}

static uint64_t get_page_phys_in_pd(uint64_t pd_phys, uint64_t virt_addr) {
    uint64_t result;

    result = vmm_get_phys_in_pml4(pd_phys, virt_addr);
    return result;
}

uint64_t vmm_get_phys_in_pml4(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t entry;
    uint64_t pdpt_phys;
    uint64_t pd_phys;
    uint64_t pt_phys;
    uint64_t pte;
    uint64_t saved_flags;
    uint64_t result;

    pml4_idx = (virt_addr >> 39) & 0x1FF;
    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pml4_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { result = 0; goto out; }
    pdpt_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, pdpt_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { result = 0; goto out; }
    pd_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(2);
    temp_map_raw(temp_virt, pd_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { result = 0; goto out; }

    if (entry & 0x80) {
        uint64_t huge_phys = (entry & 0x000FFFFFFFE00000ULL) | (virt_addr & 0x1FFFFFULL);
        result = huge_phys ? huge_phys : 1;
        goto out;
    }

    pt_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, pt_phys);
    table = (uint64_t *)temp_virt;
    pte = table[pt_idx];
    temp_unmap_raw(temp_virt);
    if (!(pte & 1)) { result = 0; goto out; }

    result = pte & VMM_PHYS_MASK;
out:
    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    return result;
}

uint64_t vmm_get_flags_in_pml4(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t entry;
    uint64_t table_phys;
    uint64_t saved_flags;
    uint64_t result;
    uint64_t effective;

    pml4_idx = (virt_addr >> 39) & 0x1FF;
    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;
    result = 0;
    effective = VMM_PTE_PRESENT | VMM_PTE_WRITE | VMM_PTE_USER;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pml4_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT)) goto out;
    if (!(entry & VMM_PTE_WRITE)) effective &= ~VMM_PTE_WRITE;
    if (!(entry & VMM_PTE_USER)) effective &= ~VMM_PTE_USER;
    if (entry & VMM_PTE_NX) effective |= VMM_PTE_NX;
    table_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT)) goto out;
    if (!(entry & VMM_PTE_WRITE)) effective &= ~VMM_PTE_WRITE;
    if (!(entry & VMM_PTE_USER)) effective &= ~VMM_PTE_USER;
    if (entry & VMM_PTE_NX) effective |= VMM_PTE_NX;
    table_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(2);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT)) goto out;
    if (!(entry & VMM_PTE_WRITE)) effective &= ~VMM_PTE_WRITE;
    if (!(entry & VMM_PTE_USER)) effective &= ~VMM_PTE_USER;
    if (entry & VMM_PTE_NX) effective |= VMM_PTE_NX;
    if (entry & 0x80) {
        result = effective | (entry & (VMM_PTE_COW | VMM_PTE_NOFREE));
        goto out;
    }
    table_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT)) goto out;
    if (!(entry & VMM_PTE_WRITE)) effective &= ~VMM_PTE_WRITE;
    if (!(entry & VMM_PTE_USER)) effective &= ~VMM_PTE_USER;
    if (entry & VMM_PTE_NX) effective |= VMM_PTE_NX;
    result = effective | (entry & (VMM_PTE_COW | VMM_PTE_NOFREE));

out:
    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    return result;
}

int vmm_protect_page_in_pml4(uint64_t pml4_phys, uint64_t virt_addr,
                             uint64_t flags) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t entry;
    uint64_t table_phys;
    uint64_t saved_flags;
    uint64_t current_cr3;
    uint64_t preserved;
    int result;

    if (!pml4_phys || virt_addr >= KERNEL_VMA) return -1;
    pml4_idx = (virt_addr >> 39) & 0x1FF;
    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;
    result = -1;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pml4_idx];
    if (entry & VMM_PTE_PRESENT)
        table[pml4_idx] |= flags & (VMM_PTE_WRITE | VMM_PTE_USER);
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT)) goto out;
    table_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    if (entry & VMM_PTE_PRESENT)
        table[pdpt_idx] |= flags & (VMM_PTE_WRITE | VMM_PTE_USER);
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT)) goto out;
    table_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(2);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    if (entry & VMM_PTE_PRESENT)
        table[pd_idx] |= flags & (VMM_PTE_WRITE | VMM_PTE_USER);
    temp_unmap_raw(temp_virt);
    if (!(entry & VMM_PTE_PRESENT) || (entry & 0x80)) goto out;
    table_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, table_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pt_idx];
    if (!(entry & VMM_PTE_PRESENT)) {
        temp_unmap_raw(temp_virt);
        goto out;
    }
    preserved = entry & (VMM_PHYS_MASK | VMM_PTE_COW | VMM_PTE_NOFREE);
    table[pt_idx] = preserved | (flags & (VMM_PTE_PRESENT | VMM_PTE_WRITE |
                                          VMM_PTE_USER | VMM_PTE_NX));
    temp_unmap_raw(temp_virt);
    result = 0;

out:
    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    if (result == 0) {
        __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3));
        if (current_cr3 == pml4_phys) {
            __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");
        }
    }
    return result;
}

int vmm_protect_range_in_pml4(uint64_t pml4_phys, uint64_t virt_addr,
                              uint64_t size, uint64_t flags) {
    uint64_t start;
    uint64_t end;
    uint64_t page;

    if (size == 0) return 0;
    if (virt_addr >= KERNEL_VMA || size > KERNEL_VMA - virt_addr) return -1;
    start = virt_addr & ~(PAGE_SIZE - 1);
    end = virt_addr + size;
    if (end & (PAGE_SIZE - 1)) {
        if (end > KERNEL_VMA - PAGE_SIZE) return -1;
        end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }
    if (end < start || end > KERNEL_VMA) return -1;
    for (page = start; page < end; page += PAGE_SIZE) {
        if (vmm_protect_page_in_pml4(pml4_phys, page, flags) < 0) return -1;
    }
    return 0;
}

uint64_t vmm_unmap_page_in_pml4(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t temp_virt;
    uint64_t *table;
    uint64_t entry;
    uint64_t pdpt_phys;
    uint64_t pd_phys;
    uint64_t pt_phys;
    uint64_t pte;
    uint64_t phys;
    uint64_t cur_cr3;
    uint64_t saved_flags;

    pml4_idx = (virt_addr >> 39) & 0x1FF;
    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_virt = TEMP_SLOT(0);
    temp_map_raw(temp_virt, pml4_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pml4_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pdpt_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, pdpt_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pd_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(2);
    temp_map_raw(temp_virt, pd_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pt_phys = entry & VMM_PHYS_MASK;

    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, pt_phys);
    table = (uint64_t *)temp_virt;
    pte = table[pt_idx];
    if (!(pte & 1)) {
        temp_unmap_raw(temp_virt);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return 0;
    }
    phys = pte & VMM_PHYS_MASK;
    table[pt_idx] = 0;
    temp_unmap_raw(temp_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == pml4_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return phys;
}

void vmm_prune_user_range(uint64_t pml4_phys, uint64_t virt_addr, uint64_t size) {
    uint64_t start;
    uint64_t end;
    uint64_t v;
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t user_limit;
    uint64_t temp_pml4;
    uint64_t temp_pdpt;
    uint64_t temp_pd;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pdpt_phys;
    uint64_t pd_phys;
    uint64_t pt_phys;
    uint64_t saved_flags;

    if (!pml4_phys || size == 0) return;
    if (virt_addr >= KERNEL_VMA) return;

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end < start || end > KERNEL_VMA) end = KERNEL_VMA;
    user_limit = vmm_user_pml4_limit();

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_pml4 = TEMP_SLOT(0);
    temp_pdpt = TEMP_SLOT(1);
    temp_pd = TEMP_SLOT(2);
    temp_map_raw(temp_pml4, pml4_phys);
    pml4 = (uint64_t *)temp_pml4;

    for (v = start; v < end; v = ((v + 0x200000ULL) & ~0x1FFFFFULL)) {
        if (v < start) break;
        pml4_idx = (v >> 39) & 0x1FF;
        pdpt_idx = (v >> 30) & 0x1FF;
        pd_idx = (v >> 21) & 0x1FF;
        if (pml4_idx >= user_limit) break;

        pml4e = pml4[pml4_idx];
        if (!(pml4e & 1)) continue;
        pdpt_phys = pml4e & VMM_PHYS_MASK;

        temp_map_raw(temp_pdpt, pdpt_phys);
        pdpt = (uint64_t *)temp_pdpt;
        pdpte = pdpt[pdpt_idx];
        if (!(pdpte & 1)) {
            temp_unmap_raw(temp_pdpt);
            continue;
        }
        pd_phys = pdpte & VMM_PHYS_MASK;

        temp_map_raw(temp_pd, pd_phys);
        pd = (uint64_t *)temp_pd;
        pde = pd[pd_idx];
        if ((pde & 1) && !(pde & 0x80)) {
            pt_phys = pde & VMM_PHYS_MASK;
            if (vmm_table_empty(pt_phys)) {
                pd[pd_idx] = 0;
                pfa_free(pt_phys);
            }
        }
        temp_unmap_raw(temp_pd);

        if (vmm_table_empty(pd_phys)) {
            pdpt[pdpt_idx] = 0;
            pfa_free(pd_phys);
        }
        temp_unmap_raw(temp_pdpt);

        if (vmm_table_empty(pdpt_phys)) {
            pml4[pml4_idx] = 0;
            pfa_free(pdpt_phys);
        }
    }

    temp_unmap_raw(temp_pml4);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

uint64_t vmm_count_user_page_tables(uint64_t pml4_phys) {
    uint64_t count;
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t user_limit;
    uint64_t temp_pml4;
    uint64_t temp_pdpt;
    uint64_t temp_pd;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t saved_flags;

    if (!pml4_phys) return 0;
    count = 0;
    user_limit = vmm_user_pml4_limit();

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_pml4 = TEMP_SLOT(0);
    temp_pdpt = TEMP_SLOT(1);
    temp_pd = TEMP_SLOT(2);
    temp_map_raw(temp_pml4, pml4_phys);
    pml4 = (uint64_t *)temp_pml4;

    for (pml4_idx = 0; pml4_idx < user_limit; pml4_idx++) {
        pml4e = pml4[pml4_idx];
        if (!(pml4e & 1)) continue;
        count++;
        temp_map_raw(temp_pdpt, pml4e & VMM_PHYS_MASK);
        pdpt = (uint64_t *)temp_pdpt;
        for (pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            pdpte = pdpt[pdpt_idx];
            if (!(pdpte & 1)) continue;
            count++;
            temp_map_raw(temp_pd, pdpte & VMM_PHYS_MASK);
            pd = (uint64_t *)temp_pd;
            for (pd_idx = 0; pd_idx < 512; pd_idx++) {
                pde = pd[pd_idx];
                if (!(pde & 1)) continue;
                if (pde & 0x80) continue;
                count++;
            }
            temp_unmap_raw(temp_pd);
        }
        temp_unmap_raw(temp_pdpt);
    }

    temp_unmap_raw(temp_pml4);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    return count;
}

uint64_t vmm_count_present_pages_in_range(uint64_t pml4_phys, uint64_t start, uint64_t end) {
    uint64_t count;
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t user_limit;
    uint64_t temp_pml4;
    uint64_t temp_pdpt;
    uint64_t temp_pd;
    uint64_t temp_pt;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;
    uint64_t base;
    uint64_t huge_end;
    uint64_t overlap_start;
    uint64_t overlap_end;
    uint64_t saved_flags;

    if (!pml4_phys) return 0;
    if (end <= start) return 0;
    start &= ~0xFFFULL;
    end = (end + 0xFFFULL) & ~0xFFFULL;
    if (end > KERNEL_VMA || end < start) end = KERNEL_VMA;
    if (start >= end) return 0;
    count = 0;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    user_limit = vmm_user_pml4_limit();
    temp_pml4 = TEMP_SLOT(0);
    temp_pdpt = TEMP_SLOT(1);
    temp_pd = TEMP_SLOT(2);
    temp_pt = TEMP_SLOT(3);
    temp_map_raw(temp_pml4, pml4_phys);
    pml4 = (uint64_t *)temp_pml4;

    for (pml4_idx = 0; pml4_idx < user_limit; pml4_idx++) {
        base = pml4_idx << 39;
        if (base >= end) break;
        if (base + (1ULL << 39) <= start) continue;
        pml4e = pml4[pml4_idx];
        if (!(pml4e & 1)) continue;
        temp_map_raw(temp_pdpt, pml4e & VMM_PHYS_MASK);
        pdpt = (uint64_t *)temp_pdpt;
        for (pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            base = (pml4_idx << 39) | (pdpt_idx << 30);
            if (base >= end) break;
            if (base + (1ULL << 30) <= start) continue;
            pdpte = pdpt[pdpt_idx];
            if (!(pdpte & 1)) continue;
            temp_map_raw(temp_pd, pdpte & VMM_PHYS_MASK);
            pd = (uint64_t *)temp_pd;
            for (pd_idx = 0; pd_idx < 512; pd_idx++) {
                base = (pml4_idx << 39) | (pdpt_idx << 30) | (pd_idx << 21);
                if (base >= end) break;
                if (base + (1ULL << 21) <= start) continue;
                pde = pd[pd_idx];
                if (!(pde & 1)) continue;
                if (pde & 0x80) {
                    huge_end = base + (1ULL << 21);
                    overlap_start = start > base ? start : base;
                    overlap_end = end < huge_end ? end : huge_end;
                    if (overlap_end > overlap_start) count += (overlap_end - overlap_start) >> 12;
                    continue;
                }
                temp_map_raw(temp_pt, pde & VMM_PHYS_MASK);
                pt = (uint64_t *)temp_pt;
                for (pt_idx = 0; pt_idx < 512; pt_idx++) {
                    base = (pml4_idx << 39) | (pdpt_idx << 30) | (pd_idx << 21) | (pt_idx << 12);
                    if (base >= end) break;
                    if (base < start) continue;
                    pte = pt[pt_idx];
                    if (pte & 1) count++;
                }
                temp_unmap_raw(temp_pt);
            }
            temp_unmap_raw(temp_pd);
        }
        temp_unmap_raw(temp_pdpt);
    }

    temp_unmap_raw(temp_pml4);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    return count;
}

int vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t pml4_idx;
    uint64_t pdpt_idx;
    uint64_t pd_idx;
    uint64_t pt_idx;
    uint64_t temp_pml4_virt;
    uint64_t temp_pdpt_virt;
    uint64_t temp_pd_virt;
    uint64_t temp_pt_virt;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pdpt_phys;
    uint64_t pd_phys;
    uint64_t pt_phys;
    uint64_t cur_cr3;
    uint64_t alloc_flags;
    uint64_t saved_flags;
    void *new_page;

    if (virt_addr >= KERNEL_VMA) {
        flags &= ~0x4;
    }

    pml4_idx = (virt_addr >> 39) & 0x1FF;
    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pd_idx = (virt_addr >> 21) & 0x1FF;
    pt_idx = (virt_addr >> 12) & 0x1FF;

    alloc_flags = (flags & 0x7) | 1;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    temp_pml4_virt = TEMP_SLOT(0);
    temp_map_raw(temp_pml4_virt, pml4_phys);
    pml4 = (uint64_t *)temp_pml4_virt;

    if (!(pml4[pml4_idx] & 1)) {
        new_page = pmm_alloc_page();
        if (!new_page) {
            temp_unmap_raw(temp_pml4_virt);
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            printf("vmm_map_page_in_pml4: Failed to alloc PDPT\n");
            return -1;
        }
        pdpt_phys = (uint64_t)new_page;
        pmm_zero_page_phys(pdpt_phys);
        pml4[pml4_idx] = (pdpt_phys & VMM_PHYS_MASK) | alloc_flags;
    } else {
        pdpt_phys = pml4[pml4_idx] & VMM_PHYS_MASK;
        if ((flags & 0x7) & ~(pml4[pml4_idx] & 0x7)) {
            pml4[pml4_idx] |= (flags & 0x7);
        }
    }
    temp_unmap_raw(temp_pml4_virt);

    temp_pdpt_virt = TEMP_SLOT(1);
    temp_map_raw(temp_pdpt_virt, pdpt_phys);
    pdpt = (uint64_t *)temp_pdpt_virt;

    if (!(pdpt[pdpt_idx] & 1)) {
        new_page = pmm_alloc_page();
        if (!new_page) {
            temp_unmap_raw(temp_pdpt_virt);
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            vmm_prune_user_range(pml4_phys, virt_addr, PAGE_SIZE);
            printf("vmm_map_page_in_pml4: Failed to alloc PD\n");
            return -1;
        }
        pd_phys = (uint64_t)new_page;
        pmm_zero_page_phys(pd_phys);
        pdpt[pdpt_idx] = (pd_phys & VMM_PHYS_MASK) | alloc_flags;
    } else {
        pd_phys = pdpt[pdpt_idx] & VMM_PHYS_MASK;
        if ((flags & 0x7) & ~(pdpt[pdpt_idx] & 0x7)) {
            pdpt[pdpt_idx] |= (flags & 0x7);
        }
    }
    temp_unmap_raw(temp_pdpt_virt);

    temp_pd_virt = TEMP_SLOT(2);
    temp_map_raw(temp_pd_virt, pd_phys);
    pd = (uint64_t *)temp_pd_virt;

    if (!(pd[pd_idx] & 1)) {
        new_page = pmm_alloc_page();
        if (!new_page) {
            temp_unmap_raw(temp_pd_virt);
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            vmm_prune_user_range(pml4_phys, virt_addr, PAGE_SIZE);
            printf("vmm_map_page_in_pml4: Failed to alloc PT\n");
            return -1;
        }
        pt_phys = (uint64_t)new_page;
        pmm_zero_page_phys(pt_phys);
        pd[pd_idx] = (pt_phys & VMM_PHYS_MASK) | alloc_flags;
    } else {
        pt_phys = pd[pd_idx] & VMM_PHYS_MASK;
        if ((flags & 0x7) & ~(pd[pd_idx] & 0x7)) {
            pd[pd_idx] |= (flags & 0x7);
        }
    }
    temp_unmap_raw(temp_pd_virt);

    temp_pt_virt = TEMP_SLOT(3);
    temp_map_raw(temp_pt_virt, pt_phys);
    pt = (uint64_t *)temp_pt_virt;
    pt[pt_idx] = (phys_addr & VMM_PHYS_MASK) | (flags & 0x8000000000000FFFULL);
    temp_unmap_raw(temp_pt_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == pml4_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return 0;
}

void vmm_map_range_in_pml4(uint64_t pd_phys, uint64_t virt_addr, uint64_t size, uint64_t flags) {
    uint64_t start;
    uint64_t end;
    uint64_t v;
    uint64_t mapped;
    uint64_t phys_page;

    if (size == 0) return;
    if (size > UINT64_MAX - virt_addr) return;
    mapped = virt_addr + size;
    if (mapped > UINT64_MAX - (PAGE_SIZE - 1)) return;

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (mapped + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= KERNEL_VMA) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pml4: Failed to alloc phys page\n");
            while (v > start) {
                v -= PAGE_SIZE;
                phys_page = vmm_unmap_page_in_pml4(pd_phys, v);
                if (phys_page) pfa_free(phys_page);
            }
            vmm_prune_user_range(pd_phys, start, end - start);
            return;
        }
        pmm_zero_page_phys(phys_page);
        if (vmm_map_page_in_pml4(pd_phys, v, phys_page, flags) < 0) {
            pfa_free(phys_page);
            while (v > start) {
                v -= PAGE_SIZE;
                phys_page = vmm_unmap_page_in_pml4(pd_phys, v);
                if (phys_page) pfa_free(phys_page);
            }
            vmm_prune_user_range(pd_phys, start, end - start);
            return;
        }
    }
}

uint64_t* vmm_map_range_in_pml4_tracked(uint64_t pd_phys, uint64_t virt_addr, uint64_t size, uint64_t flags, uint64_t *out_count) {
    uint64_t start;
    uint64_t end;
    uint64_t num_pages;
    uint64_t *pages;
    uint64_t idx;
    uint64_t v;
    uint64_t phys_page;
    uint64_t mapped;
    uint64_t i;

    if (size == 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (size > UINT64_MAX - virt_addr) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    mapped = virt_addr + size;
    if (mapped > UINT64_MAX - (PAGE_SIZE - 1)) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (mapped + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    num_pages = (end - start) / PAGE_SIZE;

    if (start >= KERNEL_VMA) {
        flags &= ~0x4;
    }
    
    pages = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
    if (!pages) {
        printf("vmm_map_range_in_pml4_tracked: kmalloc failed for %u pages (%u bytes)\n",
               num_pages, num_pages * sizeof(uint64_t));
        if (out_count) *out_count = 0;
        return NULL;
    }

    idx = 0;
    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pml4_tracked: Failed to alloc phys page %u/%u (free=%u)\n", idx, num_pages, pfa_count_free());
            for (i = 0; i < idx; i++) {
                vmm_unmap_page_in_pml4(pd_phys, start + i * PAGE_SIZE);
                pfa_free(pages[i]);
            }
            vmm_prune_user_range(pd_phys, start, idx * PAGE_SIZE);
            kfree(pages);
            if (out_count) *out_count = 0;
            return NULL;
        }
        pmm_zero_page_phys(phys_page);
        if (vmm_map_page_in_pml4(pd_phys, v, phys_page, flags) < 0) {
            pfa_free(phys_page);
            for (i = 0; i < idx; i++) {
                vmm_unmap_page_in_pml4(pd_phys, start + i * PAGE_SIZE);
                pfa_free(pages[i]);
            }
            vmm_prune_user_range(pd_phys, start, (idx + 1) * PAGE_SIZE);
            kfree(pages);
            if (out_count) *out_count = 0;
            return NULL;
        }
        pages[idx++] = phys_page;
    }
    
    if (out_count) *out_count = num_pages;
    return pages;
}

void vmm_copy_to_pml4(uint64_t pd_phys, uint64_t dest_virt, const void *src, uint64_t size) {
    const uint8_t *s = (const uint8_t *)src;
    uint64_t offset = 0;
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    while (offset < size) {
        uint64_t page_offset = (dest_virt + offset) & 0xFFF;
        uint64_t chunk = PAGE_SIZE - page_offset;
        if (chunk > size - offset) chunk = size - offset;

        uint64_t virt_page = (dest_virt + offset) & ~0xFFF;
        uint64_t page_phys = get_page_phys_in_pd(pd_phys, virt_page);
        if (page_phys == 0) {
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            printf("vmm_copy_to_pml4: page not mapped for 0x%016lX\n", virt_page);
            return;
        }

        temp_map_raw(TEMP_SLOT(3), page_phys);
        memcpy((void *)(TEMP_SLOT(3) + page_offset), s + offset, chunk);
        temp_unmap_raw(TEMP_SLOT(3));

        offset += chunk;
    }

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

void vmm_read_from_pml4(uint64_t pd_phys, uint64_t src_virt, void *dest, uint64_t size) {
    uint8_t *d = (uint8_t *)dest;
    uint64_t offset = 0;
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

    while (offset < size) {
        uint64_t page_offset = (src_virt + offset) & 0xFFF;
        uint64_t chunk = PAGE_SIZE - page_offset;
        if (chunk > size - offset) chunk = size - offset;

        uint64_t virt_page = (src_virt + offset) & ~0xFFF;
        uint64_t page_phys = get_page_phys_in_pd(pd_phys, virt_page);
        if (page_phys == 0) {
            if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
            return;
        }

        temp_map_raw(TEMP_SLOT(3), page_phys);
        memcpy(d + offset, (void *)(TEMP_SLOT(3) + page_offset), chunk);
        temp_unmap_raw(TEMP_SLOT(3));

        offset += chunk;
    }

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

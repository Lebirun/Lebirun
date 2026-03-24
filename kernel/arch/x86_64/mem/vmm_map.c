#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);
extern void *pmm_alloc_page(void);
extern void pmm_zero_page_phys(uint64_t phys_addr);
extern uint64_t pfa_alloc(void);

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
    pdpt_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, pdpt_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { result = 0; goto out; }
    pd_phys = entry & ~0xFFFULL;

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

    pt_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, pt_phys);
    table = (uint64_t *)temp_virt;
    pte = table[pt_idx];
    temp_unmap_raw(temp_virt);
    if (!(pte & 1)) { result = 0; goto out; }

    result = pte & ~0xFFFULL;
out:
    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
    return result;
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
    pdpt_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(1);
    temp_map_raw(temp_virt, pdpt_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pdpt_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pd_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(2);
    temp_map_raw(temp_virt, pd_phys);
    table = (uint64_t *)temp_virt;
    entry = table[pd_idx];
    temp_unmap_raw(temp_virt);
    if (!(entry & 1)) { if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory"); return 0; }
    pt_phys = entry & ~0xFFFULL;

    temp_virt = TEMP_SLOT(3);
    temp_map_raw(temp_virt, pt_phys);
    table = (uint64_t *)temp_virt;
    pte = table[pt_idx];
    if (!(pte & 1)) {
        temp_unmap_raw(temp_virt);
        if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
        return 0;
    }
    phys = pte & ~0xFFFULL;
    table[pt_idx] = 0;
    temp_unmap_raw(temp_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == pml4_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return phys;
}

void vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
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
            return;
        }
        pdpt_phys = (uint64_t)new_page;
        pmm_zero_page_phys(pdpt_phys);
        pml4[pml4_idx] = (pdpt_phys & ~0xFFFULL) | alloc_flags;
    } else {
        pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
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
            printf("vmm_map_page_in_pml4: Failed to alloc PD\n");
            return;
        }
        pd_phys = (uint64_t)new_page;
        pmm_zero_page_phys(pd_phys);
        pdpt[pdpt_idx] = (pd_phys & ~0xFFFULL) | alloc_flags;
    } else {
        pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
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
            printf("vmm_map_page_in_pml4: Failed to alloc PT\n");
            return;
        }
        pt_phys = (uint64_t)new_page;
        pmm_zero_page_phys(pt_phys);
        pd[pd_idx] = (pt_phys & ~0xFFFULL) | alloc_flags;
    } else {
        pt_phys = pd[pd_idx] & ~0xFFFULL;
        if ((flags & 0x7) & ~(pd[pd_idx] & 0x7)) {
            pd[pd_idx] |= (flags & 0x7);
        }
    }
    temp_unmap_raw(temp_pd_virt);

    temp_pt_virt = TEMP_SLOT(3);
    temp_map_raw(temp_pt_virt, pt_phys);
    pt = (uint64_t *)temp_pt_virt;
    pt[pt_idx] = (phys_addr & ~0xFFFULL) | (flags & 0xFFF);
    temp_unmap_raw(temp_pt_virt);

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == pml4_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
}

void vmm_map_range_in_pml4(uint64_t pd_phys, uint64_t virt_addr, uint64_t size, uint64_t flags) {
    uint64_t start;
    uint64_t end;
    uint64_t v;
    uint64_t phys_page;

    if (size == 0) return;

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= KERNEL_VMA) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pml4: Failed to alloc phys page\n");
            return;
        }
        pmm_zero_page_phys(phys_page);
        vmm_map_page_in_pml4(pd_phys, v, phys_page, flags);
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
    uint64_t i;

    if (size == 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    num_pages = (end - start) / PAGE_SIZE;

    if (start >= KERNEL_VMA) {
        flags &= ~0x4;
    }
    
    pages = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
    if (!pages) {
        printf("vmm_map_range_in_pml4_tracked: kmalloc failed for %u pages (%u bytes)\n", num_pages, num_pages * 4);
        if (out_count) *out_count = 0;
        return NULL;
    }

    idx = 0;
    for (v = start; v < end; v += PAGE_SIZE) {
        phys_page = pfa_alloc();
        if (!phys_page) {
            printf("vmm_map_range_in_pml4_tracked: Failed to alloc phys page %u/%u (free=%u)\n", idx, num_pages, pfa_count_free());
            for (i = 0; i < idx; i++) {
                pfa_free(pages[i]);
            }
            kfree(pages);
            if (out_count) *out_count = 0;
            return NULL;
        }
        pmm_zero_page_phys(phys_page);
        pages[idx++] = phys_page;
        vmm_map_page_in_pml4(pd_phys, v, phys_page, flags);
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

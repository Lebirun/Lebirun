#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;
extern uint64_t boot_pd_high[] __attribute__((aligned(4096)));

extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pmm_zero_page_phys(uint32_t phys_addr);
extern void vmm_map_page_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
extern void vmm_map_page_early_pae(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
extern void vmm_map_range_alloc_pae(uint32_t virt_addr, uint32_t size, uint32_t flags);
extern int heap_map_page_pae(uint32_t virt_addr);
extern void vmm_unmap_page_pae(uint32_t virt_addr);

static struct {
    uint32_t v;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t old;
    uint32_t newval;
    uint32_t pd1023;
    uint32_t pd_pde;
    uint32_t op_count;
} map_debug = {0};

void vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt_new;
    uint32_t *pt;

    if (pae_enabled) {
        vmm_map_page_pae(virt_addr, phys_addr, flags);
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) {
        pt_page = pmm_alloc_low_page();
        if (!pt_page) {
            pt_page = pmm_alloc_page();
        }
        if (!pt_page) {
            printf("vmm_map_page: Failed to alloc page table\n");
            return;
        }
        pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | (flags & 0xFFF);
        pt_addr = 0xFFC00000 + (pd_idx << 12);
        __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
        pt_new = (uint32_t *)pt_addr;
        memset(pt_new, 0, PAGE_SIZE);
    } else {
        if ((flags & 0x4) && !(pd[pd_idx] & 0x4)) {
            pd[pd_idx] |= 0x4;
        }
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_map_page_early_avail(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    uint32_t *pt;

    if (pae_enabled) {
        vmm_map_page_early_pae(virt_addr, phys_addr, flags);
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    if (virt_addr >= 0xC0000000) {
        flags &= ~0x4;
    }

    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) {
        return;
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF);
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void vmm_map_range_alloc(uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;
    uint32_t v;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt_new;
    uint32_t *pt;
    void *phys_page;
    uint32_t old;
    uint32_t newval;
    uint32_t cr3;

    if (pae_enabled) {
        vmm_map_range_alloc_pae(virt_addr, size, flags);
        return;
    }

    if (size == 0) return;
    start = virt_addr & ~(PAGE_SIZE - 1);
    end = (virt_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (start >= 0xC0000000) {
        flags &= ~0x4;
    }

    for (v = start; v < end; v += PAGE_SIZE) {
        pd_idx = v >> 22;
        pt_idx = (v >> 12) & 0x3FF;
        pd = (uint32_t *)0xFFFFF000;

        if (!(pd[pd_idx] & 1)) {
            pt_page = pmm_alloc_low_page();
            if (!pt_page) {
                pt_page = pmm_alloc_page();
            }
            if (!pt_page) {
                printf("vmm_map_range_alloc: Failed to alloc page table\n");
                return;
            }
            pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | (flags & 0xFFF);
            pt_addr = 0xFFC00000 + (pd_idx << 12);
            __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
            pt_new = (uint32_t *)pt_addr;
            memset(pt_new, 0, PAGE_SIZE);
        } else {
            if ((flags & 0x4) && !(pd[pd_idx] & 0x4)) {
                pd[pd_idx] |= 0x4;
                __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
                __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
            }
        }

        pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
        if (!(pt[pt_idx] & 1)) {
            phys_page = pmm_alloc_page();
            if (!phys_page) {
                printf("vmm_map_range_alloc: Failed to alloc phys page\n");
                return;
            }
            old = pt[pt_idx];
            newval = ((uint32_t)phys_page & ~0xFFF) | (flags & 0xFFF);
            pt[pt_idx] = newval;
            map_debug.v = v; map_debug.pd_idx = pd_idx; map_debug.pt_idx = pt_idx; map_debug.old = old; map_debug.newval = newval; map_debug.pd1023 = ((uint32_t *)0xFFFFF000)[1023]; map_debug.pd_pde = pd[pd_idx]; map_debug.op_count++;
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        } else {
            old = pt[pt_idx];
            newval = (old & ~0xFFF) | (flags & 0xFFF);
            pt[pt_idx] = newval;
            map_debug.v = v; map_debug.pd_idx = pd_idx; map_debug.pt_idx = pt_idx; map_debug.old = old; map_debug.newval = newval; map_debug.pd1023 = ((uint32_t *)0xFFFFF000)[1023]; map_debug.pd_pde = pd[pd_idx]; map_debug.op_count++;
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        }
    }
}

int heap_map_page(uint32_t virt_addr) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    void *pt_page;
    uint32_t pt_addr;
    uint32_t *pt_new;
    uint32_t *pt;
    void *phys_page;

    if (pae_enabled) {
        return heap_map_page_pae(virt_addr);
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;

    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) {
        pt_page = pmm_alloc_low_page();
        if (!pt_page) {
            pt_page = pmm_alloc_page();
        }
        if (!pt_page) {
            printf("heap_map_page: Failed to alloc page table\n");
            return -1;
        }
        pd[pd_idx] = ((uint32_t)pt_page & ~0xFFF) | 3;
        pt_addr = 0xFFC00000 + (pd_idx << 12);
        __asm__ volatile("invlpg (%0)" : : "r"(pt_addr) : "memory");
        pt_new = (uint32_t *)pt_addr;
        memset(pt_new, 0, PAGE_SIZE);
    }

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));

    if (!(pt[pt_idx] & 1)) {
        phys_page = pmm_alloc_page();
        if (!phys_page) {
            printf("heap_map_page: Failed to alloc phys page\n");
            return -1;
        }
        pt[pt_idx] = ((uint32_t)phys_page & ~0xFFF) | 3;

        __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    }
    return 0;
}

void vmm_unmap_page(uint32_t virt_addr) {
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint32_t *pd;
    uint32_t *pt;

    if (pae_enabled) {
        vmm_unmap_page_pae(virt_addr);
        return;
    }

    pd_idx = virt_addr >> 22;
    pt_idx = (virt_addr >> 12) & 0x3FF;
    pd = (uint32_t *)0xFFFFF000;

    if (!(pd[pd_idx] & 1)) return;

    pt = (uint32_t *)(0xFFC00000 + (pd_idx << 12));
    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void dump_map_debug(void) {
    printf("map_debug: v=0x%08X pd_idx=%u pt_idx=%u old=0x%08X new=0x%08X pd1023=0x%08X pd_pde=0x%08X op_count=%u\n",
           map_debug.v, map_debug.pd_idx, map_debug.pt_idx, map_debug.old, map_debug.newval, map_debug.pd1023, map_debug.pd_pde, map_debug.op_count);
}

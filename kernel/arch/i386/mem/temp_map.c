#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern uint32_t pae_enabled;
extern uint32_t kernel_pd_phys;
extern uint32_t kernel_pdpt_phys;
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern int pae_ensure_phys_mapped(uint32_t phys_addr);

#define TEMP_MAP_BASE 0xF7000000u
#define TEMP_MAP_PAGES 8u

static uint32_t pae_temp_pt_ready = 0;
static uint64_t *pae_temp_pt = NULL;
static uint32_t pae_temp_pd_idx = 0;

static volatile int temp_map_lock = 0;

uint32_t pae_temp_pt_ready_check(void) {
    return pae_temp_pt_ready;
}

static inline void temp_lock_acquire(uint32_t *eflags_out) {
    __asm__ volatile ("pushf; pop %0" : "=r"(*eflags_out));
    __asm__ volatile ("cli");
    while (__sync_lock_test_and_set(&temp_map_lock, 1)) {
    }
}

static inline void temp_lock_release(uint32_t eflags) {
    __sync_lock_release(&temp_map_lock);
    if (eflags & (1 << 9)) __asm__ volatile ("sti");
}

extern void pae_sync_kernel_mappings(void);
extern uint64_t *pae_vmm_page_tables[32];
extern uint32_t pae_vmm_pt_count;
extern uint64_t boot_pd_high[];

void pae_init_temp_mapping(void) {
    uint32_t pd_idx;
    uint64_t pde;
    uint64_t *pd;
    uint64_t *pt;
    uint32_t pt_phys;
    uint32_t pt_slot;

    if (!pae_enabled) return;
    if (pae_temp_pt_ready) return;

    pd_idx = (TEMP_MAP_BASE >> 21) & 0x1FF;
    pd = boot_pd_high;
    pde = pd[pd_idx];

    if (pde & 1) {
        pt_phys = (uint32_t)(pde & ~0xFFFULL);
        pt = (uint64_t *)(pt_phys + 0xC0000000);
        pae_temp_pt = pt;
        pae_temp_pd_idx = pd_idx;
        pae_temp_pt_ready = 1;
        return;
    }

    if (pae_vmm_pt_count >= 32) {
        printf("pae_init_temp_mapping: out of vmm page tables\n");
        return;
    }

    {
        void *pt_page;
        pt_page = pmm_alloc_low_page();
        if (!pt_page) pt_page = pmm_alloc_page();
        if (!pt_page) {
            printf("pae_init_temp_mapping: failed to alloc page table\n");
            return;
        }
        if (pae_ensure_phys_mapped((uint32_t)pt_page) < 0) {
            printf("pae_init_temp_mapping: failed to map page table\n");
            return;
        }
        pt_slot = pae_vmm_pt_count++;
        pt = (uint64_t *)((uint32_t)pt_page + 0xC0000000);
        pae_vmm_page_tables[pt_slot] = pt;
        memset(pt, 0, PAGE_SIZE);
        pt_phys = (uint32_t)pt_page;
    }
    pd[pd_idx] = ((uint64_t)pt_phys & ~0xFFFULL) | 3;
    pae_sync_kernel_mappings();

    pae_temp_pt = pt;
    pae_temp_pd_idx = pd_idx;
    pae_temp_pt_ready = 1;
}

void temp_map_raw_pae(uint32_t temp_virt, uint64_t phys_addr) {
    uint32_t flags;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t *pt;

    temp_lock_acquire(&flags);

    pdpt_idx = (temp_virt >> 30) & 0x3;
    pd_idx = (temp_virt >> 21) & 0x1FF;
    pt_idx = (temp_virt >> 12) & 0x1FF;

    if (pdpt_idx != 3) {
        temp_lock_release(flags);
        return;
    }

    if (!pae_temp_pt_ready) {
        pae_init_temp_mapping();
    }
    if (!pae_temp_pt_ready || pd_idx != pae_temp_pd_idx) {
        temp_lock_release(flags);
        return;
    }

    pt = pae_temp_pt;
    pt[pt_idx] = (phys_addr & ~0xFFFULL) | 3;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    temp_lock_release(flags);
}

void temp_unmap_raw_pae(uint32_t temp_virt) {
    uint32_t flags;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t *pt;

    temp_lock_acquire(&flags);

    pdpt_idx = (temp_virt >> 30) & 0x3;
    pd_idx = (temp_virt >> 21) & 0x1FF;
    pt_idx = (temp_virt >> 12) & 0x1FF;

    if (pdpt_idx != 3) {
        temp_lock_release(flags);
        return;
    }

    if (!pae_temp_pt_ready) {
        pae_init_temp_mapping();
    }
    if (!pae_temp_pt_ready || pd_idx != pae_temp_pd_idx) {
        temp_lock_release(flags);
        return;
    }

    pt = pae_temp_pt;
    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    temp_lock_release(flags);
}

void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr) {
    uint32_t orig_cr3;
    uint32_t flags;

    if (pae_enabled) {
        temp_map_raw_pae(temp_virt, (uint64_t)phys_addr);
        return;
    }

    temp_lock_acquire(&flags);

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    uint32_t kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    uint32_t pd_idx = temp_virt >> 22;
    uint32_t pt_idx = (temp_virt >> 12) & 0x3FF;
    uint32_t pt_addr = 0xFFC00000 + (pd_idx << 12);
    uint32_t *pt = (uint32_t *)pt_addr;
    pt[pt_idx] = (phys_addr & ~0xFFF) | 3;
    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    temp_lock_release(flags);
}

void temp_unmap_raw(uint32_t temp_virt) {
    uint32_t orig_cr3;
    uint32_t flags;

    if (pae_enabled) {
        temp_unmap_raw_pae(temp_virt);
        return;
    }

    temp_lock_acquire(&flags);

    __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
    uint32_t kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    __asm__ volatile("invlpg (%0)" : : "r"(temp_virt) : "memory");

    if (kernel_cr3 && orig_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
    }

    temp_lock_release(flags);
}

void vmm_temp_map_raw(uint32_t temp_virt, uint32_t phys_addr) {
    temp_map_raw(temp_virt, phys_addr);
}

void vmm_temp_map_raw64(uint32_t temp_virt, uint64_t phys_addr) {
    if (pae_enabled) {
        temp_map_raw_pae(temp_virt, phys_addr);
    } else {
        temp_map_raw(temp_virt, (uint32_t)phys_addr);
    }
}

void vmm_temp_unmap_raw(uint32_t temp_virt) {
    temp_unmap_raw(temp_virt);
}

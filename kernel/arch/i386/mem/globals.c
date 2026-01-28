#include <kernel/mem_map.h>
#include <stdint.h>

mem_region_t memory_map[MAX_REGIONS];
uint32_t num_regions = 0;
uint64_t bump_current = 0;
uint32_t active_region = 0;
uint64_t low_bump = 0;
uint32_t kernel_reserved_frames = 0;
uint32_t total_pages_managed = TOTAL_PAGES_32BIT;
static uint32_t kernel_pd_phys = 0;
static uint32_t kernel_pdpt_phys = 0;

void vmm_register_kernel_cr3(uint32_t pd_phys) {
    extern uint32_t pae_enabled;
    if (pae_enabled) {
        kernel_pdpt_phys = pd_phys & ~0x1Fu;
    } else {
        kernel_pd_phys = pd_phys & ~0xFFFu;
    }
}

uint32_t vmm_get_kernel_cr3(void) {
    extern uint32_t pae_enabled;
    if (pae_enabled) {
        return kernel_pdpt_phys;
    }
    return kernel_pd_phys;
}

void vmm_set_cr3(uint32_t pd_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

uint32_t vmm_get_cr3(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

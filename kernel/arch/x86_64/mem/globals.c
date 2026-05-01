#include <lebirun/mem_map.h>
#include <stdint.h>

mem_region_t memory_map[MAX_REGIONS];
uint64_t num_regions = 0;
uint64_t bump_current = 0;
uint64_t active_region = 0;
uint64_t low_bump = 0;
uint64_t kernel_reserved_frames = 0;
uint64_t total_pages_managed = TOTAL_PAGES;
static uint64_t kernel_pml4_phys = 0;
uint64_t kernel_irq_cr3 = 0;

void vmm_register_kernel_cr3(uint64_t pml4_phys) {
    kernel_pml4_phys = pml4_phys & ~0xFFFUL;
    kernel_irq_cr3 = 0;
}

uint64_t vmm_get_kernel_cr3(void) {
    return kernel_pml4_phys;
}

void vmm_set_cr3(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

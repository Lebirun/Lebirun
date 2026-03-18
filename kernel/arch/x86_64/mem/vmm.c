#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/debug.h>
#include <string.h>

extern void pmm_zero_page_phys(uint64_t phys_addr);
extern void vmm_map_page_pae(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
extern void vmm_map_page_early_pae(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
extern void vmm_map_range_alloc_pae(uint64_t virt_addr, uint64_t size, uint64_t flags);
extern int heap_map_page_pae(uint64_t virt_addr);
extern void vmm_unmap_page_pae(uint64_t virt_addr);

void vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    vmm_map_page_pae(virt_addr, phys_addr, flags);
}

void vmm_map_page_early_avail(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    vmm_map_page_early_pae(virt_addr, phys_addr, flags);
}

void vmm_map_range_alloc(uint64_t virt_addr, uint64_t size, uint64_t flags) {
    vmm_map_range_alloc_pae(virt_addr, size, flags);
}

int heap_map_page(uint64_t virt_addr) {
    return heap_map_page_pae(virt_addr);
}

void vmm_unmap_page(uint64_t virt_addr) {
    vmm_unmap_page_pae(virt_addr);
}

void dump_map_debug(void) {
}

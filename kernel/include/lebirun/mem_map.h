#ifndef MEM_MAP_H
#define MEM_MAP_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 0x1000UL

#define KERNEL_VMA 0xFFFFFFFF80000000ULL

#define TEMP_MAP_BASE          (KERNEL_VMA + 0x37000000ULL)
#define TEMP_MAP_PAGES_PER_CPU 8
int smp_processor_id(void);
#define TEMP_SLOT(n) (TEMP_MAP_BASE + ((uint64_t)smp_processor_id() * TEMP_MAP_PAGES_PER_CPU + (n)) * PAGE_SIZE)

#define MAX_PHYSICAL_MEMORY 0x10000000000ULL
#define TOTAL_PAGES (MAX_PHYSICAL_MEMORY / PAGE_SIZE)
#define BITMAP_BYTES_MAX (TOTAL_PAGES / 8)

extern uint64_t total_pages_managed;

#define HEAP_START       0xFFFFFFFFC0000000ULL
#define HEAP_INITIAL_SIZE 0x10000
#define HEAP_MAX_SIZE_DEFAULT 0x01000000
#define HEAP_MAX_SIZE_CAP 0x10000000
#define HEAP_MAGIC 0xDEADBEEF
#define HEAP_MIN_BLOCK 16

#define HEAP_CANARY_HEAD 0xCAFEBABE
#define HEAP_CANARY_TAIL 0xDEADC0DE
#define HEAP_POISON_FREED 0xFE
#define HEAP_POISON_ALLOC 0xCD
#define HEAP_POISON_REDZONE 0xFD

#define KMALLOC_ZERO      0x01
#define KMALLOC_NO_POISON 0x02
#define KMALLOC_SECURE    0x04

typedef struct {
    uint64_t base;
    uint64_t length;
    uint64_t type;
} mem_region_t;

typedef struct heap_block {
    uint64_t magic;
    uint64_t size;
    uint64_t alloc_size;
    uint64_t flags;
    uint8_t is_free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

typedef struct {
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t max_addr;
    heap_block_t *free_list;
    uint64_t total_size;
    uint64_t used_size;
} heap_t;

#define MAX_REGIONS 32

extern mem_region_t memory_map[MAX_REGIONS];
extern uint64_t num_regions;
extern uint8_t *pfa_bitmap;
extern heap_t kernel_heap;

#define MAX_RESERVED_REGIONS 8
typedef struct {
    uint64_t start_phys;
    uint64_t end_phys;
} reserved_region_t;
extern reserved_region_t reserved_regions[MAX_RESERVED_REGIONS];
extern uint64_t num_reserved_regions;

void vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void vmm_map_page_early_avail(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void vmm_map_range_alloc(uint64_t virt_addr, uint64_t size, uint64_t flags);

uint64_t vmm_create_pml4(void);
uint64_t vmm_clone_pml4(uint64_t src_pml4_phys, uint64_t **out_user_pages, uint64_t *out_user_pages_count);
void vmm_free_pml4(uint64_t pml4_phys);
void vmm_set_cr3(uint64_t pml4_phys);
uint64_t vmm_get_cr3(void);

void vmm_register_kernel_cr3(uint64_t pml4_phys);
uint64_t vmm_get_kernel_cr3(void);

void init_mem_map(uint64_t mb_magic, uint64_t mb_ptr);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint64_t num);
void *pmm_alloc_low_page(void);
void *pmm_alloc_early_pages(uint64_t num);
void pfa_init(void);
uint64_t pfa_alloc(void);
uint64_t pfa_alloc_contiguous(uint64_t num_frames);
void pfa_free(uint64_t phys_addr);
void pfa_reclaim_kernel_range(uint64_t phys_start, uint64_t phys_end);
void pfa_free_contiguous(uint64_t phys_addr, uint64_t num_frames);
uint64_t pfa_count_free(void);
void pfa_sync_free_count(void);
uint64_t pfa_get_total_ram_kb(void);

void pfa_ref_init(void);
void pfa_ref_inc(uint64_t phys_addr);
int pfa_ref_dec(uint64_t phys_addr);
uint8_t pfa_ref_get(uint64_t phys_addr);
void pfa_cow_release64(uint64_t phys_addr);

int cow_handle_fault(uint64_t fault_addr, uint64_t pml4_phys);

uint64_t pfa_get_total_ram_kb(void);
uint64_t pfa_get_usable_ram_kb(void);
uint64_t pfa_get_kernel_used_kb(void);
uint64_t pfa_get_kernel_binary_kb(void);
uint64_t pfa_get_bitmap_kb(void);
void pfa_set_reserved_stats(uint64_t kern_bin_kb, uint64_t bmp_kb);
void heap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, uint64_t alignment);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void heap_dump(void);
uint64_t heap_free_space(void);
int is_early_heap_ptr(void *ptr);

void *ksafe_alloc(size_t size, uint64_t flags);
void *kcalloc(size_t nmemb, size_t size);
void ksafe_free(void *ptr);
void kfree_secure(void *ptr);

int heap_check_canaries(void *ptr);
int heap_validate_ptr(void *ptr);

#define SAFE_MUL_SIZE(a, b, result) \
    (((b) != 0 && (a) > SIZE_MAX / (b)) ? 0 : (*(result) = (a) * (b), 1))

#define SAFE_ADD_SIZE(a, b, result) \
    (((a) > SIZE_MAX - (b)) ? 0 : (*(result) = (a) + (b), 1))

uint64_t heap_block_size_for_ptr(void *ptr);
void heap_verify(void);
void vmm_debug_page(uint64_t virt_addr);

void slab_init(void);
void *slab_alloc(size_t size);
void slab_free(void *ptr);
void slab_gc(void);
int slab_owns(void *ptr);
size_t slab_max_size(void);
size_t slab_alloc_size(void *ptr);
void slab_stats(void);
uint64_t slab_get_total_pages(void);

int demand_page_fault_handler(uint64_t fault_addr, uint64_t err_code);
void demand_paging_init(void);
uint64_t demand_get_committed_pages(void);
uint64_t demand_get_reserved_pages(void);
int demand_reserve_range(uint64_t virt_start, uint64_t size);
int demand_is_reserved(uint64_t virt_addr);
int demand_commit_page(uint64_t virt_addr);
int demand_decommit_page(uint64_t virt_addr);

void vmm_unmap_page(uint64_t virt_addr);
void vmm_map_temp(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void vmm_unmap_temp(uint64_t virt_addr);
int pt_ensure_phys_mapped(uint64_t phys_addr);

void vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
uint64_t vmm_get_phys_in_pml4(uint64_t pml4_phys, uint64_t virt_addr);
uint64_t vmm_unmap_page_in_pml4(uint64_t pml4_phys, uint64_t virt_addr);
uint64_t *vmm_map_range_in_pml4_tracked(uint64_t pml4_phys, uint64_t virt_addr, uint64_t size, uint64_t flags, uint64_t *out_count);
void vmm_map_range_in_pml4(uint64_t pml4_phys, uint64_t virt_addr, uint64_t size, uint64_t flags);
void vmm_copy_to_pml4(uint64_t pml4_phys, uint64_t dest_virt, const void *src, uint64_t size);
void vmm_read_from_pml4(uint64_t pml4_phys, uint64_t src_virt, void *dest, uint64_t size);

void vmm_temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
void vmm_temp_unmap_raw(uint64_t temp_virt);

void pmm_zero_page_phys(uint64_t phys_addr);

void dump_map_debug(void);
void dump_pml4_for_virt(uint64_t virt_addr);
void vmm_dump_for_pml4(uint64_t pml4_phys, uint64_t virt_addr);

#endif

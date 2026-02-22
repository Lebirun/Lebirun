#ifndef MEM_MAP_H
#define MEM_MAP_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 0x1000UL

#define MAX_PHYSICAL_MEMORY_32BIT 0x100000000ULL
#define MAX_PHYSICAL_MEMORY_PAE   0x1000000000ULL

extern uint32_t pae_enabled;

#define MAX_PHYSICAL_MEMORY (pae_enabled ? MAX_PHYSICAL_MEMORY_PAE : MAX_PHYSICAL_MEMORY_32BIT)
#define TOTAL_PAGES_32BIT (MAX_PHYSICAL_MEMORY_32BIT / PAGE_SIZE)
#define TOTAL_PAGES_PAE   (MAX_PHYSICAL_MEMORY_PAE / PAGE_SIZE)
#define TOTAL_PAGES (pae_enabled ? TOTAL_PAGES_PAE : TOTAL_PAGES_32BIT)
#define BITMAP_BYTES_32BIT (TOTAL_PAGES_32BIT / 8)
#define BITMAP_BYTES_PAE   (TOTAL_PAGES_PAE / 8)
#define BITMAP_BYTES_MAX (BITMAP_BYTES_32BIT > BITMAP_BYTES_PAE ? BITMAP_BYTES_32BIT : BITMAP_BYTES_PAE)

extern uint32_t total_pages_managed;

#define HEAP_START 0xD0000000
#define HEAP_INITIAL_SIZE 0x10000
#define HEAP_MAX_SIZE 0x01000000
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
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t color_info[6];
} __attribute__((packed)) multiboot_t;

typedef struct {
    uint32_t size;
    uint32_t base_addr_low;
    uint32_t base_addr_high;
    uint32_t length_low;
    uint32_t length_high;
    uint32_t type;
} __attribute__((packed)) multiboot_memory_map_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mem_region_t;

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;          
    uint32_t alloc_size;     
    uint32_t flags;         
    uint8_t is_free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

typedef struct {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t max_addr;
    heap_block_t *free_list;
    uint32_t total_size;
    uint32_t used_size;
} heap_t;

#define MAX_REGIONS 32

extern mem_region_t memory_map[MAX_REGIONS];
extern uint32_t num_regions;
extern uint8_t *pfa_bitmap;
extern heap_t kernel_heap;

#define MAX_RESERVED_REGIONS 8
typedef struct {
    uint32_t start_phys;
    uint32_t end_phys;
} reserved_region_t;
extern reserved_region_t reserved_regions[MAX_RESERVED_REGIONS];
extern uint32_t num_reserved_regions;

extern multiboot_t *g_multiboot;

void vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
void vmm_map_page_early_avail(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
void vmm_map_range_alloc(uint32_t virt_addr, uint32_t size, uint32_t flags);

uint32_t vmm_create_page_directory(void);
uint32_t vmm_clone_page_directory(uint32_t src_pd_phys, uint32_t **out_user_pages, uint32_t *out_user_pages_count);
void vmm_free_page_directory(uint32_t pd_phys);
void vmm_set_cr3(uint32_t pd_phys);
uint32_t vmm_get_cr3(void);

void vmm_register_kernel_cr3(uint32_t pd_phys);
uint32_t vmm_get_kernel_cr3(void);

void init_mem_map(uint32_t mb_magic, uint32_t mb_ptr);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint32_t num);
void *pmm_alloc_low_page(void);
void pfa_init(void);
uint32_t pfa_alloc(void);
uint64_t pfa_alloc64(void);
uint32_t pfa_alloc_contiguous(uint32_t num_frames);
void pfa_free(uint32_t phys_addr);
void pfa_free64(uint64_t phys_addr);
void pfa_reclaim_kernel_range(uint32_t phys_start, uint32_t phys_end);
void pfa_free_contiguous(uint32_t phys_addr, uint32_t num_frames);
uint32_t pfa_count_free(void);
void pfa_sync_free_count(void);
uint32_t pfa_get_total_ram_kb(void);

void pfa_ref_init(void);
void pfa_ref_inc(uint32_t phys_addr);
int pfa_ref_dec(uint32_t phys_addr);
uint8_t pfa_ref_get(uint32_t phys_addr);

int cow_handle_fault(uint32_t fault_addr, uint32_t pd_phys);

uint32_t pfa_get_total_ram_kb(void);
uint32_t pfa_get_usable_ram_kb(void);
uint32_t pfa_get_kernel_used_kb(void);
uint32_t pfa_get_kernel_binary_kb(void);
uint32_t pfa_get_bitmap_kb(void);
void pfa_set_reserved_stats(uint32_t kern_bin_kb, uint32_t bmp_kb);
void heap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, uint32_t alignment);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void heap_dump(void);
uint32_t heap_free_space(void);
int is_early_heap_ptr(void *ptr);

void *ksafe_alloc(size_t size, uint32_t flags);
void *kcalloc(size_t nmemb, size_t size);      
void ksafe_free(void *ptr);                   
void kfree_secure(void *ptr);                

int heap_check_canaries(void *ptr);              
int heap_validate_ptr(void *ptr);                

#define SAFE_MUL_SIZE(a, b, result) \
    (((b) != 0 && (a) > SIZE_MAX / (b)) ? 0 : (*(result) = (a) * (b), 1))

#define SAFE_ADD_SIZE(a, b, result) \
    (((a) > SIZE_MAX - (b)) ? 0 : (*(result) = (a) + (b), 1))

uint32_t heap_block_size_for_ptr(void *ptr);
void heap_verify(void);
void vmm_debug_page(uint32_t virt_addr);

void slab_init(void);
void *slab_alloc(size_t size);
void slab_free(void *ptr);
void slab_gc(void);
int slab_owns(void *ptr);
size_t slab_max_size(void);
void slab_stats(void);
uint32_t slab_get_total_pages(void);

int demand_page_fault_handler(uint32_t fault_addr, uint32_t err_code);
void demand_paging_init(void);
uint32_t demand_get_committed_pages(void);
uint32_t demand_get_reserved_pages(void);
int demand_reserve_range(uint32_t virt_start, uint32_t size);
int demand_is_reserved(uint32_t virt_addr);
int demand_commit_page(uint32_t virt_addr);
int demand_decommit_page(uint32_t virt_addr);

void vmm_unmap_page(uint32_t virt_addr);
void vmm_map_temp(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
void vmm_unmap_temp(uint32_t virt_addr);

void vmm_map_page_in_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
void vmm_map_page_in_pd64(uint32_t pd_phys, uint32_t virt_addr, uint64_t phys_addr, uint32_t flags);
uint32_t vmm_get_phys_in_pd(uint32_t pd_phys, uint32_t virt_addr);
uint32_t vmm_unmap_page_in_pd(uint32_t pd_phys, uint32_t virt_addr);
uint32_t *vmm_map_range_in_pd_tracked(uint32_t pd_phys, uint32_t virt_addr, uint32_t size, uint32_t flags, uint32_t *out_count);
void vmm_map_range_in_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t size, uint32_t flags);
void vmm_copy_to_pd(uint32_t pd_phys, uint32_t dest_virt, const void *src, uint32_t size);
void vmm_read_from_pd(uint32_t pd_phys, uint32_t src_virt, void *dest, uint32_t size);

void vmm_temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
void vmm_temp_map_raw64(uint32_t temp_virt, uint64_t phys_addr);
void vmm_temp_unmap_raw(uint32_t temp_virt);

void pmm_zero_page_phys(uint32_t phys_addr);

void dump_map_debug(void);
void dump_pd_pt_for_virt(uint32_t virt_addr);
void vmm_dump_for_pd(uint32_t pd_phys, uint32_t virt_addr);

#endif
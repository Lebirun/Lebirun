#ifndef MEM_MAP_H
#define MEM_MAP_H

#define PAGE_SIZE 0x1000UL
#define MAX_PHYSICAL_MEMORY 0x100000000ULL
#define TOTAL_PAGES (MAX_PHYSICAL_MEMORY / PAGE_SIZE)
#define BITMAP_BYTES (TOTAL_PAGES / 8)

#define HEAP_START 0xD0000000
#define HEAP_INITIAL_SIZE 0x100000
#define HEAP_MAX_SIZE 0x10000000
#define HEAP_MAGIC 0xDEADBEEF
#define HEAP_MIN_BLOCK 16

#include <stdint.h>
#include <stddef.h>

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
extern uint8_t pfa_bitmap[BITMAP_BYTES];
extern heap_t kernel_heap;

void vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
void vmm_map_range_alloc(uint32_t virt_addr, uint32_t size, uint32_t flags);

void init_mem_map(uint32_t mb_magic, uint32_t mb_ptr);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint32_t num);
void *pmm_alloc_low_page(void);
void pfa_init(void);
uint32_t pfa_alloc(void);
uint32_t pfa_alloc_contiguous(uint32_t num_frames);
void pfa_free(uint32_t phys_addr);
void pfa_free_contiguous(uint32_t phys_addr, uint32_t num_frames);
uint32_t pfa_count_free(void);
void heap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, uint32_t alignment);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void heap_dump(void);
uint32_t heap_free_space(void);

uint32_t heap_block_size_for_ptr(void *ptr);
void heap_verify(void);
void vmm_debug_page(uint32_t virt_addr);

#endif
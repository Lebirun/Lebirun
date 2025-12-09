#ifndef MEM_MAP_H
#define MEM_MAP_H

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

#define MAX_REGIONS 32

extern mem_region_t memory_map[MAX_REGIONS];
extern uint32_t num_regions;

void init_mem_map(uint32_t mb_magic, uint32_t mb_ptr);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(uint32_t num);
void *pmm_alloc_low_page(void);
void *malloc_virt(size_t size);
void free_virt(void *ptr);

#endif
#include <kernel/mem_map.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/elf.h>
#include "launch_user.h"
#include <string.h>

#define USER_STACK_TOP 0x00800000u
#define USER_STACK_SIZE 0x4000

task_t* launch_user_binary(const uint8_t *bin_start, const uint8_t *bin_end) {
    if (!bin_start || !bin_end || bin_end <= bin_start) return NULL;
    uint32_t size = (uint32_t)(bin_end - bin_start);

    int elf_valid = elf_validate(bin_start, size);
    if (elf_valid != 0) {
        printf("launch_user_binary: ELF validation failed (%d)\n", elf_valid);
        return NULL;
    }

    uint32_t new_pd = vmm_create_page_directory();
    if (!new_pd) {
        printf("launch_user_binary: Failed to create page directory\n");
        return NULL;
    }

    DPRINTF3("launch_user: created PD=0x%08X for ELF binary\n", new_pd);
    if (debugMode && debugLevel >= 3) heap_verify();

    elf_info_t elf_info;
    uint32_t *elf_pages = NULL;
    uint32_t elf_page_count = 0;

    int load_result = elf_load_to_pd(new_pd, bin_start, size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        printf("launch_user_binary: ELF loading failed (%d)\n", load_result);
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        return NULL;
    }

    DPRINTF3("launch_user: ELF loaded entry=0x%08X base=0x%08X end=0x%08X\n",
             elf_info.entry_point, elf_info.load_base, elf_info.bss_end);

    uint32_t stack_page_count = 0;
    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(new_pd, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, 0x7, &stack_page_count);
    DPRINTF3("launch_user: mapped stack at 0x%08X (%u pages)\n", USER_STACK_TOP - USER_STACK_SIZE, stack_page_count);
    if (debugMode && debugLevel >= 3) heap_verify();

    task_t* t = create_task_with_cr3((void*)elf_info.entry_point, TASK_READY, true, new_pd);
    if (!t) {
        printf("launch_user_binary: create_task failed\n");
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        if (stack_pages) {
            for (uint32_t i = 0; i < stack_page_count; i++) pfa_free(stack_pages[i]);
            kfree(stack_pages);
        }
        vmm_free_page_directory(new_pd);
        return NULL;
    }

    t->pd_phys = new_pd;
    t->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;

    uint32_t total_pages = elf_page_count + stack_page_count;
    t->user_pages = (uint32_t *)kmalloc(total_pages * sizeof(uint32_t));
    if (t->user_pages) {
        if (elf_pages) {
            memcpy(t->user_pages, elf_pages, elf_page_count * sizeof(uint32_t));
        }
        if (stack_pages) {
            memcpy(t->user_pages + elf_page_count, stack_pages, stack_page_count * sizeof(uint32_t));
        }
        t->user_pages_count = total_pages;
    } else {
        t->user_pages_count = 0;
    }

    if (elf_pages) kfree(elf_pages);
    if (stack_pages) kfree(stack_pages);

    return t;
} 

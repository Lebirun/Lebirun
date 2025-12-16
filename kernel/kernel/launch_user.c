#include <kernel/mem_map.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include "launch_user.h"
#include <string.h>

#define DEFAULT_USER_CODE_ADDR 0x00400000
#define USER_STACK_TOP 0x00800000u
#define USER_STACK_SIZE 0x4000

task_t* launch_user_binary(const uint8_t *bin_start, const uint8_t *bin_end) {
    if (!bin_start || !bin_end || bin_end <= bin_start) return NULL;
    uint32_t size = (uint32_t)(bin_end - bin_start);
    uint32_t code_addr = DEFAULT_USER_CODE_ADDR;

    uint32_t new_pd = vmm_create_page_directory();
    if (!new_pd) {
        printf("launch_user_binary: Failed to create page directory\n");
        return NULL;
    }

    DPRINTF3("launch_user: created PD=0x%08X, mapping without CR3 switch\n", new_pd);
    if (debugMode && debugLevel >= 3) heap_verify();

    uint32_t code_page_count = 0;
    uint32_t stack_page_count = 0;
    
    uint32_t *code_pages = vmm_map_range_in_pd_tracked(new_pd, code_addr, size, 0x7, &code_page_count);
    DPRINTF3("launch_user: mapped code region at 0x%08X size=%u (%u pages)\n", code_addr, size, code_page_count);
    if (debugMode && debugLevel >= 3) heap_verify();

    vmm_copy_to_pd(new_pd, code_addr, bin_start, size);
    DPRINTF3("launch_user: copied %u bytes to 0x%08X\n", size, code_addr);
    if (debugMode && debugLevel >= 3) heap_verify();

    DPRINTF5("launch_user: verifying mapping for code at 0x%08X in PD=0x%08X\n", code_addr, new_pd);
    vmm_dump_for_pd(new_pd, code_addr);

    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(new_pd, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, 0x7, &stack_page_count);
    DPRINTF3("launch_user: mapped stack at 0x%08X (%u pages)\n", USER_STACK_TOP - USER_STACK_SIZE, stack_page_count);
    if (debugMode && debugLevel >= 3) heap_verify();
    DPRINTF5("launch_user: verifying mapping for stack at 0x%08X in PD=0x%08X\n", USER_STACK_TOP - USER_STACK_SIZE, new_pd);
    vmm_dump_for_pd(new_pd, USER_STACK_TOP - USER_STACK_SIZE);

    task_t* t = create_task_with_cr3((void*)code_addr, TASK_READY, true, new_pd);
    if (!t) {
        printf("launch_user_binary: create_task failed\n");
        if (code_pages) {
            for (uint32_t i = 0; i < code_page_count; i++) pfa_free(code_pages[i]);
            kfree(code_pages);
        }
        if (stack_pages) {
            for (uint32_t i = 0; i < stack_page_count; i++) pfa_free(stack_pages[i]);
            kfree(stack_pages);
        }
        vmm_free_page_directory(new_pd);
        return NULL;
    }
    
    t->pd_phys = new_pd;
    t->user_brk = (code_addr + size + 0xFFF) & ~0xFFFu;
    
    uint32_t total_pages = code_page_count + stack_page_count;
    t->user_pages = (uint32_t *)kmalloc(total_pages * sizeof(uint32_t));
    if (t->user_pages) {
        if (code_pages) {
            memcpy(t->user_pages, code_pages, code_page_count * sizeof(uint32_t));
        }
        if (stack_pages) {
            memcpy(t->user_pages + code_page_count, stack_pages, stack_page_count * sizeof(uint32_t));
        }
        t->user_pages_count = total_pages;
    } else {
        t->user_pages_count = 0;
    }
    
    if (code_pages) kfree(code_pages);
    if (stack_pages) kfree(stack_pages);
    
    return t;
}

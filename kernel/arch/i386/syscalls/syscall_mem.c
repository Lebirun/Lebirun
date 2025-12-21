#include "syscall_defs.h"

static int sys_sbrk(int inc, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (inc == 0) {
        return (int)current_task->user_brk;
    }
    if (!current_task) return -1;
    if ((int)inc < 0) return -1;
    
    uint32_t old = current_task->user_brk;
    uint32_t newbrk = (old + (uint32_t)inc + 0xFFF) & ~0xFFFu;
    if (newbrk >= 0x007F0000 || newbrk < old) return -1;
    
    uint32_t start_page = (old + 0xFFF) & ~0xFFFu;
    uint32_t num_new_pages = (newbrk - start_page) / 0x1000;
    
    if (num_new_pages > 0) {
        uint32_t page_count = 0;
        uint32_t *new_pages = vmm_map_range_in_pd_tracked(
            current_task->pd_phys, start_page, newbrk - start_page, 0x7, &page_count);
        
        if (!new_pages && num_new_pages > 0) {
            return -1;
        }
        
        if (new_pages && page_count > 0) {
            uint32_t old_count = current_task->user_pages_count;
            uint32_t new_count = old_count + page_count;
            uint32_t *expanded = (uint32_t *)kmalloc(new_count * sizeof(uint32_t));
            if (expanded) {
                if (current_task->user_pages && old_count > 0) {
                    memcpy(expanded, current_task->user_pages, old_count * sizeof(uint32_t));
                    kfree(current_task->user_pages);
                }
                memcpy(expanded + old_count, new_pages, page_count * sizeof(uint32_t));
                current_task->user_pages = expanded;
                current_task->user_pages_count = new_count;
            }
            kfree(new_pages);
        }
    }
    
    current_task->user_brk = newbrk;
    return (int)old;
}

static int sys_mmap(int len, const char *unused, int prot) {
    (void)unused; (void)prot;
    if (len <= 0 || !current_task) return -1;
    
    uint32_t base = 0x00600000;
    uint32_t size = (len + 0xFFF) & ~0xFFFu;
    
    uint32_t page_count = 0;
    uint32_t *new_pages = vmm_map_range_in_pd_tracked(
        current_task->pd_phys, base, size, 0x7, &page_count);
    
    if (!new_pages && size > 0) {
        return -1;
    }
    
    if (new_pages && page_count > 0) {
        uint32_t old_count = current_task->user_pages_count;
        uint32_t new_count = old_count + page_count;
        uint32_t *expanded = (uint32_t *)kmalloc(new_count * sizeof(uint32_t));
        if (expanded) {
            if (current_task->user_pages && old_count > 0) {
                memcpy(expanded, current_task->user_pages, old_count * sizeof(uint32_t));
                kfree(current_task->user_pages);
            }
            memcpy(expanded + old_count, new_pages, page_count * sizeof(uint32_t));
            current_task->user_pages = expanded;
            current_task->user_pages_count = new_count;
        }
        kfree(new_pages);
    }
    
    return (int)base;
}

void syscalls_mem_init(void) {
    syscall_table[SYSCALL_SBRK] = sys_sbrk;
    syscall_table[SYSCALL_MMAP] = sys_mmap;
}

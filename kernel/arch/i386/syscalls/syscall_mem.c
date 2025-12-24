#include "syscall_defs.h"

static int sys_brk(int addr, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    
    if (!current_task) {
        DPRINTF1("brk: no current_task!\n");
        return -1;
    }
    
    uint32_t requested = (uint32_t)addr;
    uint32_t current_brk = current_task->user_brk;
    
    DPRINTF3("brk: req=0x%08X cur=0x%08X\n", requested, current_brk);
    
    if (requested == 0) {
        return (int)current_brk;
    }
    
    if (requested < current_brk) {
        return (int)current_brk;
    }
    
    uint32_t newbrk = (requested + 0xFFF) & ~0xFFFu;
    
    if (newbrk >= 0x40000000) {
        DPRINTF1("brk: requested 0x%08X exceeds limit\n", newbrk);
        return (int)current_brk;
    }
    
    uint32_t old_page_end = (current_brk + 0xFFF) & ~0xFFFu;
    if (newbrk > old_page_end) {
        uint32_t page_count = 0;
        uint32_t *new_pages = vmm_map_range_in_pd_tracked(
            current_task->pd_phys, old_page_end, newbrk - old_page_end, 0x7, &page_count);
        
        if (!new_pages && (newbrk > old_page_end)) {
            DPRINTF1("brk: mapping %u bytes failed (free=%u)\n", newbrk - old_page_end, pfa_count_free());
            return (int)current_brk;
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
    return (int)newbrk;
}

static int sys_sbrk(int inc, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (inc == 0) {
        return (int)current_task->user_brk;
    }
    if (!current_task) {
        DPRINTF1("sys_sbrk: no current_task\n");
        return -1;
    }
    if ((int)inc < 0) {
        DPRINTF1("sys_sbrk: negative increment %d\n", inc);
        if (console_is_initialized()) {
            int cur_con = console_get_current();
            char buf[128];
            int n = snprintf(buf, sizeof(buf), "sbrk: negative increment %d not allowed\n", inc);
            console_write_to(cur_con, buf, (size_t)n);
        }
        return -1;
    }
    
    uint32_t old = current_task->user_brk;
    uint32_t newbrk = (old + (uint32_t)inc + 0xFFF) & ~0xFFFu;
    DPRINTF3("sys_sbrk: inc=%d old=0x%08X newbrk=0x%08X\n", inc, old, newbrk);
    if (console_is_initialized()) {
        int cur_con = console_get_current();
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "sbrk: inc=%u old=0x%08X new=0x%08X free=%u\n", (uint32_t)inc, old, newbrk, pfa_count_free());
        console_write_to(cur_con, buf, (size_t)n);
    }
    if (newbrk >= 0x40000000 || newbrk < old) {
        DPRINTF1("sys_sbrk: newbrk 0x%08X out of range (limit 0x40000000, old=0x%08X)\n", newbrk, old);
        if (console_is_initialized()) {
            int cur_con = console_get_current();
            char buf[128];
            int n = snprintf(buf, sizeof(buf), "sbrk: requested brk 0x%08X out of range (limit 0x40000000)\n", newbrk);
            console_write_to(cur_con, buf, (size_t)n);
        }
        return -1;
    }
    
    uint32_t old_page_end = (old + 0xFFF) & ~0xFFFu;
    if (newbrk > old_page_end) {
        uint32_t page_count = 0;
        DPRINTF3("sys_sbrk: mapping 0x%08X - 0x%08X in pd=0x%08X\n", old_page_end, newbrk, current_task->pd_phys);
        uint32_t *new_pages = vmm_map_range_in_pd_tracked(
            current_task->pd_phys, old_page_end, newbrk - old_page_end, 0x7, &page_count);
        
        if (!new_pages && (newbrk > old_page_end)) {
            DPRINTF1("sys_sbrk: vmm_map_range_in_pd_tracked failed for %u bytes (virt=0x%08X size=0x%X)\n", 
                     newbrk - old_page_end, old_page_end, newbrk - old_page_end);
            if (console_is_initialized()) {
                int cur_con = console_get_current();
                char buf[128];
                uint32_t freep = pfa_count_free();
                int n = snprintf(buf, sizeof(buf), "sbrk: mapping %u bytes at 0x%08X failed (free pages=%u)\n", newbrk - old_page_end, old_page_end, freep);
                console_write_to(cur_con, buf, (size_t)n);
            }
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
    syscall_table[SYSCALL_SBRK] = sys_brk;
    syscall_table[SYSCALL_MMAP] = sys_mmap;
}

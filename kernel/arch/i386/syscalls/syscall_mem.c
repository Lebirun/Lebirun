#include "syscall_defs.h"
#include <stdint.h>
#include <stddef.h>

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

static uint32_t mmap_next_addr = 0x10000000;

static int sys_mmap2(void *addr, size_t length, int prot, int flags, int fd, int64_t pgoffset) {
    (void)prot; (void)flags; (void)fd; (void)pgoffset;
    
    if (length == 0 || !current_task) return -EINVAL;
    
    uint32_t size = (length + 0xFFF) & ~0xFFFu;
    uint32_t base;
    
    if (addr != NULL && ((uint32_t)addr & 0xFFF) == 0) {
        base = (uint32_t)addr;
    } else {
        base = mmap_next_addr;
        mmap_next_addr += size;
        if (mmap_next_addr >= 0x40000000) {
            mmap_next_addr = 0x10000000;
        }
    }
    
    uint32_t page_count = 0;
    uint32_t *new_pages = vmm_map_range_in_pd_tracked(
        current_task->pd_phys, base, size, 0x7, &page_count);
    
    if (!new_pages && size > 0) {
        return -ENOMEM;
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

static int sys_munmap(void *addr, size_t length) {
    if (!current_task) return -EINVAL;
    if ((uint32_t)addr & 0xFFF) return -EINVAL;
    if (length == 0) return 0;
    
    return 0;
}

static int sys_mprotect(void *addr, size_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    if (!current_task) return -EINVAL;
    return 0;
}

static void *sys_mremap(void *old_addr, size_t old_size, size_t new_size, int flags, void *new_addr) {
    (void)old_addr; (void)old_size; (void)flags; (void)new_addr;
    
    if (!current_task) return (void *)(long)-EINVAL;
    if (new_size == 0) return (void *)(long)-EINVAL;
    
    uint32_t base = mmap_next_addr;
    uint32_t size = (new_size + 0xFFF) & ~0xFFFu;
    mmap_next_addr += size;
    
    uint32_t page_count = 0;
    uint32_t *new_pages = vmm_map_range_in_pd_tracked(
        current_task->pd_phys, base, size, 0x7, &page_count);
    
    if (!new_pages && size > 0) {
        return (void *)(long)-ENOMEM;
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
    
    if (old_addr && old_size > 0) {
        memcpy((void *)base, old_addr, old_size < new_size ? old_size : new_size);
    }
    
    return (void *)base;
}

static int sys_madvise(void *addr, size_t length, int advice) {
    (void)addr; (void)length; (void)advice;
    return 0;
}

static int sys_mincore(void *addr, size_t length, unsigned char *vec) {
    (void)addr;
    if (!vec) return -EFAULT;
    
    size_t pages = (length + 0xFFF) / 0x1000;
    for (size_t i = 0; i < pages; i++) {
        vec[i] = 1;
    }
    return 0;
}

void syscalls_mem_init(void) {
    syscall_table[SYSCALL_SBRK] = sys_brk;
    syscall_table[SYSCALL_MMAP] = sys_mmap;
    syscall_table[SYSCALL_MMAP2] = sys_mmap2;
    syscall_table[SYSCALL_MUNMAP] = sys_munmap;
    syscall_table[SYSCALL_MPROTECT] = sys_mprotect;
    syscall_table[SYSCALL_MREMAP] = sys_mremap;
    syscall_table[SYSCALL_MADVISE] = sys_madvise;
    syscall_table[SYSCALL_MINCORE] = sys_mincore;
}

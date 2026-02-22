#include "syscall_defs.h"
#include <stdint.h>
#include <stddef.h>

extern void pfa_cow_release(uint32_t phys_addr);

#define USER_MMAP_BASE 0x10000000u
#define USER_MMAP_LIMIT 0x40000000u

static uint32_t align_up_u32(uint32_t v, uint32_t align) {
    if (!align) return v;
    return (v + align - 1u) & ~(align - 1u);
}

static int sys_brk(int addr, const char *unused, int unused2) {
    uint32_t requested;
    uint32_t current_brk;
    uint32_t newbrk;
    uint32_t old_page_end;
    uint32_t page_count;
    uint32_t *new_pages;
    uint32_t old_count;
    uint32_t new_count;
    uint32_t *expanded;

    (void)unused; (void)unused2;
    
    if (!current_task) {
        return -1;
    }
    
    requested = (uint32_t)addr;
    current_brk = current_task->user_brk;
    
    if (requested == 0) {
        return (int)current_brk;
    }
    
    if (requested < current_brk) {
        return (int)current_brk;
    }
    
    newbrk = (requested + 0xFFF) & ~0xFFFu;
    
    if (newbrk >= 0x40000000) {
        return (int)current_brk;
    }
    
    old_page_end = (current_brk + 0xFFF) & ~0xFFFu;
    if (newbrk > old_page_end) {
        page_count = 0;
        new_pages = vmm_map_range_in_pd_tracked(
            current_task->pd_phys, old_page_end, newbrk - old_page_end, 0x7, &page_count);
        
        if (!new_pages && (newbrk > old_page_end)) {
            return (int)current_brk;
        }
        
        if (new_pages && page_count > 0) {
            old_count = current_task->user_pages_count;
            new_count = old_count + page_count;
            expanded = (uint32_t *)kmalloc(new_count * sizeof(uint32_t));
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

static int sys_mmap(int a1, const char *a2, int a3) {
    (void)a2; (void)a3;
    if (!current_task) return -EINVAL;

    uint32_t addr = 0;
    uint32_t length = 0;

    if (current_task->syscall_frame) {
        registers_t *r = current_task->syscall_frame;
        addr = r->ebx;
        length = r->ecx;
    } else {
        addr = 0;
        length = (uint32_t)a1;
    }

    if (length == 0) return -EINVAL;

    uint32_t size = align_up_u32(length, 0x1000u);
    if (size == 0) return -EINVAL;

    if (current_task->mmap_next_addr < USER_MMAP_BASE || current_task->mmap_next_addr >= USER_MMAP_LIMIT) {
        current_task->mmap_next_addr = USER_MMAP_BASE;
    }

    uint32_t base;
    if (addr != 0 && (addr & 0xFFFu) == 0) {
        base = addr;
    } else {
        base = align_up_u32(current_task->mmap_next_addr, 0x1000u);
        if (base < USER_MMAP_BASE) base = USER_MMAP_BASE;
        if (base + size >= USER_MMAP_LIMIT) {
            base = USER_MMAP_BASE;
        }
        current_task->mmap_next_addr = base + size;
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

static int sys_mmap2(void *addr, size_t length, int prot, int flags, int fd, int64_t pgoffset) {
    (void)prot; (void)flags; (void)fd; (void)pgoffset;
    
    if (length == 0 || !current_task) return -EINVAL;
    
    uint32_t size = (length + 0xFFF) & ~0xFFFu;
    uint32_t base;
    
    if (current_task->mmap_next_addr < USER_MMAP_BASE || current_task->mmap_next_addr >= USER_MMAP_LIMIT) {
        current_task->mmap_next_addr = USER_MMAP_BASE;
    }

    if (addr != NULL && ((uint32_t)addr & 0xFFFu) == 0) {
        base = (uint32_t)addr;
    } else {
        base = align_up_u32(current_task->mmap_next_addr, 0x1000u);
        if (base < USER_MMAP_BASE) base = USER_MMAP_BASE;
        if (base + size >= USER_MMAP_LIMIT) {
            base = USER_MMAP_BASE;
        }
        current_task->mmap_next_addr = base + size;
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
    uint32_t base;
    uint32_t size;
    uint32_t page_addr;
    uint32_t end;
    uint32_t phys;
    uint32_t i;
    uint32_t dst;

    if (!current_task) return -EINVAL;
    base = (uint32_t)addr;
    if (base & 0xFFF) return -EINVAL;
    if (length == 0) return 0;
    if (base >= 0xC0000000) return -EINVAL;

    size = (length + 0xFFF) & ~0xFFFu;
    end = base + size;
    if (end < base) return -EINVAL;
    if (end > 0xC0000000) end = 0xC0000000;

    for (page_addr = base; page_addr < end; page_addr += 0x1000) {
        phys = vmm_unmap_page_in_pd(current_task->pd_phys, page_addr);
        if (phys) {
            pfa_cow_release(phys);
            for (i = 0; i < current_task->user_pages_count; i++) {
                if (current_task->user_pages[i] == phys) {
                    current_task->user_pages[i] = 0;
                    break;
                }
            }
        }
    }

    dst = 0;
    for (i = 0; i < current_task->user_pages_count; i++) {
        if (current_task->user_pages[i] != 0) {
            current_task->user_pages[dst++] = current_task->user_pages[i];
        }
    }
    current_task->user_pages_count = dst;

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
    
    uint32_t base = current_task->mmap_next_addr;
    uint32_t size = (new_size + 0xFFF) & ~0xFFFu;
    current_task->mmap_next_addr += size;
    
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

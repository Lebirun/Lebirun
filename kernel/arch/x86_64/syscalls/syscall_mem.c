#include "syscall_defs.h"
#include <stdint.h>
#include <stddef.h>

extern void pfa_cow_release(uint64_t phys_addr);
extern void exec_page_cache_on_page_release(uint64_t phys_addr);

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

static uint64_t mmap_pte_flags(int prot) {
    uint64_t flags;

    flags = VMM_PTE_PRESENT;
    if (prot != PROT_NONE) flags |= VMM_PTE_USER;
    if (prot & PROT_WRITE) flags |= VMM_PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= VMM_PTE_NX;
    return flags;
}

static uint64_t align_up_u64(uint64_t v, uint64_t align) {
    if (!align) return v;
    return (v + align - 1u) & ~(align - 1u);
}

static int user_mmap_range_valid(uint64_t addr, uint64_t end) {
    if (addr >= USER_MMAP_LOW_BASE && end < USER_MMAP_LOW_LIMIT) return 1;
    if (addr >= USER_MMAP_HIGH_BASE && end < USER_MMAP_HIGH_LIMIT) return 1;
    return 0;
}

static uint64_t user_mmap_auto_base(uint64_t next, uint64_t size) {
    uint64_t base;

    if (next >= USER_MMAP_LOW_BASE && next < USER_MMAP_LOW_LIMIT) {
        base = align_up_u64(next, PAGE_SIZE);
        if (base < USER_MMAP_LOW_LIMIT &&
            size <= USER_MMAP_LOW_LIMIT - base) return base;
    }
    if (next < USER_MMAP_HIGH_BASE || next >= USER_MMAP_HIGH_LIMIT) {
        next = USER_MMAP_HIGH_BASE;
    }
    base = align_up_u64(next, PAGE_SIZE);
    if (base >= USER_MMAP_HIGH_LIMIT ||
        size > USER_MMAP_HIGH_LIMIT - base) return 0;
    return base;
}

static int user_range_mapped_mem(uint64_t addr, uint64_t size) {
    uint64_t end;
    uint64_t p;
    uint64_t pend;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size - 1;
    if (end < addr) return 0;
    if (addr < 0x1000 || end >= KERNEL_VMA) return 0;
    p = addr & ~0xFFFu;
    pend = end & ~0xFFFu;
    for (;;) {
        if (vmm_get_phys_in_pml4(current_task->cr3, p) == 0) return 0;
        if (p == pend) break;
        if (p > 0xFFFFFFFFFFFFF000ULL) return 0;
        p += 0x1000u;
    }
    return 1;
}

static int user_range_free_mem(uint64_t addr, uint64_t size, uint64_t allow_base, uint64_t allow_size) {
    uint64_t end;
    uint64_t p;
    uint64_t pend;
    uint64_t allow_end;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size - 1;
    if (end < addr) return 0;
    if (!user_mmap_range_valid(addr, end)) return 0;
    allow_end = allow_base + allow_size;
    p = addr & ~0xFFFu;
    pend = end & ~0xFFFu;
    for (;;) {
        if (vmm_get_phys_in_pml4(current_task->cr3, p) != 0) {
            if (!(allow_size != 0 && p >= allow_base && p < allow_end)) return 0;
        }
        if (p == pend) break;
        if (p > 0xFFFFFFFFFFFFF000ULL) return 0;
        p += 0x1000u;
    }
    return 1;
}

static void compact_user_pages(void) {
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t i;
    uint64_t dst;

    if (!current_task || !current_task->user_pages) return;
    old_count = current_task->user_pages_count;
    dst = 0;
    for (i = 0; i < old_count; i++) {
        if (current_task->user_pages[i] != 0) {
            current_task->user_pages[dst++] = current_task->user_pages[i];
        }
    }
    current_task->user_pages_count = dst;
    if (dst == old_count) return;
    if (dst == 0) {
        kfree(current_task->user_pages);
        current_task->user_pages = NULL;
        return;
    }
    new_pages = (uint64_t *)kmalloc(dst * sizeof(uint64_t));
    if (!new_pages) return;
    memcpy(new_pages, current_task->user_pages, dst * sizeof(uint64_t));
    kfree(current_task->user_pages);
    current_task->user_pages = new_pages;
}

static int remove_user_page_phys(uint64_t phys) {
    uint64_t i;

    if (!current_task || !current_task->user_pages) return 0;
    for (i = 0; i < current_task->user_pages_count; i++) {
        if (current_task->user_pages[i] == phys) {
            current_task->user_pages[i] = 0;
            return 1;
        }
    }
    return 0;
}

static void release_user_leaf_range(uint64_t base, uint64_t end) {
    uint64_t page_addr;
    uint64_t phys;

    for (page_addr = base; page_addr < end; page_addr += 0x1000) {
        phys = vmm_unmap_page_in_pml4(current_task->pml4_phys, page_addr);
        if (phys) {
            if (remove_user_page_phys(phys)) {
                exec_page_cache_on_page_release(phys);
                pfa_cow_release(phys);
            }
        }
    }
    compact_user_pages();
    vmm_prune_user_range(current_task->pml4_phys, base, end - base);
}

static int sys_brk(int addr, const char *unused, int unused2) {
    uint64_t requested;
    uint64_t current_brk;
    uint64_t newbrk;
    uint64_t old_page_end;
    uint64_t shrink_page_end;
    uint64_t page_count;
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t new_count;
    uint64_t *expanded;

    (void)unused; (void)unused2;
    
    if (!current_task) {
        return -1;
    }
    
    requested = (uint64_t)addr;
    current_brk = current_task->user_brk;
    
    if (requested == 0) {
        return (int)current_brk;
    }

    if (requested < current_task->user_brk_start) {
        return (int)current_brk;
    }
    if (!task_data_allows(current_task, requested)) return (int)current_brk;
    
    if (requested < current_brk) {
        old_page_end = (current_brk + 0xFFF) & ~0xFFFu;
        shrink_page_end = (requested + 0xFFF) & ~0xFFFu;
        if (old_page_end > shrink_page_end) {
            release_user_leaf_range(shrink_page_end, old_page_end);
        }
        current_task->user_brk = requested;
        return (int)requested;
    }
    
    newbrk = (requested + 0xFFF) & ~0xFFFu;
    
    if (newbrk >= USER_MMAP_LOW_BASE) {
        return (int)current_brk;
    }
    
    old_page_end = (current_brk + 0xFFF) & ~0xFFFu;
    if (newbrk > old_page_end) {
        if (!task_memory_allows(current_task, newbrk - old_page_end)) {
            return (int)current_brk;
        }
        page_count = 0;
        new_pages = vmm_map_range_in_pml4_tracked(
            current_task->pml4_phys, old_page_end, newbrk - old_page_end,
            0x7 | VMM_PTE_NX, &page_count);
        
        if (!new_pages && (newbrk > old_page_end)) {
            return (int)current_brk;
        }
        
        if (new_pages && page_count > 0) {
            old_count = current_task->user_pages_count;
            new_count = old_count + page_count;
            expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
            if (!expanded) {
                release_user_leaf_range(old_page_end, newbrk);
                kfree(new_pages);
                return (int)current_brk;
            }
            if (current_task->user_pages && old_count > 0) {
                memcpy(expanded, current_task->user_pages,
                       old_count * sizeof(uint64_t));
                kfree(current_task->user_pages);
            }
            memcpy(expanded + old_count, new_pages,
                   page_count * sizeof(uint64_t));
            current_task->user_pages = expanded;
            current_task->user_pages_count = new_count;
            kfree(new_pages);
        }
    }
    
    current_task->user_brk = requested;
    return (int)requested;
}

static int sys_mmap(int a1, const char *a2, int a3) {
    uint64_t length;
    uint64_t size;
    uint64_t base;
    uint64_t page_count;
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t new_count;
    uint64_t *expanded;
    registers_t *r;

    (void)a2; (void)a3;
    if (!current_task) return -EINVAL;

    length = 0;

    if (current_task->syscall_frame) {
        r = current_task->syscall_frame;
        length = r->rcx;
    } else {
        length = (uint64_t)a1;
    }

    if (length == 0) return -EINVAL;

    size = align_up_u64(length, 0x1000u);
    if (size == 0) return -EINVAL;
    if (!task_memory_allows(current_task, size)) return -ENOMEM;

    base = user_mmap_auto_base(current_task->mmap_next_addr, size);
    if (!base) return -ENOMEM;
    current_task->mmap_next_addr = base + size;

    if (base + size < base || base + size >= KERNEL_VMA) return -EINVAL;
    if (!user_range_free_mem(base, size, 0, 0)) return -EINVAL;

    page_count = 0;
    new_pages = vmm_map_range_in_pml4_tracked(current_task->pml4_phys, base,
                                               size, 0x7 | VMM_PTE_NX,
                                               &page_count);

    if (!new_pages && size > 0) {
        return -ENOMEM;
    }

    if (new_pages && page_count > 0) {
        old_count = current_task->user_pages_count;
        new_count = old_count + page_count;
        expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
        if (!expanded) {
            release_user_leaf_range(base, base + size);
            kfree(new_pages);
            return -ENOMEM;
        }
        if (current_task->user_pages && old_count > 0) {
            memcpy(expanded, current_task->user_pages,
                   old_count * sizeof(uint64_t));
            kfree(current_task->user_pages);
        }
        memcpy(expanded + old_count, new_pages,
               page_count * sizeof(uint64_t));
        current_task->user_pages = expanded;
        current_task->user_pages_count = new_count;
        kfree(new_pages);
    }

    return (int)base;
}

static int sys_mmap2(void *addr, size_t length, int prot, int flags, int fd, int64_t pgoffset) {
    uint64_t size;
    uint64_t base;
    uint64_t page_count;
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t new_count;
    uint64_t *expanded;
    vfs_node_t *fnode;
    uint64_t file_off;
    uint64_t to_read;
    uint64_t nread;
    uint8_t *tmpbuf;
    framebuffer_t *fb_dev;
    uint64_t fb_phys_base;
    uint64_t fb_total;
    uint64_t fb_num_pages;
    uint64_t pi;
    task_fd_t *tfd;
    uint64_t pte_flags;
    uint64_t read_done;
    uint64_t read_chunk;
    
    if (length == 0 || !current_task) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if ((prot & (PROT_WRITE | PROT_EXEC)) ==
        (PROT_WRITE | PROT_EXEC)) return -EACCES;
    pte_flags = mmap_pte_flags(prot);

    size = (length + 0xFFF) & ~0xFFFu;

    if (size == 0 || size < length) return -EINVAL;
    if (!task_memory_allows(current_task, size)) return -ENOMEM;

    if (addr != NULL && ((uint64_t)addr & 0xFFFu) == 0 && (flags & 0x10)) {
        base = (uint64_t)addr;
    } else {
        base = user_mmap_auto_base(current_task->mmap_next_addr, size);
        if (!base) return -ENOMEM;
        current_task->mmap_next_addr = base + size;
    }

    if (base + size < base || base + size >= KERNEL_VMA) return -EINVAL;
    if (!user_range_free_mem(base, size, 0, 0)) return -EINVAL;

    if ((flags & 0x10) && base < KERNEL_VMA) {
        release_user_leaf_range(base, base + size);
    }

    if (fd >= 0) {
        tfd = task_fd_get(current_task, fd);
        if (tfd && tfd->in_use && tfd->node) {
            fnode = (vfs_node_t *)tfd->node;
            if (strcmp(fnode->name, "fb0") == 0) {
                fb_dev = fb_get();
                if (!fb_dev || !fb_dev->phys_addr) return -ENODEV;
                fb_phys_base = fb_dev->phys_addr;
                fb_total = fb_dev->pitch * fb_dev->height;
                fb_num_pages = (fb_total + 0xFFF) / 0x1000;
                if (size / 0x1000 < fb_num_pages) {
                    fb_num_pages = size / 0x1000;
                }
                for (pi = 0; pi < fb_num_pages; pi++) {
                    vmm_map_page_in_pml4(current_task->pml4_phys,
                        base + pi * 0x1000,
                        fb_phys_base + pi * 0x1000,
                        pte_flags | VMM_PTE_NOFREE);
                }
                return (int)base;
            }
        }
    }

    page_count = 0;
    new_pages = vmm_map_range_in_pml4_tracked(
        current_task->pml4_phys, base, size, pte_flags, &page_count);

    if (!new_pages && size > 0) {
        return -ENOMEM;
    }

    if (new_pages && page_count > 0) {
        old_count = current_task->user_pages_count;
        new_count = old_count + page_count;
        expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
        if (!expanded) {
            release_user_leaf_range(base, base + size);
            kfree(new_pages);
            return -ENOMEM;
        }
        if (current_task->user_pages && old_count > 0) {
            memcpy(expanded, current_task->user_pages,
                   old_count * sizeof(uint64_t));
            kfree(current_task->user_pages);
        }
        memcpy(expanded + old_count, new_pages,
               page_count * sizeof(uint64_t));
        current_task->user_pages = expanded;
        current_task->user_pages_count = new_count;
        kfree(new_pages);
    }

    if (fd >= 0) {
        tfd = task_fd_get(current_task, fd);
        if (tfd && tfd->in_use && tfd->node) {
            fnode = (vfs_node_t *)tfd->node;
            if (strcmp(fnode->name, "fb0") != 0) {
                file_off = (uint64_t)pgoffset * 4096;
                to_read = length < size ? length : size;
                tmpbuf = (uint8_t *)kmalloc(PAGE_SIZE);
                if (tmpbuf) {
                    read_done = 0;
                    while (read_done < to_read) {
                        read_chunk = to_read - read_done;
                        if (read_chunk > PAGE_SIZE) read_chunk = PAGE_SIZE;
                        nread = vfs_read(fnode, file_off + read_done,
                                         read_chunk, tmpbuf);
                        if (nread == 0) break;
                        vmm_copy_to_pml4(current_task->pml4_phys,
                                         base + read_done, tmpbuf, nread);
                        read_done += nread;
                        if (nread < read_chunk) break;
                    }
                    kfree(tmpbuf);
                }
            }
        }
    }

    return (int)base;
}

static int sys_munmap(void *addr, size_t length) {
    uint64_t base;
    uint64_t size;
    uint64_t end;

    if (!current_task) return -EINVAL;
    base = (uint64_t)addr;
    if (base & 0xFFF) return -EINVAL;
    if (length == 0) return -EINVAL;
    if (base >= KERNEL_VMA) return -EINVAL;

    size = (length + 0xFFF) & ~0xFFFu;
    end = base + size;
    if (end < base) return -EINVAL;
    if (end > KERNEL_VMA) end = KERNEL_VMA;

    if (end > base) {
        release_user_leaf_range(base, end);
    }

    if (end == current_task->mmap_next_addr &&
            user_mmap_range_valid(base, end - 1)) {
        current_task->mmap_next_addr = base;
    }

    return 0;
}

static int sys_mprotect(void *addr, size_t length, int prot) {
    uint64_t base;
    uint64_t size;

    if (!current_task) return -EINVAL;
    base = (uint64_t)(uintptr_t)addr;
    if (base & 0xFFFu) return -EINVAL;
    if (prot & ~7) return -EINVAL;
    if ((prot & (PROT_WRITE | PROT_EXEC)) ==
        (PROT_WRITE | PROT_EXEC)) return -EACCES;
    if (length == 0) return 0;
    size = (length + 0xFFFu) & ~0xFFFu;
    if (size < length || base >= KERNEL_VMA || base + size < base ||
            base + size > KERNEL_VMA) return -EINVAL;
    if (!user_range_mapped_mem(base, size)) return -ENOMEM;
    if (vmm_protect_range_in_pml4(current_task->pml4_phys, base, size,
                                  mmap_pte_flags(prot)) < 0) return -ENOMEM;
    return 0;
}

static void *sys_mremap(void *old_addr, size_t old_size, size_t new_size, int flags, void *new_addr) {
    uint64_t old_base;
    uint64_t old_len;
    uint64_t old_end;
    uint64_t base;
    uint64_t size;
    uint64_t page_count;
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t new_count;
    uint64_t *expanded;
    uint64_t copy_size;
    uint64_t old_flags;
    uint64_t additional_size;
    uint64_t copied;
    uint64_t chunk;
    uint8_t copy_buffer[256];

    (void)flags;

    if (!current_task) return (void *)(long)-ENOMEM;
    if (!old_addr || new_size == 0) return (void *)(long)-EINVAL;
    if (((uint64_t)old_addr & 0xFFFu) != 0) return (void *)(long)-EINVAL;

    old_base = (uint64_t)old_addr;
    old_len = (old_size + 0xFFF) & ~0xFFFu;
    size = (new_size + 0xFFF) & ~0xFFFu;
    if (old_len == 0 || size == 0 || old_len < old_size || size < new_size)
        return (void *)(long)-EINVAL;
    if (old_base >= KERNEL_VMA || old_base + old_len < old_base)
        return (void *)(long)-EINVAL;
    if (!user_range_mapped_mem(old_base, old_len))
        return (void *)(long)-EFAULT;

    if (size <= old_len) {
        old_end = old_base + old_len;
        if (old_base + size < old_end)
            release_user_leaf_range(old_base + size, old_end);
        return old_addr;
    }

    additional_size = size - old_len;
    if (!task_memory_allows(current_task, additional_size))
        return (void *)(long)-ENOMEM;
    old_flags = vmm_get_flags_in_pml4(current_task->pml4_phys, old_base);
    if (!(old_flags & VMM_PTE_PRESENT))
        return (void *)(long)-EFAULT;

    if (new_addr && ((uint64_t)new_addr & 0xFFFu) == 0) {
        base = (uint64_t)new_addr;
    } else {
        base = user_mmap_auto_base(current_task->mmap_next_addr, size);
        if (!base) return (void *)(long)-ENOMEM;
    }

    if (base + size < base || base + size >= KERNEL_VMA) return (void *)(long)-ENOMEM;
    if (base < old_base + old_len && old_base < base + size)
        return (void *)(long)-EINVAL;
    if (!user_range_free_mem(base, size, old_base, old_len)) return (void *)(long)-ENOMEM;

    page_count = 0;
    new_pages = vmm_map_range_in_pml4_tracked(
        current_task->pml4_phys, base, size, 0x7, &page_count);
    if (!new_pages && size > 0) return (void *)(long)-ENOMEM;

    if (new_pages && page_count > 0) {
        old_count = current_task->user_pages_count;
        new_count = old_count + page_count;
        expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
        if (!expanded) {
            release_user_leaf_range(base, base + size);
            kfree(new_pages);
            return (void *)(long)-ENOMEM;
        }
        if (current_task->user_pages && old_count > 0) {
            memcpy(expanded, current_task->user_pages,
                   old_count * sizeof(uint64_t));
            kfree(current_task->user_pages);
        }
        memcpy(expanded + old_count, new_pages,
               page_count * sizeof(uint64_t));
        current_task->user_pages = expanded;
        current_task->user_pages_count = new_count;
        kfree(new_pages);
    }

    copy_size = old_size < new_size ? old_size : new_size;
    copied = 0;
    while (copied < copy_size) {
        chunk = copy_size - copied;
        if (chunk > sizeof(copy_buffer)) chunk = sizeof(copy_buffer);
        vmm_read_from_pml4(current_task->pml4_phys,
                           old_base + copied, copy_buffer, chunk);
        vmm_copy_to_pml4(current_task->pml4_phys,
                         base + copied, copy_buffer, chunk);
        copied += chunk;
    }
    if (vmm_protect_range_in_pml4(current_task->pml4_phys, base, size,
                                  old_flags) < 0) {
        release_user_leaf_range(base, base + size);
        return (void *)(long)-ENOMEM;
    }

    release_user_leaf_range(old_base, old_base + old_len);
    if (base + size > current_task->mmap_next_addr)
        current_task->mmap_next_addr = base + size;

    return (void *)base;
}

static int sys_madvise(void *addr, size_t length, int advice) {
    uint64_t base;

    base = (uint64_t)(uintptr_t)addr;
    if (base & 0xFFFu) return -EINVAL;
    if (length > 0 && (base >= KERNEL_VMA || base + length < base ||
            base + length > KERNEL_VMA)) return -ENOMEM;

    if ((advice >= 0 && advice <= 4) ||
            (advice >= 8 && advice <= 21) ||
            (advice >= 100 && advice <= 101)) return 0;
    return -EINVAL;
}

static int sys_mincore(void *addr, size_t length, unsigned char *vec) {
    uint64_t base;
    size_t pages;
    size_t i;
    size_t done;
    size_t chunk;
    uint8_t values[64];

    if (!current_task) return -ESRCH;
    if (!vec) return -EFAULT;
    base = (uint64_t)(uintptr_t)addr;
    if (base & 0xFFFu) return -EINVAL;
    if (length == 0) return -EINVAL;
    if (base >= KERNEL_VMA || base + length < base ||
            base + length > KERNEL_VMA) return -ENOMEM;
    pages = (length + 0xFFF) / 0x1000;
    if (!user_access_ok(vec, pages, UACCESS_WRITE)) return -EFAULT;
    for (i = 0; i < pages; i++) {
        if (!vmm_get_phys_in_pml4(current_task->pml4_phys,
                base + i * 0x1000)) return -ENOMEM;
    }
    memset(values, 1, sizeof(values));
    done = 0;
    while (done < pages) {
        chunk = pages - done;
        if (chunk > sizeof(values)) chunk = sizeof(values);
        if (copy_to_user(vec + done, values, chunk) < 0) return -EFAULT;
        done += chunk;
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

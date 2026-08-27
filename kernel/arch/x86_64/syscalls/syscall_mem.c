#include "syscall_defs.h"
#include <stdint.h>
#include <stddef.h>

extern void pfa_cow_release(uint64_t phys_addr);

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4
#define MADV_DONTNEED 4
#define MADV_FREE 8

static int sys_msync(void *addr, size_t length, int flags);

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
    if (addr >= USER_DYNAMIC_BASE && end < USER_DYNAMIC_LIMIT) return 1;
    return addr >= USER_HIGH_DYNAMIC_BASE && end < USER_HIGH_DYNAMIC_LIMIT;
}

static int user_range_free_pages(uint64_t addr, uint64_t size,
                                 uint64_t allow_base,
                                 uint64_t allow_size) {
    uint64_t end;
    uint64_t page;
    uint64_t last;
    uint64_t allow_end;
    uint64_t range_end;
    uint64_t area_end;
    int i;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size - 1;
    if (end < addr) return 0;
    range_end = end + 1;
    allow_end = allow_base + allow_size;
    for (i = 0; i < current_task->file_map_count; i++) {
        area_end = current_task->file_maps[i].vaddr +
                   current_task->file_maps[i].memsz;
        if (area_end < current_task->file_maps[i].vaddr) return 0;
        if (addr >= area_end || range_end <= current_task->file_maps[i].vaddr)
            continue;
        if (allow_size != 0 &&
            current_task->file_maps[i].vaddr >= allow_base &&
            area_end <= allow_end) continue;
        return 0;
    }
    page = addr & ~(PAGE_SIZE - 1u);
    last = end & ~(PAGE_SIZE - 1u);
    for (;;) {
        if (vmm_get_phys_in_pml4(current_task->pml4_phys, page) != 0 &&
            !(allow_size != 0 && page >= allow_base && page < allow_end))
            return 0;
        if (page == last) break;
        page += PAGE_SIZE;
    }
    return 1;
}

static uint64_t user_mmap_auto_base(uint64_t next, uint64_t size) {
    uint64_t cursor;
    uint64_t base;

    if (size == 0 || size > USER_HIGH_DYNAMIC_LIMIT - USER_HIGH_DYNAMIC_BASE)
        return 0;
    if (next >= USER_HIGH_DYNAMIC_BASE && next <= USER_HIGH_DYNAMIC_LIMIT) {
        cursor = next & ~(PAGE_SIZE - 1u);
        while (cursor >= USER_HIGH_DYNAMIC_BASE + size) {
            base = cursor - size;
            if (user_range_free_pages(base, size, 0, 0)) return base;
            cursor -= PAGE_SIZE;
        }
        return 0;
    }
    cursor = next;
    if (cursor <= USER_DYNAMIC_BASE || cursor > USER_DYNAMIC_LIMIT)
        cursor = USER_DYNAMIC_LIMIT;
    cursor &= ~(PAGE_SIZE - 1u);
    while (cursor >= USER_DYNAMIC_BASE + size) {
        base = cursor - size;
        if (user_range_free_pages(base, size, 0, 0)) return base;
        cursor -= PAGE_SIZE;
    }
    cursor = USER_HIGH_DYNAMIC_LIMIT & ~(PAGE_SIZE - 1u);
    while (cursor >= USER_HIGH_DYNAMIC_BASE + size) {
        base = cursor - size;
        if (user_range_free_pages(base, size, 0, 0)) return base;
        cursor -= PAGE_SIZE;
    }
    return 0;
}

#define user_range_mapped_mem(addr, size) \
    syscall_user_range_present((addr), (size), 1, 0)

static int user_range_covered_by_vmas(uint64_t addr, uint64_t size) {
    uint64_t end;
    uint64_t position;
    uint64_t area_end;
    int found;
    int i;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size;
    if (end < addr || end > KERNEL_VMA) return 0;
    position = addr;
    while (position < end) {
        found = 0;
        for (i = 0; i < current_task->file_map_count; i++) {
            area_end = current_task->file_maps[i].vaddr +
                       current_task->file_maps[i].memsz;
            if (position < current_task->file_maps[i].vaddr ||
                position >= area_end) continue;
            position = area_end < end ? area_end : end;
            found = 1;
            break;
        }
        if (!found) return 0;
    }
    return 1;
}

static int user_range_free_mem(uint64_t addr, uint64_t size, uint64_t allow_base, uint64_t allow_size) {
    uint64_t end;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size - 1;
    if (end < addr) return 0;
    if (!user_mmap_range_valid(addr, end)) return 0;
    return user_range_free_pages(addr, size, allow_base, allow_size);
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
                pfa_cow_release(phys);
            }
        }
    }
    compact_user_pages();
    vmm_prune_user_range(current_task->pml4_phys, base, end - base);
}

static int64_t sys_brk(uint64_t addr, const char *unused, int unused2) {
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
        return (int64_t)current_brk;
    }

    if (requested < current_task->user_brk_start) {
        return (int64_t)current_brk;
    }
    if (!task_data_allows(current_task, requested)) return (int64_t)current_brk;
    
    if (requested < current_brk) {
        old_page_end = (current_brk + 0xFFF) & ~0xFFFu;
        shrink_page_end = (requested + 0xFFF) & ~0xFFFu;
        if (old_page_end > shrink_page_end) {
            release_user_leaf_range(shrink_page_end, old_page_end);
        }
        current_task->user_brk = requested;
        return (int64_t)requested;
    }
    
    newbrk = (requested + 0xFFF) & ~0xFFFu;
    
    if (newbrk > USER_DYNAMIC_LIMIT) {
        return (int64_t)current_brk;
    }
    
    old_page_end = (current_brk + 0xFFF) & ~0xFFFu;
    if (newbrk > old_page_end) {
        if (!user_range_free_pages(old_page_end,
                                   newbrk - old_page_end, 0, 0))
            return (int64_t)current_brk;
        if (!task_memory_allows(current_task, newbrk - old_page_end)) {
            return (int64_t)current_brk;
        }
        page_count = 0;
        new_pages = vmm_map_range_in_pml4_tracked(
            current_task->pml4_phys, old_page_end, newbrk - old_page_end,
            0x7 | VMM_PTE_NX, &page_count);
        
        if (!new_pages && (newbrk > old_page_end)) {
            return (int64_t)current_brk;
        }
        
        if (new_pages && page_count > 0) {
            old_count = current_task->user_pages_count;
            new_count = old_count + page_count;
            expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
            if (!expanded) {
                release_user_leaf_range(old_page_end, newbrk);
                kfree(new_pages);
                return (int64_t)current_brk;
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
    return (int64_t)requested;
}

static int64_t sys_mmap(int a1, const char *a2, int a3) {
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

    if (base + size < base || base + size >= KERNEL_VMA) return -EINVAL;
    if (!user_range_free_mem(base, size, 0, 0)) return -EINVAL;
    current_task->mmap_next_addr = base;

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

    if (task_add_vm_area(current_task, NULL, base, size, 0, 0,
                         0x7 | VMM_PTE_NX,
                         TASK_VMA_PRIVATE | TASK_VMA_ANONYMOUS) != 0) {
        release_user_leaf_range(base, base + size);
        return -ENOMEM;
    }

    return (int64_t)base;
}

static int64_t sys_mmap2(void *addr, size_t length, int prot, int flags, int fd, int64_t pgoffset) {
    uint64_t size;
    uint64_t base;
    uint64_t page_count;
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t new_count;
    uint64_t *expanded;
    vfs_node_t *fnode;
    uint64_t file_off;
    framebuffer_t *fb_dev;
    uint64_t fb_phys_base;
    uint64_t fb_total;
    uint64_t fb_num_pages;
    uint64_t pi;
    task_fd_t *tfd;
    uint64_t pte_flags;
    uint32_t vma_map_flags;
    vfs_node_t *vma_node;
    uint64_t vma_file_size;
    uint64_t vma_offset;
    
    if (length == 0 || !current_task) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if ((prot & (PROT_WRITE | PROT_EXEC)) ==
        (PROT_WRITE | PROT_EXEC)) return -EACCES;
    pte_flags = mmap_pte_flags(prot);
    if ((flags & 0x3) != 0x1 && (flags & 0x3) != 0x2) return -EINVAL;
    if (!(flags & 0x20) && fd < 0) return -EBADF;
    if (!(flags & 0x20) && pgoffset < 0) return -EINVAL;
    vma_map_flags = (flags & 0x1) ? TASK_VMA_SHARED : TASK_VMA_PRIVATE;
    if (flags & 0x1) pte_flags |= VMM_PTE_SHARED;
    if (fd < 0 || (flags & 0x20)) vma_map_flags |= TASK_VMA_ANONYMOUS;
    else vma_map_flags |= TASK_VMA_FILE;
    vma_node = NULL;
    vma_file_size = 0;
    vma_offset = 0;
    tfd = NULL;
    if (!(flags & 0x20)) {
        tfd = task_fd_get(current_task, fd);
        if (!tfd || !tfd->in_use || !tfd->node) return -EBADF;
        if ((prot & PROT_EXEC) &&
            (vfs_get_mount_flags_for_node((vfs_node_t *)tfd->node) &
             VFS_MS_NOEXEC)) return -EACCES;
        if ((flags & 0x1) && (prot & PROT_WRITE) &&
            ((tfd->flags & 0x3) == VFS_O_RDONLY)) return -EACCES;
    }

    size = (length + 0xFFF) & ~0xFFFu;

    if (size == 0 || size < length) return -EINVAL;
    if (!task_memory_allows(current_task, size)) return -ENOMEM;

    if (flags & 0x10) {
        if ((uint64_t)addr & 0xFFFu) return -EINVAL;
        base = (uint64_t)addr;
    } else {
        base = user_mmap_auto_base(current_task->mmap_next_addr, size);
        if (!base) return -ENOMEM;
        current_task->mmap_next_addr = base;
    }

    if (base < 0x1000) return -EPERM;
    if (base + size < base || base + size >= KERNEL_VMA) return -EINVAL;
    if (!user_mmap_range_valid(base, base + size - 1)) return -EINVAL;
    if ((flags & 0x10) && base < current_task->user_brk &&
        base + size > current_task->user_brk_start) return -EINVAL;
    if ((flags & 0x10) && base < KERNEL_VMA) {
        if (task_unmap_vm_areas(current_task, base, size) != 0)
            return -ENOMEM;
        release_user_leaf_range(base, base + size);
    } else if (!user_range_free_mem(base, size, 0, 0)) {
        return -EINVAL;
    }

    if (fd >= 0) {
        if (tfd && tfd->in_use && tfd->node) {
            fnode = (vfs_node_t *)tfd->node;
            if (strcmp(vfs_node_name(fnode), "fb0") == 0) {
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
                if (task_add_vm_area(current_task, fnode, base, size,
                                     fb_total, 0,
                                     pte_flags | VMM_PTE_NOFREE,
                                     TASK_VMA_SHARED | TASK_VMA_DEVICE) != 0) {
                    release_user_leaf_range(base, base + size);
                    return -ENOMEM;
                }
                return (int64_t)base;
            }
        }
    }

    if (fd >= 0 && tfd && tfd->in_use && tfd->node) {
        fnode = (vfs_node_t *)tfd->node;
        if (strcmp(vfs_node_name(fnode), "fb0") != 0) {
            file_off = (uint64_t)pgoffset * 4096;
            vma_node = fnode;
            vma_file_size = length;
            vma_offset = file_off;
        }
    }

    page_count = 0;
    new_pages = NULL;
    if (!vma_node)
        new_pages = vmm_map_range_in_pml4_tracked(
            current_task->pml4_phys, base, size, pte_flags, &page_count);

    if (!vma_node && !new_pages && size > 0) {
        task_memory_pressure_reclaim_now();
        new_pages = vmm_map_range_in_pml4_tracked(
            current_task->pml4_phys, base, size, pte_flags, &page_count);
        if (!new_pages) return -ENOMEM;
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

    if (task_add_vm_area(current_task, vma_node, base, size,
                         vma_file_size, vma_offset, pte_flags,
                         vma_map_flags) != 0) {
        release_user_leaf_range(base, base + size);
        return -ENOMEM;
    }

    return (int64_t)base;
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

    if (sys_msync(addr, end - base, MS_ASYNC) < 0) return -EIO;

    if (end > base && task_unmap_vm_areas(current_task, base,
                                           end - base) != 0) {
        return -ENOMEM;
    }

    if (end > base) {
        release_user_leaf_range(base, end);
    }

    if (base == current_task->mmap_next_addr &&
            user_mmap_range_valid(base, end - 1)) {
        current_task->mmap_next_addr = end;
    }

    return 0;
}

static int sys_mprotect(void *addr, size_t length, int prot) {
    uint64_t base;
    uint64_t size;
    uint64_t end;
    uint64_t area_end;
    uint64_t page;
    uint64_t pte_flags;
    uint64_t current_flags;
    int cow_result;
    int i;

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
    end = base + size;
    if (prot & PROT_EXEC) {
        for (i = 0; i < current_task->file_map_count; i++) {
            area_end = current_task->file_maps[i].vaddr +
                       current_task->file_maps[i].memsz;
            if (end <= current_task->file_maps[i].vaddr ||
                base >= area_end || !current_task->file_maps[i].node)
                continue;
            if (vfs_get_mount_flags_for_node(current_task->file_maps[i].node) &
                VFS_MS_NOEXEC) return -EACCES;
        }
    }
    if (!user_range_covered_by_vmas(base, size)) return -ENOMEM;
    pte_flags = mmap_pte_flags(prot);
    if (task_protect_vm_areas(current_task, base, size, pte_flags) != 0)
        return -ENOMEM;
    for (page = base; page < end; page += PAGE_SIZE) {
        if (!vmm_get_phys_in_pml4(current_task->pml4_phys, page)) continue;
        current_flags = vmm_get_flags_in_pml4(current_task->pml4_phys,
                                              page);
        if (current_flags & VMM_PTE_COW) {
            cow_result = cow_handle_fault(page,
                                          current_task->pml4_phys);
            if (cow_result != 1) return -ENOMEM;
            current_flags = vmm_get_flags_in_pml4(
                current_task->pml4_phys, page);
        }
        if ((pte_flags & VMM_PTE_WRITE) &&
            (current_flags & VMM_PTE_NOFREE) &&
            !task_handle_file_write_fault(current_task, page))
            return -ENOMEM;
        if (vmm_protect_page_in_pml4(current_task->pml4_phys, page,
                                     pte_flags) < 0) return -ENOMEM;
    }
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
    old_flags &= ~(VMM_PTE_COW | VMM_PTE_NOFREE | VMM_PTE_SHARED);

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
    if (base < current_task->mmap_next_addr)
        current_task->mmap_next_addr = base;

    return (void *)base;
}

static int sys_madvise(void *addr, size_t length, int advice) {
    uint64_t base;
    uint64_t size;
    uint64_t end;
    uint64_t page;
    int covered;
    int i;

    base = (uint64_t)(uintptr_t)addr;
    if (base & 0xFFFu) return -EINVAL;
    if (length > 0 && (base >= KERNEL_VMA || base + length < base ||
            base + length > KERNEL_VMA)) return -ENOMEM;

    if (!((advice >= 0 && advice <= 4) ||
            (advice >= 8 && advice <= 21) ||
            (advice >= 100 && advice <= 101))) return -EINVAL;
    if (length == 0 || (advice != MADV_DONTNEED && advice != MADV_FREE))
        return 0;
    size = align_up_u64(length, PAGE_SIZE);
    if (size == 0 || base + size < base || base + size > KERNEL_VMA)
        return -ENOMEM;
    end = base + size;
    for (page = base; page < end; page += PAGE_SIZE) {
        covered = 0;
        for (i = 0; i < current_task->file_map_count; i++) {
            if (page >= current_task->file_maps[i].vaddr &&
                page < current_task->file_maps[i].vaddr +
                    current_task->file_maps[i].memsz) {
                covered = 1;
                break;
            }
        }
        if (!covered) return -ENOMEM;
    }
    if (sys_msync(addr, length, MS_ASYNC) < 0) return -EIO;
    release_user_leaf_range(base, end);
    return 0;
}

static int sys_msync(void *addr, size_t length, int flags) {
    uint64_t base;
    uint64_t end;
    uint64_t area_start;
    uint64_t area_end;
    uint64_t start;
    uint64_t finish;
    uint64_t position;
    uint64_t chunk;
    uint64_t file_position;
    uint64_t written;
    uint8_t *buffer;
    int matched;
    int i;

    if (!current_task) return -ESRCH;
    if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) return -EINVAL;
    if ((flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
        return -EINVAL;
    base = (uint64_t)(uintptr_t)addr;
    if (base & (PAGE_SIZE - 1)) return -EINVAL;
    if (length == 0) return 0;
    end = base + length;
    if (end < base || end > KERNEL_VMA) return -ENOMEM;
    buffer = (uint8_t *)slab_page_alloc(PAGE_SIZE);
    if (!buffer) return -ENOMEM;
    matched = 0;
    for (i = 0; i < current_task->file_map_count; i++) {
        area_start = current_task->file_maps[i].vaddr;
        area_end = area_start + current_task->file_maps[i].memsz;
        if (end <= area_start || base >= area_end) continue;
        matched = 1;
        if (!(current_task->file_maps[i].map_flags & TASK_VMA_SHARED) ||
            !(current_task->file_maps[i].map_flags & TASK_VMA_FILE) ||
            !current_task->file_maps[i].node) continue;
        start = base > area_start ? base : area_start;
        finish = end < area_end ? end : area_end;
        if (finish > area_start + current_task->file_maps[i].filesz)
            finish = area_start + current_task->file_maps[i].filesz;
        position = start;
        while (position < finish) {
            chunk = finish - position;
            if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
            if (!vmm_get_phys_in_pml4(current_task->pml4_phys, position)) {
                position += chunk;
                continue;
            }
            vmm_read_from_pml4(current_task->pml4_phys, position,
                               buffer, chunk);
            file_position = current_task->file_maps[i].offset +
                            position - area_start;
            written = vfs_write(current_task->file_maps[i].node,
                                file_position, chunk, buffer);
            if (written != chunk) {
                slab_page_free(buffer, PAGE_SIZE);
                return -EIO;
            }
            position += chunk;
        }
        if ((flags & MS_SYNC) &&
            vfs_sync_node(current_task->file_maps[i].node, 0) != 0) {
            slab_page_free(buffer, PAGE_SIZE);
            return -EIO;
        }
    }
    slab_page_free(buffer, PAGE_SIZE);
    if (!matched) return -ENOMEM;
    if (flags & MS_INVALIDATE)
        release_user_leaf_range(base, align_up_u64(end, PAGE_SIZE));
    return 0;
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
    syscall_table_set(SYSCALL_SBRK, (void *)(sys_brk));
    syscall_table_set(SYSCALL_MMAP, (void *)(sys_mmap));
    syscall_table_set(SYSCALL_MMAP2, (void *)(sys_mmap2));
    syscall_table_set(SYSCALL_MUNMAP, (void *)(sys_munmap));
    syscall_table_set(SYSCALL_MPROTECT, (void *)(sys_mprotect));
    syscall_table_set(SYSCALL_MREMAP, (void *)(sys_mremap));
    syscall_table_set(SYSCALL_MADVISE, (void *)(sys_madvise));
    syscall_table_set(SYSCALL_MINCORE, (void *)(sys_mincore));
    syscall_table_set(SYSCALL_MSYNC, (void *)(sys_msync));
}

#include "task_priv.h"

static spinlock_t user_zero_page_lock = {0};
static spinlock_t task_anon_fault_commit_lock = {0};
static uint64_t user_zero_page_phys = 0;
static mutex_t task_file_fault_commit_lock;

uint64_t user_zero_page_get(void) {
    uint64_t phys;
    uint64_t existing;

    spin_lock(&user_zero_page_lock);
    existing = user_zero_page_phys;
    spin_unlock(&user_zero_page_lock);
    if (existing) return existing;

    phys = pfa_alloc();
    if (!phys) return 0;
    pmm_zero_page_phys(phys);

    spin_lock(&user_zero_page_lock);
    existing = user_zero_page_phys;
    if (!existing) {
        user_zero_page_phys = phys;
        existing = phys;
        phys = 0;
    }
    spin_unlock(&user_zero_page_lock);
    if (phys) pfa_free(phys);
    return existing;
}

static uint64_t exec_page_find_active(vfs_node_t *node, uint64_t offset) {
    task_t *task;
    task_file_map_t *map;
    uint64_t vaddr;
    uint64_t phys;
    int i;

    if (!node) return 0;
    phys = 0;
    lock_scheduler();
    for (task = all_tasks_head; task && task_ptr_valid(task) && !phys;
         task = task->all_next) {
        if (task == current_task || !task->is_user ||
            task->state == TASK_DEAD || task->resources_released ||
            task->exec_completed != 0 || !task->pml4_phys ||
            !task->vm->file_maps || task->vm->file_map_count <= 0 ||
            task->vm->file_map_count > task->vm->file_map_capacity)
            continue;
        for (i = 0; i < task->vm->file_map_count; i++) {
            map = &task->vm->file_maps[i];
            if (!(map->map_flags & TASK_VMA_CLEAN) ||
                (map->map_flags & TASK_VMA_SHARED)) continue;
            if (offset < map->offset ||
                offset >= map->offset + map->filesz ||
                map->offset + map->filesz - offset < PAGE_SIZE)
                continue;
            if (!overlay_same_file(map->node, node)) continue;
            if (task_is_current_on_any_cpu(task)) break;
            vaddr = map->vaddr + (offset - map->offset);
            phys = vmm_get_phys_in_pml4(task->pml4_phys, vaddr);
            if (phys) break;
        }
    }
    if (phys && pfa_ref_share(phys) != 0) phys = 0;
    unlock_scheduler();
    return phys;
}

int task_handle_file_page_fault(task_t *task, uint64_t fault_addr) {
    uint64_t page;
    uint64_t phys;
    uint64_t read_start;
    uint64_t read_end;
    uint64_t read_off;
    uint64_t read_len;
    uint64_t mapped_phys;
    uint64_t map_flags;
    uint64_t read_actual;
    int cached_page;
    int match;
    int share_page;
    int memory_allowed;
    int i;
    uint8_t pinned;
    uint8_t stage;

    if (!task || !task->is_user) return 0;
    page = fault_addr & ~(PAGE_SIZE - 1);
    for (i = 0; i < task->vm->file_map_count; i++) {
        if (!task->vm->file_maps[i].node) continue;
        if (page < task->vm->file_maps[i].vaddr) continue;
        if (page >= task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz) continue;
        if (task->vm->file_maps[i].flags & 0x2) {
            break;
        }
    }
    match = i;
    if (match >= task->vm->file_map_count) {
        for (i = 0; i < task->vm->file_map_count; i++) {
            if (!task->vm->file_maps[i].node) continue;
            if (page < task->vm->file_maps[i].vaddr) continue;
            if (page >= task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz) continue;
            match = i;
            break;
        }
    }
    if (match >= task->vm->file_map_count) return 0;
    pinned = task->kernel_cpu_pinned;
    task->kernel_cpu_pinned = 1;
    memory_allowed = task_memory_allows(task, PAGE_SIZE);
    if (!memory_allowed) {
        task->kernel_cpu_pinned = pinned;
        return 0;
    }

    phys = 0;
    cached_page = 0;
    map_flags = task->vm->file_maps[match].flags;
    read_start = page;
    read_end = page + PAGE_SIZE;
    if (read_start < task->vm->file_maps[match].vaddr) read_start = task->vm->file_maps[match].vaddr;
    if (read_end > task->vm->file_maps[match].vaddr + task->vm->file_maps[match].filesz) {
        read_end = task->vm->file_maps[match].vaddr + task->vm->file_maps[match].filesz;
    }
    if (read_end > read_start) {
        read_off = task->vm->file_maps[match].offset + (read_start - task->vm->file_maps[match].vaddr);
        read_len = read_end - read_start;
        share_page = read_start == page && read_len == PAGE_SIZE &&
                     (task->vm->file_maps[match].map_flags & TASK_VMA_CLEAN) &&
                     !(task->vm->file_maps[match].map_flags & TASK_VMA_SHARED);
        if (share_page) {
            phys = exec_page_find_active(task->vm->file_maps[match].node,
                                         read_off & ~(PAGE_SIZE - 1));
            if (phys) cached_page = 1;
        }
        if (!phys) {
            phys = pfa_alloc();
            if (!phys) {
                task->kernel_cpu_pinned = pinned;
                return 0;
            }
            stage = task->kernel_stage;
            task->kernel_stage = TASK_KERNEL_STAGE_FILE_FAULT;
            read_actual = vfs_read_phys_page(task->vm->file_maps[match].node,
                                             read_off, read_len, phys,
                                             read_start - page);
            task->kernel_stage = stage;
            if (read_actual != read_len &&
                (read_actual > read_len ||
                 read_off + read_actual <
                 task->vm->file_maps[match].node->length)) {
                pfa_free(phys);
                task->kernel_cpu_pinned = pinned;
                return 0;
            }
        }
    } else {
        phys = user_zero_page_get();
        if (!phys) {
            task->kernel_cpu_pinned = pinned;
            return 0;
        }
        map_flags = (map_flags & ~0x2ULL) | VMM_PTE_NOFREE;
        cached_page = 2;
    }
    mutex_lock(&task_file_fault_commit_lock);
    mapped_phys = vmm_get_phys_in_pml4(task->pml4_phys, page);
    if (mapped_phys) {
        mutex_unlock(&task_file_fault_commit_lock);
        if (cached_page == 1)
            pfa_ref_dec(phys);
        else if (!cached_page)
            pfa_free(phys);
        task->kernel_cpu_pinned = pinned;
        return 1;
    }
    if (vmm_map_page_in_pml4(task->pml4_phys, page, phys, map_flags) < 0) {
        mutex_unlock(&task_file_fault_commit_lock);
        if (cached_page == 1) {
            pfa_ref_dec(phys);
        } else if (!cached_page) {
            pfa_free(phys);
        }
        task->kernel_cpu_pinned = pinned;
        return 0;
    }
    mapped_phys = vmm_get_phys_in_pml4(task->pml4_phys, page);
    if (mapped_phys != phys) {
        vmm_unmap_page_in_pml4(task->pml4_phys, page);
        mutex_unlock(&task_file_fault_commit_lock);
        if (cached_page == 1) {
            pfa_ref_dec(phys);
        } else if (!cached_page) {
            pfa_free(phys);
        }
        task->kernel_cpu_pinned = pinned;
        return 0;
    }
    if (task_track_user_page(task, phys) != 0) {
        vmm_unmap_page_in_pml4(task->pml4_phys, page);
        mutex_unlock(&task_file_fault_commit_lock);
        if (cached_page == 1) {
            pfa_ref_dec(phys);
        } else if (!cached_page) {
            pfa_free(phys);
        }
        task->kernel_cpu_pinned = pinned;
        return 0;
    }
    mutex_unlock(&task_file_fault_commit_lock);
    task->kernel_cpu_pinned = pinned;
    if (cached_page == 1 || cached_page == 2)
        task->minor_faults++;
    else
        task->major_faults++;
    return 1;
}

int task_handle_anon_page_fault(task_t *task, uint64_t fault_addr,
                                int write_fault) {
    task_file_map_t *area;
    uint64_t page;
    uint64_t physical;
    uint64_t map_flags;
    uint64_t irq_flags;
    int mapped;
    int result;
    int i;

    if (!task || fault_addr >= KERNEL_VMA) return 0;
    page = fault_addr & ~(PAGE_SIZE - 1);
    for (i = 0; i < task->vm->file_map_count; i++) {
        area = &task->vm->file_maps[i];
        if (!(area->map_flags & TASK_VMA_ANONYMOUS) ||
            (area->map_flags & TASK_VMA_SHARED)) continue;
        if (page < area->vaddr || page >= area->vaddr + area->memsz) continue;
        if (write_fault && !(area->flags & VMM_PTE_WRITE)) return 0;
        map_flags = area->flags;
        if (write_fault) {
            physical = pfa_alloc();
            if (!physical) return 0;
            pmm_zero_page_phys(physical);
        } else {
            physical = user_zero_page_get();
            if (!physical) return 0;
            map_flags = (map_flags & ~0x2ULL) | VMM_PTE_NOFREE;
        }
        __asm__ volatile ("pushfq; popq %0; cli"
                          : "=r"(irq_flags) :: "memory");
        spin_lock(&task_anon_fault_commit_lock);
        mapped = 0;
        result = 0;
        if (vmm_get_phys_in_pml4(task->pml4_phys, page) != 0) {
            result = 1;
        } else if (vmm_map_page_in_pml4(task->pml4_phys, page, physical,
                                        map_flags) == 0) {
            if (task_track_user_page(task, physical) == 0) {
                mapped = 1;
                result = 1;
                task->minor_faults++;
            } else {
                vmm_unmap_page_in_pml4(task->pml4_phys, page);
            }
        }
        spin_unlock(&task_anon_fault_commit_lock);
        if (irq_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
        if (!mapped && write_fault) pfa_free(physical);
        return result;
    }
    return 0;
}

void task_untrack_user_page(task_t *task, uint64_t physical) {
    if (task && physical && task->vm->user_pages_count)
        task->vm->user_pages_count--;
}

int task_handle_file_write_fault(task_t *task, uint64_t fault_addr) {
    uint64_t page;
    uint64_t old_phys;
    uint64_t new_phys;
    uint64_t map_flags;
    uint8_t *old_ptr;
    uint8_t *new_ptr;
    int match;
    int i;

    if (!task || !task->is_user) return 0;
    page = fault_addr & ~(PAGE_SIZE - 1);
    match = -1;
    for (i = 0; i < task->vm->file_map_count; i++) {
        if (!(task->vm->file_maps[i].flags & 0x2)) continue;
        if (task->vm->file_maps[i].map_flags & TASK_VMA_SHARED) continue;
        if (!task->vm->file_maps[i].node &&
            !(task->vm->file_maps[i].map_flags & TASK_VMA_ANONYMOUS)) continue;
        if (page < task->vm->file_maps[i].vaddr) continue;
        if (page >= task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz) continue;
        match = i;
        break;
    }
    if (match < 0) return 0;

    old_phys = vmm_get_phys_in_pml4(task->pml4_phys, page);
    if (!old_phys) return 0;

    new_phys = pfa_alloc();
    if (!new_phys) return 0;

    old_ptr = (uint8_t *)TEMP_SLOT(6);
    new_ptr = (uint8_t *)TEMP_SLOT(7);
    temp_map_raw((uint64_t)old_ptr, old_phys);
    temp_map_raw((uint64_t)new_ptr, new_phys);
    memcpy(new_ptr, old_ptr, PAGE_SIZE);
    temp_unmap_raw((uint64_t)old_ptr);
    temp_unmap_raw((uint64_t)new_ptr);

    task->vm->file_maps[match].map_flags &= ~TASK_VMA_CLEAN;
    map_flags = task->vm->file_maps[match].flags | 0x2ULL;
    vmm_map_page_in_pml4(task->pml4_phys, page, new_phys, map_flags);
    if (vmm_get_phys_in_pml4(task->pml4_phys, page) != new_phys) {
        pfa_free(new_phys);
        return 0;
    }

    if (old_phys != user_zero_page_phys) {
        pfa_cow_release64(old_phys);
    }
    return 1;
}

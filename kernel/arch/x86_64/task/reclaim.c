#include "task_priv.h"

static volatile int memory_pressure_pending;
static uint64_t memory_pressure_last_tick;

int task_oom_kill_one(void) {
    task_t *t;
    pid_t victim_pid;
    uint64_t pages;
    uint64_t worst;
    int pending;

    victim_pid = 0;
    worst = 0;
    pending = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t && task_ptr_valid(t)) {
        if (t->state != TASK_DEAD &&
            __atomic_load_n(&t->exit_code, __ATOMIC_ACQUIRE) ==
                (TASK_EXIT_PENDING | 137)) {
            pending = 1;
            break;
        }
        if (t->is_user && t != task_current() && t->pid != TASK_INIT_PID &&
            !(__atomic_load_n(&t->exit_code, __ATOMIC_ACQUIRE) & TASK_EXIT_PENDING) &&
            (t->state == TASK_READY || t->state == TASK_RUNNING ||
             t->state == TASK_BLOCKED)) {
            pages = t->vm->user_pages_count;
            if (pages > worst) {
                worst = pages;
                victim_pid = t->pid;
            }
        }
        t = t->all_next;
    }
    unlock_scheduler();
    if (pending || !victim_pid) return 0;
    t = task_find(victim_pid);
    if (!t || !t->is_user || t->pid == TASK_INIT_PID) return 0;
    printf("oom: killing pid %d (%u pages)\n", victim_pid,
           (unsigned)worst);
    task_kill(t, 137);
    return 1;
}

static uint64_t task_stack_pages(task_t *task) {
    if (!task || task->stack_size == 0) return 0;
    return (task->stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
}

static uint64_t task_file_pages(task_t *task) {
    uint64_t pages;
    int i;

    if (!task || !task->pml4_phys) return 0;
    pages = 0;
    for (i = 0; i < task->vm->file_map_count; i++) {
        if (!task->vm->file_maps[i].node) continue;
        pages += vmm_count_present_pages_in_range(task->pml4_phys,
                                                  task->vm->file_maps[i].vaddr,
                                                  task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz);
    }
    return pages;
}

static uint64_t task_heap_pages(task_t *task) {
    if (!task || !task->pml4_phys) return 0;
    if (task->vm->user_brk <= task->vm->user_brk_start) return 0;
    return vmm_count_present_pages_in_range(task->pml4_phys, task->vm->user_brk_start, task->vm->user_brk);
}

static uint64_t task_mmap_pages(task_t *task) {
    uint64_t pages;
    int i;

    if (!task || !task->pml4_phys) return 0;
    pages = 0;
    for (i = 0; i < task->vm->file_map_count; i++) {
        if (task->vm->file_maps[i].node) continue;
        pages += vmm_count_present_pages_in_range(
            task->pml4_phys, task->vm->file_maps[i].vaddr,
            task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz);
    }
    return pages;
}

static int task_pml4_has_multiple_users(task_t *owner) {
    task_t *task;

    if (!owner || !owner->pml4_phys) return 1;
    task = all_tasks_head;
    while (task && task_ptr_valid(task)) {
        if (task != owner && !task->resources_released &&
            task->pml4_phys == owner->pml4_phys)
            return 1;
        task = task->all_next;
    }
    return 0;
}

static uint64_t task_reclaim_file_exec_pages(task_t *task,
                                             uint64_t max_pages,
                                             uint64_t protected_address) {
    task_file_map_t *area;
    uint64_t address;
    uint64_t area_end;
    uint64_t removed;
    uint64_t flags;
    uint64_t reclaimed;
    uint64_t prune_start;
    uint64_t prune_end;
    int i;

    if (!task || !task->is_user || !task->pml4_phys || max_pages == 0)
        return 0;
    reclaimed = 0;
    prune_start = 0;
    prune_end = 0;
    for (i = 0; i < task->vm->file_map_count && reclaimed < max_pages; i++) {
        area = &task->vm->file_maps[i];
        if (!area->node ||
            (area->map_flags & (TASK_VMA_FILE | TASK_VMA_PRIVATE)) !=
            (TASK_VMA_FILE | TASK_VMA_PRIVATE) ||
            (area->map_flags & (TASK_VMA_SHARED | TASK_VMA_ANONYMOUS)) ||
            !(area->flags & VMM_PTE_USER) ||
            (area->flags & VMM_PTE_WRITE))
            continue;
        area_end = area->vaddr + area->memsz;
        if (area_end < area->vaddr) continue;
        for (address = area->vaddr;
             address < area_end && reclaimed < max_pages;
             address += PAGE_SIZE) {
            if (protected_address >= address &&
                protected_address < address + PAGE_SIZE) continue;
            flags = vmm_get_flags_in_pml4(task->pml4_phys, address);
            if (!(flags & VMM_PTE_PRESENT) ||
                (flags & (VMM_PTE_WRITE | VMM_PTE_COW | VMM_PTE_NOFREE |
                          VMM_PTE_SHARED)) ||
                !(flags & VMM_PTE_USER))
                continue;
            removed = vmm_unmap_page_in_pml4(task->pml4_phys, address);
            if (!removed) continue;
            task_untrack_user_page(task, removed);
            pfa_cow_release64(removed);
            if (prune_end && address != prune_end) {
                vmm_prune_user_range(task->pml4_phys, prune_start,
                                     prune_end - prune_start);
                prune_start = 0;
                prune_end = 0;
            }
            if (!prune_end)
                prune_start = address;
            prune_end = address + PAGE_SIZE;
            reclaimed++;
        }
    }
    if (prune_end > prune_start)
        vmm_prune_user_range(task->pml4_phys, prune_start,
                             prune_end - prune_start);
    return reclaimed;
}

uint64_t task_reclaim_blocked_file_exec(uint64_t max_pages) {
    task_t *task;
    registers_t *frame;
    uint64_t protected_address;
    uint64_t reclaimed;
    uint64_t step;

    if (max_pages == 0) return 0;
    reclaimed = 0;
    while (reclaimed < max_pages) {
        step = 0;
        lock_scheduler();
        task = all_tasks_head;
        while (task && task_ptr_valid(task)) {
            if (task->state == TASK_BLOCKED && task->is_user &&
                !task->resources_released && task->exec_completed == 0 &&
                task->pml4_phys &&
                !task_pml4_has_multiple_users(task) &&
                !task_pml4_is_current_on_any_cpu(task->pml4_phys) &&
                task_vm_trylock(task)) {
                protected_address = 0;
                if (task->pid != TASK_INIT_PID) {
                    frame = task->syscall_frame;
                    if (frame) protected_address = frame->rip;
                    else protected_address = task->regs.rip;
                    if (protected_address < 0x1000 ||
                        protected_address >= KERNEL_VMA)
                        protected_address = 0;
                }
                step = task_reclaim_file_exec_pages(
                    task, 1, protected_address);
                task_vm_unlock(task);
                if (step) break;
            }
            task = task->all_next;
        }
        unlock_scheduler();
        if (!step) break;
        reclaimed += step;
        if (task_has_pending_signals()) break;
    }
    return reclaimed;
}

uint64_t task_reclaim_current_file_exec(uint64_t max_pages) {
    task_t *task;
    registers_t *frame;
    uint64_t protected_address;
    uint64_t reclaimed;
    int eligible;

    if (max_pages == 0) return 0;
    reclaimed = 0;
    task = current_task;
    frame = task ? task->syscall_frame : NULL;
    eligible = 0;
    lock_scheduler();
    if (task && frame && task->is_user && !task->resources_released &&
        task->exec_completed == 0 && task->pml4_phys &&
        current_task == task && !task_pml4_has_multiple_users(task))
        eligible = 1;
    unlock_scheduler();
    if (eligible && task_vm_trylock(task)) {
        protected_address = frame->rip;
        if (protected_address >= 0x1000 && protected_address < KERNEL_VMA) {
            reclaimed = task_reclaim_file_exec_pages(
                task, max_pages, protected_address);
        }
        task_vm_unlock(task);
    }
    return reclaimed;
}

static uint64_t task_zero_reclaim_cursor_get(task_t *task) {
    uint64_t cursor;

    cursor = 0;
    memcpy(&cursor, task->zero_reclaim_cursor,
           sizeof(task->zero_reclaim_cursor));
    return cursor;
}

static void task_zero_reclaim_cursor_set(task_t *task, uint64_t cursor) {
    memcpy(task->zero_reclaim_cursor, &cursor,
           sizeof(task->zero_reclaim_cursor));
}

uint64_t task_reclaim_zero_anon(task_t *task, uint64_t max_pages) {
    task_file_map_t *area;
    uint64_t address;
    uint64_t area_end;
    uint64_t physical;
    uint64_t flags;
    uint64_t *words;
    uint64_t word;
    uint64_t reclaimed;
    uint64_t scanned;
    uint64_t scan_budget;
    uint64_t cursor;
    uint64_t position;
    uint64_t area_pages;
    uint64_t page_index;
    uint64_t shared_zero;
    uint64_t replacement_flags;
    int file_backed;
    int zero;
    int i;

    if (!task || !task->is_user || max_pages == 0) return 0;
    reclaimed = 0;
    scanned = 0;
    if (max_pages > UINT64_MAX / 4)
        scan_budget = UINT64_MAX;
    else
        scan_budget = max_pages * 4;
    shared_zero = 0;
    cursor = task_zero_reclaim_cursor_get(task);
    position = 0;
    for (i = 0; i < task->vm->file_map_count && reclaimed < max_pages &&
         scanned < scan_budget; i++) {
        area = &task->vm->file_maps[i];
        area_end = area->vaddr + area->memsz;
        file_backed = area->node != NULL &&
                      !(area->map_flags & TASK_VMA_ANONYMOUS);
        if (area_end < area->vaddr ||
            (area->map_flags & TASK_VMA_SHARED) ||
            (!file_backed &&
             !(area->map_flags & TASK_VMA_ANONYMOUS)) ||
            (file_backed && !(area->flags & VMM_PTE_WRITE))) continue;
        area_pages = area->memsz / PAGE_SIZE;
        if (area->memsz % PAGE_SIZE) area_pages++;
        if (area_pages > UINT64_MAX - position) {
            task_zero_reclaim_cursor_set(task, 0);
            return reclaimed;
        }
        if (cursor >= position + area_pages) {
            position += area_pages;
            continue;
        }
        page_index = cursor > position ? cursor - position : 0;
        address = area->vaddr + page_index * PAGE_SIZE;
        for (;
             page_index < area_pages && address < area_end &&
             reclaimed < max_pages &&
             scanned < scan_budget;
             address += PAGE_SIZE, page_index++) {
            scanned++;
            task_zero_reclaim_cursor_set(task, position + page_index + 1);
            physical = vmm_get_phys_in_pml4(task->pml4_phys, address);
            flags = vmm_get_flags_in_pml4(task->pml4_phys, address);
            if (!physical || !(flags & VMM_PTE_PRESENT) ||
                !(flags & VMM_PTE_USER) ||
                (file_backed && !(flags & VMM_PTE_WRITE)) ||
                (flags & (VMM_PTE_COW | VMM_PTE_NOFREE |
                          VMM_PTE_SHARED)))
                continue;
            temp_map_raw(TASK_ANON_RECLAIM_TEMP, physical);
            words = (uint64_t *)TASK_ANON_RECLAIM_TEMP;
            zero = 1;
            for (word = 0; word < PAGE_SIZE / sizeof(uint64_t); word++) {
                if (words[word] != 0) {
                    zero = 0;
                    break;
                }
            }
            temp_unmap_raw(TASK_ANON_RECLAIM_TEMP);
            if (!zero) continue;
            if (!shared_zero) shared_zero = user_zero_page_get();
            if (!shared_zero) break;
            replacement_flags = (flags & ~VMM_PTE_WRITE) | VMM_PTE_NOFREE;
            if (vmm_map_page_in_pml4(task->pml4_phys, address, shared_zero,
                                     replacement_flags) != 0)
                continue;
            if (vmm_get_phys_in_pml4(task->pml4_phys, address) != shared_zero) {
                vmm_map_page_in_pml4(task->pml4_phys, address, physical,
                                     flags);
                continue;
            }
            pfa_cow_release64(physical);
            reclaimed++;
        }
        if (page_index < area_pages && address < area_end)
            return reclaimed;
        position += area_pages;
    }
    if (i >= task->vm->file_map_count) task_zero_reclaim_cursor_set(task, 0);
    return reclaimed;
}

uint64_t task_reclaim_inactive_zero_anon(uint64_t max_pages) {
    task_t *task;
    uint64_t reclaimed;
    uint64_t remaining;

    if (max_pages == 0) return 0;
    reclaimed = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task && task_ptr_valid(task) && reclaimed < max_pages) {
        if (task->is_user && task->state != TASK_DEAD &&
            !task->resources_released && task->pml4_phys &&
            !task_pml4_is_current_on_any_cpu(task->pml4_phys) &&
            task_vm_trylock(task)) {
            remaining = max_pages - reclaimed;
            reclaimed += task_reclaim_zero_anon(task, remaining);
            task_vm_unlock(task);
        }
        task = task->all_next;
    }
    unlock_scheduler();
    return reclaimed;
}

void task_memory_pressure_request(void) {
    __sync_fetch_and_or(&memory_pressure_pending, MEMORY_PRESSURE_REQUESTED);
}

static void task_memory_pressure_reclaim(void) {
    uint64_t reclaimed;

    task_reclaim_exited_now();
    ext4_reclaim_mounted_caches(64);
    overlay_flush_cache();
    squashfs_flush_cache();
    sysfs_reclaim_unused();
    console_reclaim_unused();
    klog_reclaim_unused();
    kstack_reclaim_unused();
    heap_reclaim_unused();
    slab_flush_stale();
    pfa_ref_gc();
    task_reclaim_blocked_file_exec(64);
    task_reclaim_current_file_exec(64);
    reclaimed = task_reclaim_inactive_zero_anon(128);
    task_ksm_merge(256);
    if (reclaimed < 128) {
        lock_scheduler();
        if (current_task && !task_pml4_has_multiple_users(current_task) &&
            task_vm_trylock(current_task)) {
            task_reclaim_zero_anon(current_task, 128 - reclaimed);
            task_vm_unlock(current_task);
        }
        unlock_scheduler();
    }
}

void task_memory_pressure_reclaim_now(void) {
    __sync_lock_test_and_set(&memory_pressure_pending, 0);
    memory_pressure_last_tick = tick_count;
    task_memory_pressure_reclaim();
}

void task_get_memory_stats_for_pml4(task_mem_stats_t *stats, uint64_t current_pml4) {
    task_t *t;
    task_t *d;
    uint64_t user_pt_pages;
    uint64_t heap_pages;
    uint64_t mmap_pages;
    uint64_t file_pages;

    if (!stats) return;
    memset(stats, 0, sizeof(*stats));

    lock_scheduler();
    t = all_tasks_head;
    while (t && task_ptr_valid(t)) {
        stats->task_count++;
        stats->task_struct_bytes += sizeof(task_t);
        if (t->fpu_state) stats->task_fpu_bytes += fpu_state_size;
        if (t->fds && t->fds_capacity > 0)
            stats->task_fd_bytes += (uint64_t)t->fds_capacity *
                                    sizeof(task_fd_t);
        if (t->vm->file_maps && t->vm->file_map_capacity > 0)
            stats->task_file_map_bytes += (uint64_t)t->vm->file_map_capacity *
                                          sizeof(task_file_map_t);
        if (t->is_user && t->state != TASK_DEAD && !t->resources_released) {
            heap_pages = task_heap_pages(t);
            file_pages = task_file_pages(t);
            mmap_pages = task_mmap_pages(t);
            stats->active_user_pages += t->vm->user_pages_count;
            stats->active_heap_pages += heap_pages;
            stats->active_mmap_pages += mmap_pages;
            stats->active_file_pages += file_pages;
            stats->active_stack_pages += task_stack_pages(t);
            if (t->pml4_phys) {
                stats->active_pd_pages++;
                user_pt_pages = vmm_count_user_page_tables(t->pml4_phys);
                stats->active_user_pt_pages += user_pt_pages;
            } else {
                user_pt_pages = 0;
            }
            if ((current_pml4 && t->pml4_phys == current_pml4) || (!current_pml4 && t == current_task)) {
                stats->current_user_pages += t->vm->user_pages_count;
                stats->current_heap_pages += heap_pages;
                stats->current_mmap_pages += mmap_pages;
                stats->current_file_pages += file_pages;
                stats->current_stack_pages += task_stack_pages(t);
                if (t->pml4_phys) {
                    stats->current_pd_pages++;
                    stats->current_user_pt_pages += user_pt_pages;
                }
            }
        }
        t = t->all_next;
    }

    d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->is_user && !d->resources_released) {
            stats->dead_user_pages += d->vm->user_pages_count;
            stats->dead_heap_pages += task_heap_pages(d);
            stats->dead_mmap_pages += task_mmap_pages(d);
            stats->dead_file_pages += task_file_pages(d);
            stats->dead_stack_pages += task_stack_pages(d);
            if (d->pml4_phys) {
                stats->dead_pd_pages++;
                stats->dead_user_pt_pages += vmm_count_user_page_tables(d->pml4_phys);
            }
            stats->dead_exec_old_pages += d->exec_old_pages_count;
        }
        d = d->wait_next;
    }
    unlock_scheduler();

    exec_cleanup_stats(&stats->exec_cleanup_entries, &stats->exec_cleanup_user_pages);
}

void task_get_memory_stats(task_mem_stats_t *stats) {
    task_get_memory_stats_for_pml4(stats, 0);
}

void task_deferred_work(void) {
    cpu_info_t *cpu;
    task_t *task;
    uint64_t usable_pages;
    uint64_t free_pages;
    uint64_t low_watermark;
    uint32_t deferred;
    int cpu_id;

    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : NULL;
    cpu_id = cpu ? (int)(cpu - cpus) : 0;

    deferred = __atomic_exchange_n(&task_deferred_pending, 0,
                                   __ATOMIC_ACQ_REL);
    if (deferred & TASK_DEFERRED_REAP) reap_dead_tasks();
    if (deferred & TASK_DEFERRED_EXEC_DRAIN) exec_cleanup_drain();
    task_timer_check();
    if ((!task || !task->is_user) &&
        cpu_id == 0) {
        klog_drain_console0(32);
        usable_pages = pfa_get_usable_ram_kb() / 4;
        free_pages = pfa_count_free();
        low_watermark = usable_pages / 64;
        if (low_watermark < 64) low_watermark = 64;
        if (low_watermark > 4096) low_watermark = 4096;
        if (((memory_pressure_pending & MEMORY_PRESSURE_REQUESTED) ||
             free_pages < low_watermark) &&
            tick_count - memory_pressure_last_tick >= 25) {
            task_memory_pressure_reclaim_now();
            if (pfa_count_free() < low_watermark)
                task_oom_kill_one();
        }
    }
}

#include "task_priv.h"

task_t* dead_queue_head = NULL;

static void task_wake_parent_waiter_locked(task_t *task) {
    task_t *parent;

    if (!task || task->ppid <= 0) return;
    parent = all_tasks_head;
    while (parent) {
        if (!task_ptr_valid(parent)) break;
        if (parent->pid == task->ppid) {
            if (parent->waiting_for_any_child && parent->state == TASK_BLOCKED) {
                task_mark_ready_locked(parent);
            }
            return;
        }
        parent = parent->all_next;
    }
}

static void task_finish_death_locked(task_t *task) {
    task_t *parent;

    task_debug_exit(task);
    parent = all_tasks_head;
    while (parent) {
        if (!task_ptr_valid(parent)) break;
        if (parent->pid == task->ppid) {
            parent->child_utime += task->utime + task->child_utime;
            parent->child_stime += task->stime + task->child_stime;
            if (parent->child_maxrss_kb < task->maxrss_kb)
                parent->child_maxrss_kb = task->maxrss_kb;
            if (parent->child_maxrss_kb < task->child_maxrss_kb)
                parent->child_maxrss_kb = task->child_maxrss_kb;
            break;
        }
        parent = parent->all_next;
    }
    waitq_wake_all(&task->join_waiters);
    task_wake_parent_waiter_locked(task);
}

int task_has_child_of(pid_t parent_pid, pid_t pgid_filter) {
    int found;
    task_t *t;
    found = 0;

    lock_scheduler();

    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->ppid == parent_pid) {
            if (pgid_filter <= 0 || t->pgid == pgid_filter) {
                found = 1;
                break;
            }
        }
        t = t->all_next;
    }

    unlock_scheduler();
    return found;
}

task_t* task_find_dead_child_of(pid_t parent_pid, pid_t pgid_filter) {
    task_t *d;
    
    lock_scheduler();
    d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->ppid == parent_pid) {
            if (pgid_filter <= 0 || d->pgid == pgid_filter) {
                unlock_scheduler();
                return d;
            }
        }
        d = d->wait_next;
    }
    d = all_tasks_head;
    while (d && task_ptr_valid(d)) {
        if (d->ppid == parent_pid &&
            (pgid_filter <= 0 || d->pgid == pgid_filter) &&
            task_stop_pending(d)) {
            unlock_scheduler();
            return d;
        }
        d = d->all_next;
    }
    unlock_scheduler();
    return NULL;
}

int task_stop_pending(task_t *task) {
    if (!task) return 0;
    if (task_trace_stop_pending(task)) return 1;
    if (!current_task || task->ppid != current_task->pid) return 0;
    return (task->job_event == 1 && task->state == TASK_STOPPED &&
            (current_task->wait_options & 2)) ||
           (task->job_event == 2 && (current_task->wait_options & 8));
}

uint64_t task_take_stop(task_t *task) {
    int event;

    if (task_trace_stop_pending(task))
        return TASK_WAIT_STOPPED | task_trace_take_stop(task);
    event = task->job_event;
    task->job_event = 0;
    if (event == 2) return TASK_WAIT_CONTINUED;
    return TASK_WAIT_STOPPED | task->job_stop_signal;
}

void task_job_stop(int sig) {
    if (!current_task) return;
    lock_scheduler();
    current_task->job_stop_signal = (uint8_t)sig;
    current_task->job_event = 1;
    current_task->state = TASK_STOPPED;
    waitq_wake_all(&current_task->join_waiters);
    task_wake_parent_waiter_locked(current_task);
    unlock_scheduler();
}

int task_job_continue(task_t *task) {
    int stopped;

    if (!task) return 0;
    lock_scheduler();
    stopped = task->job_stop_signal != 0;
    if (stopped) {
        task->job_stop_signal = 0;
        task->job_event = 2;
        if (task->state == TASK_STOPPED) {
            task->state = TASK_BLOCKED;
            task_mark_ready_locked(task);
        }
        waitq_wake_all(&task->join_waiters);
        task_wake_parent_waiter_locked(task);
    }
    unlock_scheduler();
    return stopped;
}

int task_pgrp_orphaned(pid_t pgid) {
    task_t *t;
    task_t *p;
    int orphaned;

    orphaned = 1;
    lock_scheduler();
    for (t = all_tasks_head; t && task_ptr_valid(t) && orphaned;
         t = t->all_next) {
        if (t->pgid != pgid || t->state == TASK_DEAD) continue;
        for (p = all_tasks_head; p && task_ptr_valid(p); p = p->all_next) {
            if (p->pid != t->ppid) continue;
            if (p->state != TASK_DEAD && p->pgid != pgid && p->sid == t->sid)
                orphaned = 0;
            break;
        }
    }
    unlock_scheduler();
    return orphaned;
}

int task_prepare_wait_any_child(pid_t parent_pid, pid_t pgid_filter) {
    task_t *t;
    int found;

    if (!current_task) return -1;
    lock_scheduler();

    if (task_find_dead_child_of(parent_pid, pgid_filter)) {
        unlock_scheduler();
        return 1;
    }
    found = 0;
    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->ppid == parent_pid) {
            if (pgid_filter <= 0 || t->pgid == pgid_filter) {
                found = 1;
                break;
            }
        }
        t = t->all_next;
    }

    if (!found) {
        current_task->join_target = NULL;
        unlock_scheduler();
        return -1;
    }

    current_task->join_target = t;
    current_task->waiting_for_any_child = 1;
    current_task->state = TASK_BLOCKED;
    unlock_scheduler();
    return 0;
}

void task_finish_wait_any_child(void) {
    lock_scheduler();
    if (current_task) {
        current_task->waiting_for_any_child = 0;
        current_task->join_target = NULL;
    }
    unlock_scheduler();
}

void task_exit(uint64_t exit_code) {
    task_exit_deferred(exit_code);
    schedule();
    for (;;) asm volatile ("hlt");
}

void task_exit_deferred(uint64_t exit_code) {
    cpu_info_t *cpu;
    task_t *task;
    task_t *parent;
    uint64_t clear_address;
    uint64_t clear_physical;
    uint64_t clear_key;
    int clear_value;

    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : current_task;
    if (!task) return;
    if (task->clear_child_tid) {
        clear_address = (uint64_t)(uintptr_t)task->clear_child_tid;
        clear_physical = vmm_get_phys_in_pml4(task->pml4_phys,
                                              clear_address);
        clear_value = 0;
        copy_to_user(task->clear_child_tid, &clear_value,
                     sizeof(clear_value));
        task->clear_child_tid = NULL;
        if (clear_physical) {
            clear_key = clear_physical + (clear_address & (PAGE_SIZE - 1));
            task_futex_wake(clear_key, 1);
        }
    }
    syscall_robust_list_exit(task);
    console_release_graphics_owner(task->pid);
    tty_vt_release_owner(task->pid);
    evdev_release_grabs(task->pid);
    lock_scheduler();
    if (task->state == TASK_DEAD) {
        unlock_scheduler();
        return;
    }
    if (__atomic_load_n(&task->exit_code, __ATOMIC_ACQUIRE) & TASK_EXIT_PENDING)
        exit_code = task->exit_code & ~TASK_EXIT_PENDING;
    task->exit_code = exit_code;
    sleepq_remove(task);
    task_remove_wait_locked(task);
    if (task->join_target && !task->waiting_for_any_child) {
        if (task->join_target->join_refs) task->join_target->join_refs--;
        waitq_remove(&task->join_target->join_waiters, task);
    }
    task->join_target = NULL;
    task->waiting_for_any_child = 0;
    task->state = TASK_DEAD;
    remove_task_from_runqueue(task);
    task->wait_next = dead_queue_head;
    dead_queue_head = task;
    task_finish_death_locked(task);
    unlock_scheduler();
    parent = task_find(task->ppid);
    if (parent) deliver_signal_to_task(parent, TASK_SIGCHLD);
    reap_request();
}

void task_free_user_memory(task_t* t) {
    task_vm_t *vm;
    int freed;

    if (!t) return;
    vm = t->vm;
    if (vm != &task_vm_none &&
        __atomic_load_n(&vm->refs, __ATOMIC_ACQUIRE) > 1) {
        if (__atomic_sub_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL) > 0) {
            t->vm = &task_vm_none;
            t->pml4_phys = 0;
            return;
        }
        __atomic_store_n(&vm->refs, 1, __ATOMIC_RELEASE);
    }

    task_clear_file_mappings(t);

    if (t->pml4_phys) {
        freed = task_free_pml4_if_unowned(t, t->pml4_phys);
        if (!freed) return;
        t->pml4_phys = 0;
        pfa_ref_gc();
    }

    vm->user_pages_count = 0;
    if (vm != &task_vm_none) {
        shm_close_vm(vm);
        kfree(vm);
        t->vm = &task_vm_none;
    }
}

static void task_release_exit_resources(task_t *t) {
    int freed;

    if (!t || t->resources_released) return;
    if (task_is_current_on_any_cpu(t) || task_debug_busy(t)) return;
    task_debug_release(t);

    t->syscall_frame = NULL;

    if (t->exec_old_pml4) {
        freed = task_free_exec_old_pml4_if_unowned(t, t->exec_old_pml4);
        if (!freed) return;
        t->exec_old_pml4 = 0;
    }
    t->exec_old_pages_count = 0;
    task_free_user_memory(t);
    if (t->pml4_phys) return;
    t->cr3 = 0;
    t->regs.entry_cr3 = 0;
    t->regs.return_cr3 = 0;
    t->regs.saved_entry_cr3 = 0;
    event_descriptors_close_task(t->pid);
    socket_close_task(t);
    file_locks_release_process(t->pid);
    copy_file_range_release_task(t);
    task_fd_close_all(t);
    spin_lock(&task_fd_array_lock);
    if (t->fds) {
        kfree(t->fds);
        t->fds = NULL;
        t->fds_capacity = 0;
    }
    spin_unlock(&task_fd_array_lock);
    task_affinity_free(t);
    task_free_signal_data(t);
    task_rlimit_free(t);
    posix_timers_release_task(t);
    seccomp_release_task(t);
    vfs_mnt_ns_release(t);
    if (t->task_ext) {
        kfree(t->task_ext);
        t->task_ext = NULL;
    }
    if (t->timer_data) {
        kfree(t->timer_data);
        t->timer_data = NULL;
    }
    creds_release_task(t);
    task_free_cwd(t);
    if (t->exit_code == 137) heap_reclaim_unused();
    t->resources_released = 1;
}

void task_vm_abandon(task_t *t) {
    if (t->vm == &task_vm_none) return;
    if (__atomic_sub_fetch(&t->vm->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        task_clear_file_mappings(t);
        shm_close_vm(t->vm);
        kfree(t->vm);
    }
    t->vm = &task_vm_none;
}

static void task_release_dead_resources(task_t *t) {
    if (!t) return;

    task_release_exit_resources(t);
    if (!t->resources_released) return;
    task_free_fpu_state(t);
    if (t->kernel_stack_base) {
        kstack_free(t->kernel_stack_base);
        t->kernel_stack_base = NULL;
        t->kernel_stack_size = 0;
    }
}

void reap_dead_tasks(void) {
    task_t *t;
    task_t **pp;
    task_t *keep = NULL;
    task_t *next;
    task_t *k;
    task_t *knext;
    int current_on_cpu;
    int retry_needed;

    if (!dead_queue_head) return;
    retry_needed = 0;
    lock_scheduler();
    t = dead_queue_head;
    dead_queue_head = NULL;
    unlock_scheduler();

    while (t && task_ptr_valid(t)) {
        next = t->wait_next;
        t->wait_next = NULL;

        current_on_cpu = task_is_current_on_any_cpu(t);
        if (current_on_cpu || t->join_refs != 0 || t->in_wait_queue ||
            t->refs != 0 ||
            (t->ppid > 0 && !t->waited && task_find(t->ppid))) {
            if (current_on_cpu || t->join_refs != 0 || t->in_wait_queue ||
                t->refs != 0)
                retry_needed = 1;
            if (!current_on_cpu && t != current_task && t->refs == 0) {
                task_release_dead_resources(t);
            }
            t->wait_next = keep;
            keep = t;
            t = next;
            continue;
        }

        task_release_dead_resources(t);
        if (!t->resources_released) {
            retry_needed = 1;
            t->wait_next = keep;
            keep = t;
            t = next;
            continue;
        }

        lock_scheduler();
        if (t->refs != 0) {
            retry_needed = 1;
            t->wait_next = keep;
            keep = t;
            t = next;
            unlock_scheduler();
            continue;
        }
        pp = &all_tasks_head;
        while (*pp) {
            if (*pp == t) {
                *pp = t->all_next;
                break;
            }
            pp = &(*pp)->all_next;
        }
        unlock_scheduler();

        kfree(t);
        t = next;
    }

    if (keep) {
        lock_scheduler();
        k = keep;
        while (k && task_ptr_valid(k)) {
            knext = k->wait_next;
            k->wait_next = dead_queue_head;
            dead_queue_head = k;
            task_wake_parent_waiter_locked(k);
            k = knext;
        }
        unlock_scheduler();
    }
    if (retry_needed)
        reap_request();
}

void reap_request(void) {
    __atomic_fetch_or(&task_deferred_pending, TASK_DEFERRED_REAP,
                      __ATOMIC_RELEASE);
}

void exec_drain_request(void) {
    __atomic_fetch_or(&task_deferred_pending, TASK_DEFERRED_EXEC_DRAIN,
                      __ATOMIC_RELEASE);
}

static void task_reclaim_stale_exec_now(void) {
    task_t *t;
    task_t *owner;
    task_t *scan;
    uint64_t pd;
    int found;
    int freed;

    do {
        owner = NULL;
        pd = 0;
        found = 0;

        lock_scheduler();
        t = all_tasks_head;
        while (t && task_ptr_valid(t)) {
            if (t->is_user && !t->resources_released && !t->exec_completed &&
                t->exec_old_pml4 && t->pml4_phys != t->exec_old_pml4 &&
                !task_is_current_on_any_cpu(t)) {
                owner = t;
                pd = t->exec_old_pml4;
                found = 1;
                break;
            }
            t = t->all_next;
        }
        unlock_scheduler();

        if (found) {
            freed = task_free_exec_old_pml4_if_unowned(owner, pd);
            if (freed) {
                lock_scheduler();
                scan = all_tasks_head;
                while (scan) {
                    if (!task_ptr_valid(scan)) break;
                    if (scan == owner) {
                        if (scan->exec_old_pml4 == pd) {
                            scan->exec_old_pml4 = 0;
                            scan->exec_old_pages_count = 0;
                        }
                        break;
                    }
                    scan = scan->all_next;
                }
                unlock_scheduler();
            } else {
                found = 0;
            }
        }
    } while (found);
}

static void task_reclaim_exited(int reclaim_heap) {
    task_reclaim_stale_exec_now();
    exec_cleanup_drain();
    reap_dead_tasks();
    pfa_ref_gc();
    if (reclaim_heap) heap_reclaim_unused();
    exec_cleanup_drain();
    pfa_ref_gc();
}

void task_reclaim_exited_now(void) {
    task_reclaim_exited(1);
}

void task_kill(task_t* task, uint64_t exit_code) {
    task_t *parent;

    if (!task) return;
    if (task == current_task) {
        task_exit(exit_code);
        return;
    }
    lock_scheduler();
    if (task->state == TASK_DEAD || task_debug_defer_kill(task, exit_code)) {
        unlock_scheduler();
        return;
    }
    if (task->is_user &&
        (task_is_current_on_any_cpu(task) || task->syscall_frame ||
         task->running_cpu < -1)) {
        if (exit_code == 137 ||
            !(__atomic_load_n(&task->exit_code, __ATOMIC_ACQUIRE) & TASK_EXIT_PENDING))
            __atomic_store_n(&task->exit_code, exit_code | TASK_EXIT_PENDING,
                             __ATOMIC_RELEASE);
        if (task->state == TASK_BLOCKED || task->state == TASK_STOPPED) {
            task_remove_wait_locked(task);
            sleepq_remove(task);
            task->state = TASK_BLOCKED;
            task_mark_ready_locked(task);
        }
        unlock_scheduler();
        return;
    }
    syscall_robust_list_exit(task);
    task->exit_code = exit_code;
    sleepq_remove(task);
    task_remove_wait_locked(task);
    if (task->join_target && !task->waiting_for_any_child) {
        if (task->join_target->join_refs) task->join_target->join_refs--;
        waitq_remove(&task->join_target->join_waiters, task);
    }
    task->join_target = NULL;
    task->waiting_for_any_child = 0;
    task->state = TASK_DEAD;
    remove_task_from_runqueue(task);
    task->wait_next = dead_queue_head;
    dead_queue_head = task;
    task_finish_death_locked(task);
    unlock_scheduler();
    parent = task_find(task->ppid);
    if (parent) deliver_signal_to_task(parent, TASK_SIGCHLD);
    reap_request();
}

int task_join(task_t* task, uint64_t* exit_code) {
    extern int task_has_pending_signals(void);

    if (!task || task == current_task) return -1;

    lock_scheduler();
    if (task_stop_pending(task)) {
        if (exit_code) *exit_code = task_take_stop(task);
        unlock_scheduler();
        return 0;
    }
    if (task->state == TASK_DEAD) {
        if (exit_code) *exit_code = task->exit_code;
        unlock_scheduler();
        return 0;
    }

    if (current_task->in_wait_queue || current_task->waiting_queue) {
        unlock_scheduler();
        return -1;
    }
    task->join_refs++;
    current_task->join_target = task;
    waitq_add(&task->join_waiters, current_task);
    current_task->state = TASK_BLOCKED;
    unlock_scheduler();

    for (;;) {
        schedule();

        lock_scheduler();
        if (task->state == TASK_DEAD || task_stop_pending(task)) {
            break;
        }
        if (task_has_pending_signals()) {
            waitq_remove(&task->join_waiters, current_task);
            if (task->join_refs) task->join_refs--;
            current_task->join_target = NULL;
            unlock_scheduler();
            return -KERR_EINTR;
        }
        if (!current_task->waiting_queue) {
            if (task->join_refs) task->join_refs--;
            current_task->join_target = NULL;
            unlock_scheduler();
            return -KERR_EINTR;
        }
        current_task->state = TASK_BLOCKED;
        unlock_scheduler();
    }
    if (exit_code) {
        if (task_stop_pending(task))
            *exit_code = task_take_stop(task);
        else
            *exit_code = task->exit_code;
    }
    if (current_task->waiting_queue) {
        waitq_remove(current_task->waiting_queue, current_task);
    }
    if (task->join_refs) task->join_refs--;
    current_task->join_target = NULL;
    unlock_scheduler();
    return 0;
}

int task_wait_child_pid(pid_t pid, pid_t parent_pid, uint64_t *exit_code) {
    extern int task_has_pending_signals(void);
    task_t *task;
    task_t *wait_target;
    int interrupted;

    if (!current_task || pid <= 0 || parent_pid <= 0) return -KERR_ECHILD;

    for (;;) {
        lock_scheduler();
        task = all_tasks_head;
        while (task && task_ptr_valid(task)) {
            if (task->pid == pid) break;
            task = task->all_next;
        }
        if (!task || !task_ptr_valid(task) || task->ppid != parent_pid) {
            unlock_scheduler();
            return -KERR_ECHILD;
        }
        if (task_stop_pending(task)) {
            if (exit_code) *exit_code = task_take_stop(task);
            unlock_scheduler();
            return 0;
        }
        if (task->state == TASK_DEAD) {
            if (exit_code) *exit_code = task->exit_code;
            task->waited = 1;
            unlock_scheduler();
            return 0;
        }
        if (task_has_pending_signals()) {
            unlock_scheduler();
            return -KERR_EINTR;
        }
        if (current_task->in_wait_queue || current_task->waiting_queue) {
            unlock_scheduler();
            return -KERR_EINTR;
        }
        task->join_refs++;
        current_task->join_target = task;
        waitq_add(&task->join_waiters, current_task);
        if (current_task->waiting_queue != &task->join_waiters) {
            if (task->join_refs) task->join_refs--;
            current_task->join_target = NULL;
            unlock_scheduler();
            return -KERR_EINTR;
        }
        current_task->state = TASK_BLOCKED;
        unlock_scheduler();

        schedule();

        lock_scheduler();
        wait_target = current_task->join_target;
        if (current_task->waiting_queue)
            waitq_remove(current_task->waiting_queue, current_task);
        if (wait_target && wait_target->join_refs)
            wait_target->join_refs--;
        current_task->join_target = NULL;
        interrupted = task_has_pending_signals();
        unlock_scheduler();

        if (interrupted) {
            lock_scheduler();
            task = all_tasks_head;
            while (task && task_ptr_valid(task)) {
                if (task->pid == pid) break;
                task = task->all_next;
            }
            if (task && task_ptr_valid(task) && task->ppid == parent_pid &&
                task_stop_pending(task)) {
                if (exit_code) *exit_code = task_take_stop(task);
                unlock_scheduler();
                return 0;
            }
            if (task && task_ptr_valid(task) && task->ppid == parent_pid &&
                task->state == TASK_DEAD) {
                if (exit_code) *exit_code = task->exit_code;
                task->waited = 1;
                unlock_scheduler();
                return 0;
            }
            unlock_scheduler();
            return -KERR_EINTR;
        }
    }
}

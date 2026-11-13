#include "task_priv.h"

static void task_fork_child_cleanup(task_t *child,
                                    int owns_child_pd, uint64_t child_pd,
                                    uint8_t *kernel_stack_base) {
    task_fd_close_all(child);
    socket_close_task(child);
    task_free_signal_data(child);
    if (child->fds) {
        kfree(child->fds);
        child->fds = NULL;
        child->fds_capacity = 0;
    }
    task_vm_abandon(child);
    creds_release_task(child);
    task_rlimit_free(child);
    seccomp_release_task(child);
    vfs_mnt_ns_release(child);
    task_affinity_free(child);
    if (child->task_ext) kfree(child->task_ext);
    task_free_fpu_state(child);
    task_free_cwd(child);
    kfree(child);
    kstack_free(kernel_stack_base);
    if (owns_child_pd) vmm_free_pml4(child_pd);
}

static pid_t __attribute__((optimize("Oz"))) task_fork_locked(
                registers_t *parent_regs, int share_address_space,
                uint64_t child_stack, uint64_t tls_base,
                int *parent_tid, int *child_tid) {
    uint64_t child_user_pages_count;
    uint64_t child_pd;
    uint8_t* kernel_stack_base;
    uint64_t i;
    uint64_t page;
    uint64_t physical;
    uint64_t page_flags;
    task_file_map_t area;
    int parent_cap;
    uint8_t parent_stage;
    task_t* child;
    task_t* parent;
    registers_t *child_frame;
    registers_t parent_frame;
    pipe_t *child_pipe;
    int owns_child_pd;
    int clone_error;
    pid_t child_pid;

    parent = current_task;
    if (!parent || !parent->is_user || !parent_regs) {
        printf("task_fork: can only fork user tasks\n");
        return -1;
    }
    memcpy(&parent_frame, parent_regs, sizeof(parent_frame));
    if ((parent_frame.cs & 0x3) != 0x3 ||
        parent_frame.rip < 0x1000 || parent_frame.rip >= KERNEL_VMA) {
        return -1;
    }
    child_user_pages_count = 0;
    owns_child_pd = !share_address_space;
    if (share_address_space) {
        child_pd = parent->pml4_phys;
    } else {
        parent_stage = parent->kernel_stage;
        parent->kernel_stage = TASK_KERNEL_STAGE_FORK;
        child_pd = vmm_clone_pml4(parent->pml4_phys,
                                  &child_user_pages_count, &clone_error);
        parent->kernel_stage = parent_stage;
        if (!child_pd) {
            printf("task_fork: failed to clone page directory\n");
            return clone_error;
        }
    }

    child = (task_t*)kmalloc(sizeof(task_t));
    kernel_stack_base = kstack_alloc();
    if (!child || !kernel_stack_base) {
        printf("task_fork: allocation failed\n");
        if (child) kfree(child);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -1;
    }
    memset(child, 0, sizeof(task_t));
    if (share_address_space) task_vm_share(child, parent);
    else child->vm = task_vm_alloc();
    if (!child->vm) {
        kfree(child);
        kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    if (task_copy_cwd(child, parent) != 0) {
        task_vm_abandon(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    if (task_init_fpu_state(child) != 0) {
        printf("task_fork: fpu allocation failed\n");
        task_free_cwd(child);
        task_vm_abandon(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -1;
    }
    if (parent->fpu_state) {
        fpu_save_area(parent->fpu_state);
        memcpy(child->fpu_state, parent->fpu_state, fpu_state_size);
    }

    child->id = task_allocate_id();
    child->pid = (pid_t)child->id;
    child_pid = child->pid;

    if (creds_copy_task(parent, child) != 0) {
        task_free_fpu_state(child);
        task_free_cwd(child);
        task_vm_abandon(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    signals_init_task(child);
    if (task_rlimit_copy(child, parent) != 0) {
        task_free_signal_data(child);
        task_free_fpu_state(child);
        task_free_cwd(child);
        creds_release_task(child);
        task_vm_abandon(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }

    child->ppid = parent->pid;
    child->pgid = parent->pgid ? parent->pgid : parent->pid;
    child->sid = parent->sid ? parent->sid : parent->pid;
    child->creation_mask = parent->creation_mask;

    child->start_tick = tick_count;
    child->state = TASK_READY;
    child->next = NULL;
    child->cr3 = child_pd;
    child->time_slice = SCHED_DEFAULT_TIMESLICE;
    child->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    child->nice_value = parent->nice_value;
    child->sched_policy = parent->sched_policy;
    child->sched_priority = parent->sched_priority;
    child->time_slice = task_timeslice_for_nice(child->nice_value);
    child->base_time_slice = child->time_slice;
    child->stack_size = parent->stack_size;
    child->kernel_stack_base = kernel_stack_base;
    child->kernel_stack_size = KSTACK_USABLE_SIZE;
    child->wake_deadline_ns = 0;
    child->sleep_next = NULL;
    child->in_sleep_queue = 0;
    child->in_wait_queue = 0;
    child->wait_next = NULL;
    child->waiting_queue = NULL;
    child->join_target = NULL;
    waitq_init(&child->join_waiters);
    child->join_refs = 0;
    child->exit_code = 0;
    child->is_user = true;
    child->shares_address_space = false;
    child->sighand_private = false;
    child->syscall_frame = NULL;
    child->pml4_phys = child_pd;
    child->maxrss_kb = child_user_pages_count * 4ULL;
    if (!share_address_space) {
        child->vm->user_pages_count = child_user_pages_count;
        if (parent->vm->file_map_count > 0 &&
            task_ensure_file_map_capacity(child,
                                          parent->vm->file_map_count) != 0) {
            child->vm->user_pages_count = 0;
            task_free_fpu_state(child);
            task_free_cwd(child);
            creds_release_task(child);
            task_rlimit_free(child);
            task_free_signal_data(child);
            task_vm_abandon(child);
            kfree(child);
            kstack_free(kernel_stack_base);
            vmm_free_pml4(child_pd);
            return -KERR_ENOMEM;
        }
        for (i = 0; i < (uint64_t)parent->vm->file_map_count; i++) {
            area = parent->vm->file_maps[i];
            if (area.map_flags & (TASK_VMA_DONTFORK | TASK_VMA_WIPEONFORK)) {
                for (page = area.vaddr; page < area.vaddr + area.memsz;
                     page += PAGE_SIZE) {
                    physical = vmm_unmap_page_in_pml4_flags(child_pd, page,
                                                            &page_flags);
                    if (!physical) continue;
                    task_untrack_user_page(child, physical);
                    if (!(page_flags & VMM_PTE_NOFREE))
                        pfa_cow_release64(physical);
                }
                vmm_prune_user_range(child_pd, area.vaddr, area.memsz);
                if (area.map_flags & TASK_VMA_DONTFORK) continue;
            }
            child->vm->file_maps[child->vm->file_map_count++] = area;
            if (area.node) vfs_open(area.node, 0);
        }
        child->vm->user_brk = parent->vm->user_brk;
        child->vm->user_brk_start = parent->vm->user_brk_start;
        child->vm->mmap_next_addr = parent->vm->mmap_next_addr;
    }
    child->console_id = parent->console_id;
    child->running_cpu = -1;
    child->tls_base = parent->tls_base;
    child->tls_limit = parent->tls_limit;
    child->waiting_for_any_child = 0;
    memcpy(child->name, parent->name, sizeof(child->name));
    parent_cap = parent->fds_capacity;
    if (parent_cap < TASK_INIT_FDS) parent_cap = TASK_INIT_FDS;
    child->fds = (task_fd_t *)kmalloc(parent_cap * sizeof(task_fd_t));
    if (!child->fds) {
        task_free_fpu_state(child);
        task_free_cwd(child);
        creds_release_task(child);
        task_rlimit_free(child);
        task_free_signal_data(child);
        task_vm_abandon(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (owns_child_pd) vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    child->fds_capacity = parent_cap;
    memset(child->fds, 0, parent_cap * sizeof(task_fd_t));
    if (parent->fds && parent->fds_capacity > 0)
        memcpy(child->fds, parent->fds, parent->fds_capacity * sizeof(task_fd_t));
    for (i = 0; i < (uint64_t)child->fds_capacity; i++) {
        if (child->fds[i].in_use && child->fds[i].ref_count > 0) {
            child->fds[i].ref_count = parent->fds[i].ref_count;
        }
        if (child->fds[i].in_use && child->fds[i].type == FD_TYPE_FILE && child->fds[i].node) {
            task_fd_position_share(&parent->fds[i], &child->fds[i]);
            vfs_open((vfs_node_t *)child->fds[i].node, 0);
        }
        if (child->fds[i].in_use && FD_TYPE_IS_PTY(child->fds[i].type)) {
            pty_retain_endpoint((int)(uintptr_t)child->fds[i].private_data);
        }
        if (child->fds[i].in_use && FD_TYPE_IS_PIPE(child->fds[i].type) && child->fds[i].private_data) {
            child_pipe = (pipe_t *)child->fds[i].private_data;
            pipe_retain_reference(child_pipe, child->fds[i].type);
        }
        if (child->fds[i].in_use && child->fds[i].type == FD_TYPE_SOCKET) {
            socket_retain_task_fd(&child->fds[i]);
        }
    }
    child->envp = NULL;
    child->envc = 0;

    child_frame = (registers_t *)((uint8_t *)kernel_stack_base + KSTACK_USABLE_SIZE - sizeof(registers_t));
    memcpy(child_frame, &parent_frame, sizeof(registers_t));
    child_frame->rax = 0;
    child_frame->int_no = 0;
    child_frame->err_code = 0;
    child_frame->entry_cr3 = child_pd;
    child_frame->return_cr3 = child_pd;
    child_frame->saved_entry_cr3 = child_pd;

    child->regs.rsp = (uint64_t)child_frame;
    child->regs.rip = child_frame->rip;
    child->regs.rflags = child_frame->rflags;
    child->regs.cs = child_frame->cs;
    child->regs.ds = child->regs.es = child_frame->ds;
    child->regs.entry_cr3 = child_pd;
    child->regs.return_cr3 = child_pd;
    child->regs.saved_entry_cr3 = child_pd;

    if (share_address_space) {
        child->pml4_phys = parent->pml4_phys;
        child->cr3 = parent->cr3;
        child->shares_address_space = true;
        child->sighand_private = share_address_space == 2;
        child->tls_base = tls_base;
        child->clear_child_tid = child_tid;
        child_frame->rsp = child_stack;
        child_frame->entry_cr3 = parent->pml4_phys;
        child_frame->return_cr3 = parent->pml4_phys;
        child_frame->saved_entry_cr3 = parent->pml4_phys;
        child->regs.entry_cr3 = parent->pml4_phys;
        child->regs.return_cr3 = parent->pml4_phys;
        child->regs.saved_entry_cr3 = parent->pml4_phys;
        if (parent_tid && copy_to_user(parent_tid, &child->pid,
                                       sizeof(child->pid)) < 0) {
            task_fork_child_cleanup(child,
                                    owns_child_pd, child_pd, kernel_stack_base);
            return -KERR_EFAULT;
        }
    } else if (child_stack) {
        child_frame->rsp = child_stack;
    }

    if (signals_copy_task(child, parent) != 0) {
        task_fork_child_cleanup(child,
                                owns_child_pd, child_pd, kernel_stack_base);
        return -KERR_ENOMEM;
    }

    if (!share_address_space && shm_fork_vm(parent->vm, child->vm) != 0) {
        task_fork_child_cleanup(child,
                                owns_child_pd, child_pd, kernel_stack_base);
        return -KERR_ENOMEM;
    }

    if (seccomp_fork(parent, child) != 0) {
        task_fork_child_cleanup(child,
                                owns_child_pd, child_pd, kernel_stack_base);
        return -KERR_ENOMEM;
    }

    if (vfs_mnt_ns_fork(parent, child) != 0) {
        task_fork_child_cleanup(child,
                                owns_child_pd, child_pd, kernel_stack_base);
        return -KERR_ENOMEM;
    }

    if (task_affinity_copy(child, parent) < 0) {
        task_fork_child_cleanup(child,
                                owns_child_pd, child_pd, kernel_stack_base);
        return -KERR_ENOMEM;
    }
    lock_scheduler();
    child->all_next = all_tasks_head;
    all_tasks_head = child;
    add_task_to_runqueue(child);
    unlock_scheduler();

    return child_pid;
}

pid_t task_fork(registers_t *parent_regs, int share_address_space,
                uint64_t child_stack, uint64_t tls_base,
                int *parent_tid, int *child_tid) {
    task_vm_t *vm;
    pid_t result;

    if (!current_task) return -1;
    vm = current_task->vm;
    mutex_lock(&vm->lock);
    result = task_fork_locked(parent_regs, share_address_space, child_stack,
                              tls_base, parent_tid, child_tid);
    mutex_unlock(&vm->lock);
    return result;
}

static pid_t task_create_thread_locked(void *(*entry)(void *), void *arg) {
    uint64_t thread_stack_size;
    uint64_t thread_stack_base;
    uint64_t thread_stack_top;
    uint64_t stack_page_count;
    uint64_t *stack_pages;
    uint64_t zero_val;
    uint64_t arg_val;
    uint64_t *stack_ptr;
    task_t *new_task;
    uint64_t i;
    uint64_t area_end;

    if (!current_task || !current_task->is_user) {
        return -1;
    }
    thread_stack_size = 0x2000;
    thread_stack_base = task_align_up(current_task->vm->user_brk, PAGE_SIZE);
    thread_stack_top = thread_stack_base + thread_stack_size;
    if (thread_stack_base < current_task->vm->user_brk ||
        thread_stack_top < thread_stack_base ||
        thread_stack_top > USER_DYNAMIC_LIMIT - PAGE_SIZE) return -1;
    for (i = 0; i < (uint64_t)current_task->vm->file_map_count; i++) {
        area_end = current_task->vm->file_maps[i].vaddr +
                   current_task->vm->file_maps[i].memsz;
        if (thread_stack_base < area_end &&
            thread_stack_top > current_task->vm->file_maps[i].vaddr) return -1;
    }
    for (i = thread_stack_base; i < thread_stack_top; i += PAGE_SIZE) {
        if (vmm_get_phys_in_pml4(current_task->pml4_phys, i)) return -1;
    }
    
    new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    task_vm_share(new_task, current_task);
    new_task->shares_address_space = true;
    if (task_copy_cwd(new_task, current_task) != 0) goto fail;
    if (task_init_fpu_state(new_task) != 0) goto fail;
    if (creds_copy_task(current_task, new_task) != 0) goto fail;
    
    new_task->kernel_stack_base = kstack_alloc();
    if (!new_task->kernel_stack_base) goto fail;
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    
    new_task->id = task_allocate_id();
    new_task->pid = (pid_t)new_task->id;
    signals_init_task(new_task);
    new_task->state = TASK_READY;
    new_task->is_user = true;
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->nice_value = current_task->nice_value;
    new_task->sched_policy = current_task->sched_policy;
    new_task->sched_priority = current_task->sched_priority;
    new_task->time_slice = task_timeslice_for_nice(new_task->nice_value);
    new_task->base_time_slice = new_task->time_slice;
    if (task_rlimit_copy(new_task, current_task) != 0) goto fail;
    
    new_task->pml4_phys = current_task->pml4_phys;
    new_task->cr3 = current_task->cr3;
    new_task->console_id = current_task->console_id;
    new_task->running_cpu = -1;
    new_task->creation_mask = current_task->creation_mask;
    
    if (!task_memory_allows(current_task, thread_stack_size) ||
        !task_stack_allows(new_task, thread_stack_size)) goto fail;
    if (task_affinity_copy(new_task, current_task) < 0) goto fail;

    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pml4_tracked(current_task->pml4_phys, thread_stack_base, thread_stack_size, 0x7 | VMM_PTE_NX, &stack_page_count);
    if (!stack_pages) goto fail;
    for (i = 0; i < stack_page_count; i++)
        task_track_user_page(current_task, stack_pages[i]);
    kfree(stack_pages);
    new_task->stack_size = thread_stack_size;
    
    current_task->vm->user_brk = thread_stack_top + 0x1000;
    
    zero_val = 0;
    arg_val = (uint64_t)arg;
    vmm_copy_to_pml4(current_task->pml4_phys, thread_stack_top - 16, &zero_val, 8);
    vmm_copy_to_pml4(current_task->pml4_phys, thread_stack_top - 8, &arg_val, 8);
    
    stack_ptr = (uint64_t *)(new_task->kernel_stack_base + KSTACK_USABLE_SIZE);
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = thread_stack_top - 16;
    stack_ptr--;
    *stack_ptr = 0x202;
    stack_ptr--;
    *stack_ptr = 0x1B;
    stack_ptr--;
    *stack_ptr = (uint64_t)entry;
    
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = new_task->cr3;
    stack_ptr--;
    *stack_ptr = new_task->cr3;
    
    new_task->regs.rsp = (uint64_t)stack_ptr;

    new_task->regs.rip = (uint64_t)entry;
    new_task->regs.cs = 0x1B;
    new_task->regs.ds = new_task->regs.es = new_task->regs.ss = 0x23;
    new_task->regs.rflags = 0x202;
    
    lock_scheduler();
    new_task->all_next = all_tasks_head;
    all_tasks_head = new_task;
    add_task_to_runqueue(new_task);
    unlock_scheduler();
    
    return new_task->pid;

fail:
    if (new_task->kernel_stack_base) kstack_free(new_task->kernel_stack_base);
    task_affinity_free(new_task);
    task_rlimit_free(new_task);
    task_free_signal_data(new_task);
    creds_release_task(new_task);
    task_free_fpu_state(new_task);
    task_free_cwd(new_task);
    task_vm_abandon(new_task);
    kfree(new_task);
    return -1;
}

pid_t task_create_thread_with_arg(void *(*entry)(void *), void *arg) {
    task_vm_t *vm;
    pid_t result;

    if (!current_task) return -1;
    vm = current_task->vm;
    mutex_lock(&vm->lock);
    result = task_create_thread_locked(entry, arg);
    mutex_unlock(&vm->lock);
    return result;
}

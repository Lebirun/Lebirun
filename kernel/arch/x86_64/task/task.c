#include "task_priv.h"

_Static_assert(sizeof(task_t) == 920, "task size changed");

task_ext_t *task_ext_get(task_t *task, int create) {
    task_ext_t *e;
    if (!task) return NULL;
    e = (task_ext_t *)task->task_ext;
    if (e || !create) return e;
    e = (task_ext_t *)kmalloc(sizeof(task_ext_t));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    task->task_ext = e;
    return e;
}

spinlock_t sched_lock = {0};
int scheduler_initialized = 0;
task_t* bootstrap_task = NULL;
task_t* all_tasks_head = NULL;
int idle_wake_pending;

int task_set_cwd(task_t *task, const char *cwd) {
    char *copy;
    const char *visible;
    size_t len;
    size_t root_len;

    if (!task || !cwd) return -1;
    visible = cwd;
    if (task->root) {
        root_len = strlen(task->root);
        if (strncmp(cwd, task->root, root_len) == 0 &&
            (cwd[root_len] == '\0' || cwd[root_len] == '/')) {
            visible = cwd + root_len;
            if (visible[0] == '\0') visible = "/";
        }
    }
    if (visible[0] == '/' && visible[1] == '\0') {
        if (task->cwd) kfree(task->cwd);
        task->cwd = NULL;
        return 0;
    }
    len = strlen(visible);
    if (len == SIZE_MAX) return -1;
    copy = (char *)kmalloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, visible, len + 1);
    if (task->cwd) kfree(task->cwd);
    task->cwd = copy;
    return 0;
}

int task_set_root(task_t *task, const char *root) {
    char *copy;
    size_t len;

    if (!task || !root || root[0] != '/') return -1;
    if (root[1] == '\0') {
        if (task->root) kfree(task->root);
        task->root = NULL;
        return 0;
    }
    len = strlen(root);
    if (len == SIZE_MAX) return -1;
    copy = (char *)kmalloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, root, len + 1);
    if (task->root) kfree(task->root);
    task->root = copy;
    return 0;
}

int task_copy_cwd(task_t *task, const task_t *source) {
    int result;

    if (!source) return task_set_cwd(task, "/");
    result = task_set_cwd(task, source->cwd ? source->cwd : "/");
    if (result != 0) return result;
    return task_set_root(task, source->root ? source->root : "/");
}

void task_free_cwd(task_t *task) {
    if (!task) return;
    if (task->cwd) kfree(task->cwd);
    if (task->root) kfree(task->root);
    task->cwd = NULL;
    task->root = NULL;
}

volatile uint32_t task_deferred_pending = 0;
volatile uint64_t task_context_switches = 0;

static uint64_t next_task_id = 1;
static pid_t next_kernel_pid = -1;

task_vm_t task_vm_none = { .refs = 1 };

task_vm_t *task_vm_alloc(void) {
    task_vm_t *vm;

    vm = (task_vm_t *)kmalloc(sizeof(task_vm_t));
    if (!vm) return NULL;
    memset(vm, 0, sizeof(task_vm_t));
    vm->refs = 1;
    mutex_init(&vm->lock);
    return vm;
}

void task_vm_share(task_t *child, task_t *parent) {
    child->vm = parent->vm;
    __atomic_add_fetch(&child->vm->refs, 1, __ATOMIC_ACQ_REL);
}

void task_vm_lock(task_t *t) {
    if (t && t->vm) mutex_lock(&t->vm->lock);
}

int task_vm_trylock(task_t *t) {
    return t && t->vm && mutex_trylock(&t->vm->lock);
}

void task_vm_unlock(task_t *t) {
    if (t && t->vm) mutex_unlock(&t->vm->lock);
}
uint64_t task_allocate_id(void) {
    return __atomic_fetch_add(&next_task_id, 1, __ATOMIC_RELAXED);
}

pid_t getpid(void) {
    return current_task ? (pid_t)current_task->pid : 0;
}

int task_ptr_valid(task_t *t) {
    uint64_t a;

    if (!t) return 0;
    a = (uint64_t)t;
    if ((a & 0xFFFF0000u) == 0xFEFE0000u) return 0;
    if (a < KERNEL_VMA) return 0;
    return 1;
}

task_t* task_find(pid_t pid) {
    task_t *t;

    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->pid == pid) {
            unlock_scheduler();
            return t;
        }
        t = t->all_next;
    }
    unlock_scheduler();
    return NULL;
}

task_t* task_find_retain(pid_t pid) {
    task_t *t;

    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->pid == pid) {
            t->refs++;
            unlock_scheduler();
            return t;
        }
        t = t->all_next;
    }
    unlock_scheduler();
    return NULL;
}

void task_release_ref(task_t *t) {
    if (!t || !task_ptr_valid(t)) return;
    lock_scheduler();
    t->refs--;
    unlock_scheduler();
}

int task_find_from_irq(pid_t pid, task_t **result) {
    task_t *t;

    if (!result) return 0;
    *result = NULL;
    if (!spin_trylock(&sched_lock)) return 0;
    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->pid == pid) {
            *result = t;
            break;
        }
        t = t->all_next;
    }
    spin_unlock(&sched_lock);
    return 1;
}

task_t* task_find_by_pml4(uint64_t pml4_phys) {
    task_t *t;

    if (!pml4_phys) return NULL;
    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->pml4_phys == pml4_phys) {
            return t;
        }
        t = t->all_next;
    }
    return NULL;
}

void KERNEL_INIT init_tasks(void) {
    uint64_t boot_rsp = 0;
    task_t *initial_task;

    initial_task = (task_t*)kmalloc(sizeof(task_t));
    bootstrap_task = initial_task;
    task_set_current(initial_task);
    memset(current_task, 0, sizeof(task_t));
    current_task->vm = &task_vm_none;
    fpu_init_default_state();
    current_task->id = 0;
    current_task->pid = 0;
    strcpy(current_task->name, "idle");
    current_task->state = TASK_RUNNING;
    current_task->running_cpu = 0;
    current_task->cr3 = read_cr3();
    current_task->next = current_task;
    current_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->nice_value = 0;
    current_task->sched_policy = TASK_SCHED_OTHER;
    current_task->sched_priority = 0;
    current_task->stack_size = 0;
    
    current_task->kernel_stack_base = NULL;
    current_task->kernel_stack_size = 0;

    current_task->wake_deadline_ns = 0;
    current_task->sleep_next = NULL;
    current_task->in_sleep_queue = 0;
    current_task->in_wait_queue = 0;
    current_task->wait_next = NULL;
    current_task->waiting_queue = NULL;
    current_task->join_target = NULL;
    waitq_init(&current_task->join_waiters);
    current_task->join_refs = 0;
    current_task->exit_code = 0;
    current_task->syscall_frame = NULL;
    asm volatile ("mov %%rsp, %0" : "=r" (boot_rsp));
    current_task->regs.rsp = boot_rsp;
    
    tss_set_rsp0(boot_rsp);

    current_task->regs.rip = 0;
    current_task->regs.rflags = 0x202;
    current_task->regs.cs = 0x08;
    current_task->regs.ds = current_task->regs.es = 0x10;
    current_task->vring_minor = 0;
    current_task->is_kernel_task = false;
    current_task->affinity64 = 0xFFFFFFFFFFFFFFFFULL;
    current_task->affinity_nwords = -1;
    current_task->vruntime = 0;
    all_tasks_head = current_task;
    current_task->all_next = NULL;

    {
        cpu_info_t *bsp;
        bsp = smp_this_cpu();
        bsp->running_task = current_task;
        bsp->rq_head = current_task;
        bsp->rq_tail = current_task;
        bsp->nr_running = 1;
        bsp->idle_frame = NULL;
        bsp->scheduler_lock_depth = 0;
        bsp->sched_saved_rflags = 0;
        bsp->schedule_force = 0;
    }
    
    scheduler_initialized = 1;
}

task_t* KERNEL_INIT create_task(void (*entry)(void),
                                task_state_t initial_state,
                                bool user_mode) {
    return create_task_with_cr3(entry, initial_state, user_mode, read_cr3());
}

task_t* KERNEL_INIT create_task_with_cr3(void (*entry)(void),
                                         task_state_t initial_state,
                                         bool user_mode, uint64_t cr3) {
    uint8_t *kernel_stack_base;
    uint64_t *krsp;
    uint64_t user_rsp;
    task_t *new_task;
    registers_t *frame;

    if (!entry) return NULL;

    new_task = (task_t*)kmalloc(sizeof(task_t));
    kernel_stack_base = kstack_alloc();
    if (!new_task || !kernel_stack_base) {
        printf("Task alloc fail!\n");
        if (new_task) kfree(new_task);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        return NULL;
    }
    memset(new_task, 0, sizeof(task_t));
    new_task->vm = user_mode ? task_vm_alloc() : &task_vm_none;
    if (!new_task->vm) {
        kfree(new_task);
        kstack_free(kernel_stack_base);
        return NULL;
    }
    if (user_mode && task_set_cwd(new_task, "/") != 0) {
        kfree(new_task->vm);
        kfree(new_task);
        kstack_free(kernel_stack_base);
        return NULL;
    }
    if (user_mode && task_init_fpu_state(new_task) != 0) {
        printf("Task fpu alloc fail!\n");
        task_free_cwd(new_task);
        kfree(new_task->vm);
        kfree(new_task);
        kstack_free(kernel_stack_base);
        return NULL;
    }
    if (user_mode) task_init_fds(new_task);

    lock_scheduler();
    if (user_mode) {
        new_task->id = task_allocate_id();
        new_task->pid = (pid_t)new_task->id;
    } else {
        new_task->id = 0x8000000000000000ULL |
                       (uint64_t)(-(int64_t)next_kernel_pid);
        new_task->pid = next_kernel_pid;
        next_kernel_pid--;
    }
    unlock_scheduler();

    creds_init_task(new_task);
    new_task->creation_mask = 022;
    signals_init_task(new_task);

    new_task->start_tick = tick_count;
    new_task->state = initial_state;
    new_task->next = NULL;
    new_task->cr3 = cr3;
    new_task->console_id = -1; 
    new_task->running_cpu = -1;
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->nice_value = 0;
    new_task->sched_policy = TASK_SCHED_OTHER;
    new_task->sched_priority = 0;
    new_task->stack_size = 0;
    new_task->kernel_stack_base = kernel_stack_base;
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    new_task->wake_deadline_ns = 0;
    new_task->sleep_next = NULL;
    new_task->in_sleep_queue = 0;
    new_task->in_wait_queue = 0;
    new_task->wait_next = NULL;
    new_task->waiting_queue = NULL;
    new_task->join_target = NULL;
    waitq_init(&new_task->join_waiters);
    new_task->join_refs = 0;
    new_task->refs = 0;
    new_task->exit_code = 0;
    new_task->waiting_for_any_child = 0;
    new_task->is_user = user_mode;
    new_task->syscall_frame = NULL;
    new_task->vring_minor = 0;
    new_task->is_kernel_task = false;
    new_task->affinity64 = 0xFFFFFFFFFFFFFFFFULL;
    new_task->affinity_nwords = -1;
    new_task->vruntime = 0;

    if (user_mode) {
        krsp = (uint64_t*)(kernel_stack_base + KSTACK_USABLE_SIZE);
        
        user_rsp = USER_STACK_INIT_ESP;
        
        *--krsp = 0x23;
        *--krsp = user_rsp;
        *--krsp = 0x202;
        *--krsp = 0x1B;
        *--krsp = (uint64_t)entry;
        
        *--krsp = 0;
        *--krsp = 0;
        
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        
        *--krsp = 0x23;
        *--krsp = 0x23;
        *--krsp = cr3;
        *--krsp = cr3;

        new_task->regs.rsp = (uint64_t)krsp;
        new_task->regs.rip = (uint64_t)entry;
        new_task->regs.rflags = 0x202;
        new_task->regs.cs = 0x1B;
        new_task->regs.ds = new_task->regs.es = 0x23;
        new_task->vm->user_brk = 0;
        new_task->vm->user_brk_start = 0;
        new_task->vm->mmap_next_addr = task_random_mmap_base();
        
    } else {
        frame = (registers_t *)(kernel_stack_base + KSTACK_USABLE_SIZE - sizeof(registers_t));
        memset(frame, 0, sizeof(registers_t));
        frame->return_cr3 = cr3;
        frame->entry_cr3 = cr3;
        frame->es = 0x10;
        frame->ds = 0x10;
        frame->int_no = 48;
        frame->rip = (uint64_t)entry;
        frame->cs = 0x08;
        frame->rflags = 0x202;
        frame->rsp = (uint64_t)(kernel_stack_base + KSTACK_USABLE_SIZE);
        frame->ss = 0x10;
        new_task->regs.rsp = (uint64_t)frame;
        new_task->regs.rip = (uint64_t)entry;
        new_task->regs.rflags = 0x202;
        new_task->regs.cs = 0x08;
        new_task->regs.ds = new_task->regs.es = 0x10;
    }

    lock_scheduler();
    new_task->all_next = all_tasks_head;
    all_tasks_head = new_task;
    unlock_scheduler();

    return new_task;
}

task_t* KERNEL_INIT create_kernel_task(void (*entry)(void),
                                       task_state_t initial_state) {
    task_t *t;

    t = create_task(entry, initial_state, false);
    if (!t) return NULL;
    t->is_kernel_task = true;
    return t;
}

bool task_is_kernel_pid(int32_t pid) {
    return pid < 0;
}

void task_set_vring(task_t *task, uint8_t vring_minor) {
    uint64_t stack_base;
    uint64_t stack_end;
    vring_t *ring;

    if (!task) return;
    task->vring_minor = vring_minor;
    task->is_kernel_task = (vring_minor != 0);

    if (vring_minor != 0 && !task->is_user && task->kernel_stack_base && task->kernel_stack_size) {
        stack_base = (uint64_t)task->kernel_stack_base;
        stack_end = stack_base + task->kernel_stack_size;
        vring_add_region(vring_minor, stack_base, stack_end, VRING_PERM_READ | VRING_PERM_WRITE);
    }

    if (vring_minor != 0 && !task->is_user) {
        ring = vring_get(vring_minor);
        if (ring && ring->vring_pml4) {
            task->pml4_phys = ring->vring_pml4;
        }
    }
}

static int cached_proc_count = 1;

void task_update_cached_stats(void) {
    int count;
    int i;

    count = 0;
    for (i = 0; i < cpu_count; i++)
        count += (int)cpus[i].nr_running;
    if (count < 1) count = 1;
    cached_proc_count = count;
}

void task_get_cached_stats(int *proc_count, int *unused1, int *unused2, pid_t *unused3) {
    if (proc_count) *proc_count = cached_proc_count;
    (void)unused1;
    (void)unused2;
    (void)unused3;
}

#include "task_priv.h"

static void task_rq_rebalance_locked(task_t *task);

int task_timeslice_for_nice(int nice_value) {
    int slice;

    slice = SCHED_DEFAULT_TIMESLICE - nice_value / 5;
    if (slice < 1) slice = 1;
    if (slice > 7) slice = 7;
    return slice;
}

static int task_scheduler_rank(task_t *task) {
    if (!task) return -1;
    if (task->sched_policy == TASK_SCHED_DEADLINE)
        return 200;
    if (task->sched_policy == TASK_SCHED_FIFO ||
        task->sched_policy == TASK_SCHED_RR)
        return 100 + task->sched_priority;
    return task->pi_boost;
}

static uint64_t task_affinity_mask(task_t *task) {
    if (!task) return 0;
    return task->affinity64;
}

static int task_affinity_words(void) {
    int w;
    if (cpu_count <= 0) return 1;
    w = (cpu_count + 63) / 64;
    return w > 0 ? w : 1;
}

static uint64_t task_online_cpu_mask(int word) {
    int64_t bits;

    bits = (int64_t)cpu_count - (int64_t)word * 64;
    if (bits <= 0) return 0;
    if (bits >= 64) return UINT64_MAX;
    return (1ULL << bits) - 1;
}

static int task_cpu_allowed(task_t *task, int cpu) {
    uint64_t m;
    int idx;
    if (!task || cpu < 0) return 0;
    if (cpu < 64) {
        m = task_affinity_mask(task);
        return (m & (1ULL << cpu)) != 0;
    }
    if (task->affinity_nwords < 0) return 1;
    idx = (cpu >> 6) - 1;
    if (idx < 0 || idx >= task->affinity_nwords) return 0;
    return (task->affinity_dyn[idx] & (1ULL << (cpu & 63))) != 0;
}

int task_affinity_copy(task_t *dst, task_t *src) {
    uint64_t *nd;
    int words;
    int i;
    if (!dst || !src) return -1;
    nd = NULL;
    words = 0;
    for (;;) {
        lock_scheduler();
        if (src->affinity_nwords <= words) break;
        words = src->affinity_nwords;
        unlock_scheduler();
        if (nd) kfree(nd);
        nd = (uint64_t *)kmalloc((uint64_t)words * sizeof(uint64_t));
        if (!nd) return -1;
    }
    for (i = 0; i < src->affinity_nwords; i++)
        nd[i] = src->affinity_dyn[i];
    dst->affinity64 = src->affinity64;
    dst->affinity_nwords = src->affinity_nwords;
    dst->affinity_dyn = src->affinity_nwords > 0 ? nd : NULL;
    unlock_scheduler();
    if (!dst->affinity_dyn && nd) kfree(nd);
    return 0;
}

void task_affinity_free(task_t *task) {
    if (!task) return;
    if (task->affinity_dyn) {
        kfree(task->affinity_dyn);
        task->affinity_dyn = NULL;
        task->affinity_nwords = 0;
    }
}

static int task_pick_cpu(task_t *task) {
    cpu_info_t *me;
    uint64_t best_load;
    int best;
    int n;
    int i;
    if (!task) return 0;
    me = smp_this_cpu();
    n = cpu_count > 0 ? cpu_count : 1;
    best = -1;
    best_load = (uint64_t)-1;
    for (i = 0; i < n; i++) {
        if (!task_cpu_allowed(task, i))
            continue;
        if (cpus && i < cpu_count && cpus[i].active) {
            if (cpus[i].nr_running < best_load) {
                best_load = cpus[i].nr_running;
                best = i;
            }
        } else if (best < 0) {
            best = i;
        }
    }
    if (best < 0)
        best = me ? smp_processor_id() : 0;
    task->last_cpu = best;
    return best;
}

int task_get_nice(task_t *task) {
    if (!task) return 0;
    return task->nice_value;
}

int task_set_nice(task_t *task, int nice_value) {
    int slice;

    if (!task || nice_value < -20 || nice_value > 19) return -1;
    slice = task_timeslice_for_nice(nice_value);
    lock_scheduler();
    task->nice_value = nice_value;
    task->base_time_slice = slice;
    if (task->time_slice > slice || task->time_slice <= 0) {
        task->time_slice = slice;
    }
    unlock_scheduler();
    return 0;
}

int task_set_scheduler(task_t *task, int policy, int priority) {
    int slice;

    if (!task) return -1;
    if (policy == TASK_SCHED_DEADLINE) {
        if (priority < 0) return -1;
        lock_scheduler();
        task->sched_policy = policy;
        task->sched_priority = 0;
        task->base_time_slice = 1;
        task->time_slice = 1;
        unlock_scheduler();
        return 0;
    }
    if (policy < TASK_SCHED_OTHER || policy > TASK_SCHED_RR)
        return -1;
    if (policy == TASK_SCHED_OTHER && priority != 0) return -1;
    if (policy != TASK_SCHED_OTHER && (priority < 1 || priority > 99))
        return -1;
    slice = policy == TASK_SCHED_RR ? SCHED_DEFAULT_TIMESLICE :
            task_timeslice_for_nice(task->nice_value);
    lock_scheduler();
    task->sched_policy = policy;
    task->sched_priority = priority;
    task->base_time_slice = slice;
    task->time_slice = slice;
    unlock_scheduler();
    return 0;
}

int task_get_scheduler(task_t *task, int *priority) {
    if (!task) return -1;
    if (priority) *priority = task->sched_priority;
    return task->sched_policy;
}

int task_set_cpu_affinity(task_t *task, uint32_t mask) {
    return task_set_cpu_affinity64(task, mask);
}

int task_set_cpu_affinity64(task_t *task, uint64_t mask) {
    return task_set_cpu_affinity_mask(task, &mask, 1);
}

int task_set_cpu_affinity_mask(task_t *task, const uint64_t *words, int nwords) {
    uint64_t *nd;
    uint64_t *old;
    int need;
    int i;
    int any;
    if (!task || !words || nwords <= 0) return -1;
    if (cpu_count <= 0) return -1;
    need = task_affinity_words();
    for (i = need; i < nwords; i++) {
        if (words[i]) return -1;
    }
    if (nwords > need) nwords = need;
    any = 0;
    for (i = 0; i < nwords; i++) {
        if (words[i] & task_online_cpu_mask(i)) {
            any = 1;
            break;
        }
    }
    if (!any) return -1;
    while (nwords > 1 && words[nwords - 1] == 0) nwords--;
    nd = NULL;
    if (nwords > 1) {
        nd = (uint64_t *)kmalloc((uint64_t)(nwords - 1) * sizeof(uint64_t));
        if (!nd) return -1;
        for (i = 1; i < nwords; i++) nd[i - 1] = words[i];
    }
    lock_scheduler();
    old = task->affinity_dyn;
    task->affinity64 = words[0];
    task->affinity_dyn = nd;
    task->affinity_nwords = nwords - 1;
    task_rq_rebalance_locked(task);
    unlock_scheduler();
    if (old) kfree(old);
    return 0;
}

int task_get_cpu_affinity_mask(task_t *task, uint64_t *words, int nwords) {
    int need;
    int w;
    int i;
    if (!task || !words || nwords <= 0) return -1;
    need = task_affinity_words();
    w = nwords > need ? need : nwords;
    lock_scheduler();
    words[0] = task_affinity_mask(task);
    for (i = 1; i < w; i++) {
        if (task->affinity_nwords < 0) words[i] = UINT64_MAX;
        else if (i - 1 < task->affinity_nwords)
            words[i] = task->affinity_dyn[i - 1];
        else words[i] = 0;
    }
    unlock_scheduler();
    for (i = 0; i < w; i++) words[i] &= task_online_cpu_mask(i);
    for (i = w; i < nwords; i++) words[i] = 0;
    return 0;
}

uint64_t task_get_cpu_affinity64(task_t *task) {
    uint64_t mask;
    if (!task) return 0;
    lock_scheduler();
    mask = task_affinity_mask(task);
    unlock_scheduler();
    return mask & task_online_cpu_mask(0);
}

uint32_t task_get_cpu_affinity(task_t *task) {
    uint32_t mask;

    if (!task) return 0;
    lock_scheduler();
    mask = (uint32_t)task_affinity_mask(task);
    unlock_scheduler();
    return mask & (uint32_t)task_online_cpu_mask(0);
}

_Static_assert(KSTACK_RUNTIME_SIZE == 0x1E00, "idle stack layout");
_Static_assert(sizeof(registers_t) <= KSTACK_IDLE_RESERVE, "idle frame size");
_Static_assert(KSTACK_TSS_OFFSET + TSS_CPU_BYTES <= KSTACK_USABLE_SIZE,
               "cpu stack metadata layout");

__attribute__((naked, noreturn)) static void task_cpu_idle_entry(void) {
    __asm__ volatile (
        "movq %gs:48, %rsp\n\t"
        "addq $0x1E00, %rsp\n\t"
        "1:\n\t"
        "call smp_cpu_idle\n\t"
        "jmp 1b"
    );
}

void KERNEL_INIT task_prepare_cpu_idle(struct cpu_info *cpu_ptr) {
    cpu_info_t *cpu;
    registers_t *frame;
    uint64_t cr3;

    cpu = (cpu_info_t *)cpu_ptr;
    if (!cpu || !cpu->kernel_stack) return;
    frame = (registers_t *)((uint8_t *)cpu->kernel_stack +
                            KSTACK_RUNTIME_SIZE);
    memset(frame, 0, sizeof(registers_t));
    cr3 = read_cr3();
    frame->return_cr3 = cr3;
    frame->entry_cr3 = cr3;
    frame->saved_entry_cr3 = cr3;
    frame->es = 0x10;
    frame->ds = 0x10;
    frame->int_no = 48;
    frame->rip = (uint64_t)task_cpu_idle_entry;
    frame->cs = 0x08;
    frame->rflags = 0x2;
    frame->rsp = (uint64_t)frame;
    frame->ss = 0x10;
    cpu->idle_frame = frame;
}

task_t *task_current(void) {
    cpu_info_t *cpu;

    cpu = smp_this_cpu();
    if (cpu)
        return cpu->running_task;
    return bootstrap_task;
}

void task_set_current(task_t *task) {
    cpu_info_t *cpu;
    task_t *old_task;
    int cpu_id;

    cpu = smp_this_cpu();
    if (cpu) {
        cpu_id = (int)(cpu - cpus);
        old_task = cpu->running_task;
        if (old_task && old_task != task &&
            old_task->running_cpu == cpu_id)
            old_task->running_cpu = -1;
        cpu->running_task = task;
        if (task)
            task->running_cpu = cpu_id;
    }
}

static void task_kick_stopped_cpu_locked(task_t *task) {
    int i;

    if (!cpus || !smp_percpu_irq_ready) return;
    for (i = 0; i < cpu_count; i++) {
        if (!cpus[i].active ||
            !__atomic_load_n(&cpus[i].tick_stopped, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&cpus[i].wake_pending, __ATOMIC_ACQUIRE))
            continue;
        if (task && !task_cpu_allowed(task, i)) continue;
        __atomic_store_n(&cpus[i].wake_pending, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&idle_wake_pending, 1, __ATOMIC_RELEASE);
        return;
    }
}

void task_mark_ready_locked(task_t *task) {
    task_t *running;
    int owner;
    int target;

    if (!task || task->state != TASK_BLOCKED) return;
    task->state = TASK_READY;
    if (!smp_scheduling_enabled() || !smp_percpu_irq_ready) return;
    owner = task->running_cpu;
    if (owner == -1) task_kick_stopped_cpu_locked(task);
    if (owner >= -1) return;
    target = -(owner + 2);
    if (target >= cpu_count || !cpus[target].active ||
        &cpus[target] == smp_this_cpu()) return;
    running = cpus[target].running_task;
    if (running && (running->id != 0 || running->is_user)) return;
    __atomic_store_n(&cpus[target].wake_pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&idle_wake_pending, 1, __ATOMIC_RELEASE);
}

void task_kick_idle_cpus(void) {
    int i;

    if (!__atomic_load_n(&idle_wake_pending, __ATOMIC_ACQUIRE)) return;
    if (!__atomic_exchange_n(&idle_wake_pending, 0, __ATOMIC_ACQ_REL)) return;
    for (i = 0; i < cpu_count; i++) {
        if (__atomic_exchange_n(&cpus[i].wake_pending, 0, __ATOMIC_ACQ_REL))
            lapic_send_ipi(cpus[i].lapic_id, IPI_WAKE_VECTOR);
    }
}

static inline void sched_lock_acquire(void) {
    while (!spin_trylock(&sched_lock)) {
        smp_tlb_flush_poll();
        cpu_relax();
    }
}

void lock_scheduler(void) {
    uint64_t eflags;
    cpu_info_t *cpu;

    __asm__ __volatile__ ("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
    cpu = smp_this_cpu();
    if (cpu->scheduler_lock_depth == 0) {
        sched_lock_acquire();
        cpu->sched_saved_rflags = eflags;
    }
    cpu->scheduler_lock_depth++;
}

void unlock_scheduler(void) {
    uint64_t eflags;
    cpu_info_t *cpu;

    cpu = smp_this_cpu();
    cpu->scheduler_lock_depth--;
    if (cpu->scheduler_lock_depth == 0) {
        eflags = cpu->sched_saved_rflags;
        spin_unlock(&sched_lock);
        task_kick_idle_cpus();
        if (eflags & (1 << 9)) {
            __asm__ __volatile__ ("sti" ::: "memory");
        }
    }
}

static void task_rq_insert_locked(task_t *task, int cpu) {
    cpu_info_t *c;

    c = &cpus[cpu];
    if (task->vruntime < c->vruntime_min)
        task->vruntime = c->vruntime_min;
    if (c->rq_head) {
        c->rq_tail->next = task;
        task->next = c->rq_head;
    } else {
        c->rq_head = task;
        task->next = task;
    }
    c->rq_tail = task;
    c->nr_running++;
    task->preferred_cpu = cpu;
}

void add_task_to_runqueue(task_t* new_task) {
    if (!new_task || new_task->next) return;
    task_rq_insert_locked(new_task, task_pick_cpu(new_task));
}

void remove_task_from_runqueue(task_t* task) {
    cpu_info_t *c;
    task_t *prev;

    if (!task || !task->next) return;
    c = &cpus[task->preferred_cpu];
    prev = c->rq_tail;
    if (!prev) return;
    while (prev->next != task) {
        prev = prev->next;
        if (prev == c->rq_tail) return;
    }
    if (prev == task) {
        c->rq_head = NULL;
        c->rq_tail = NULL;
    } else {
        prev->next = task->next;
        if (c->rq_head == task) c->rq_head = task->next;
        if (c->rq_tail == task) c->rq_tail = prev;
    }
    task->next = NULL;
    if (c->nr_running) c->nr_running--;
}

static void task_rq_move_locked(task_t *task, int cpu) {
    uint64_t base;

    if (!task->next || task->preferred_cpu == cpu) return;
    base = cpus[task->preferred_cpu].vruntime_min;
    remove_task_from_runqueue(task);
    task->vruntime = task->vruntime > base ? task->vruntime - base : 0;
    task->vruntime += cpus[cpu].vruntime_min;
    task_rq_insert_locked(task, cpu);
}

static void task_rq_rebalance_locked(task_t *task) {
    if (task && task->next && task->running_cpu == -1 &&
        !task_cpu_allowed(task, task->preferred_cpu))
        task_rq_move_locked(task, task_pick_cpu(task));
}

static task_t *task_find_idle_locked(void) {
    task_t *t;

    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t->id == 0 && !t->is_user && !t->resources_released) return t;
        t = t->all_next;
    }
    return NULL;
}

void switch_to(task_t* next) {
    uint64_t rsp0;
    task_t *prev;

    if (next == current_task) return;

    prev = current_task;
    task_set_current(next);
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->kernel_stack_base && next->kernel_stack_size) {
        rsp0 = (uint64_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_rsp0(rsp0);
    }

    if (next->regs.rsp != 0 && 
        !((next->regs.rsp >= KERNEL_VMA && next->regs.rsp < HEAP_START) ||
          (next->regs.rsp >= HEAP_START && next->regs.rsp < kernel_heap.max_addr) ||
          kstack_is_in_region(next->regs.rsp))) {
        printf("Switch guard: bad rsp 0x%016lX for task %d\n", next->regs.rsp, next->id);
        return;
    }

    prev->cr3 = read_cr3();
    if (prev->fpu_state) fpu_save_area(prev->fpu_state);
    if (next->fpu_state) fpu_restore_area(next->fpu_state);

    switch_to_asm(&prev->regs.rsp, next->regs.rsp);

    next->cr3 = read_cr3();
}

void schedule(void) {
    smp_this_cpu()->schedule_force = 1;
    asm volatile ("int $48");
}

void yield(void) {
    task_deferred_work();
    schedule();
}

void block_current(void) {
    lock_scheduler();
    if (current_task) current_task->state = TASK_BLOCKED;
    unlock_scheduler();
    smp_this_cpu()->schedule_force = 1;
    schedule();
}

void wake_task(task_t* task) {
    if (!task) return;
    lock_scheduler();
    if (task->state == TASK_BLOCKED) {
        task_remove_wait_locked(task);
        task_mark_ready_locked(task);
        sleepq_remove(task);
    }
    unlock_scheduler();
}

static inline void save_irq_frame_into_task(task_t* task, const registers_t* regs, uint64_t regs_ptr, uint64_t entry_cr3) {
    task->regs.return_cr3 = regs->return_cr3;
    task->regs.entry_cr3 = regs->entry_cr3;
    task->regs.es = regs->es;
    task->regs.ds = regs->ds;

    task->regs.r15 = regs->r15;
    task->regs.r14 = regs->r14;
    task->regs.r13 = regs->r13;
    task->regs.r12 = regs->r12;
    task->regs.r11 = regs->r11;
    task->regs.r10 = regs->r10;
    task->regs.r9 = regs->r9;
    task->regs.r8 = regs->r8;
    task->regs.rdi = regs->rdi;
    task->regs.rsi = regs->rsi;
    task->regs.rbp = regs->rbp;
    task->regs.rsp = regs_ptr;
    task->regs.rbx = regs->rbx;
    task->regs.rdx = regs->rdx;
    task->regs.saved_entry_cr3 = regs->saved_entry_cr3;
    task->regs.rcx = regs->rcx;
    task->regs.rax = regs->rax;

    task->regs.int_no = regs->int_no;
    task->regs.err_code = regs->err_code;
    task->regs.rip = regs->rip;
    task->regs.cs = regs->cs;
    task->regs.rflags = regs->rflags;
    task->regs.ss = regs->ss;
    task->cr3 = entry_cr3;
}

static int task_owns_irq_frame(task_t *task, uint64_t rsp) {
    uint64_t base;
    uint64_t top;

    if (!task) return 0;
    if (task == bootstrap_task &&
        (!task->kernel_stack_base ||
         task->kernel_stack_size < sizeof(registers_t)))
        return rsp >= KERNEL_VMA && rsp < HEAP_START;
    if (!task->kernel_stack_base || task->kernel_stack_size < sizeof(registers_t)) return 0;
    base = (uint64_t)task->kernel_stack_base;
    top = base + task->kernel_stack_size;
    if (top < base) return 0;
    if (rsp < base || rsp > top - sizeof(registers_t)) return 0;
    return 1;
}

static int task_irq_return_frame(task_t *task, registers_t **frame_out) {
    registers_t *frame;
    uint64_t rsp;
    uint64_t es_val;
    uint64_t ds_val;
    uint64_t cs_val;
    uint64_t rip_val;
    int valid_es;
    int valid_ds;
    int valid_cs;
    int valid_rip;

    if (frame_out) *frame_out = NULL;
    if (!task) return 0;
    rsp = task->regs.rsp;
    if (rsp == 0) return 0;
    if (!((rsp >= KERNEL_VMA && rsp < HEAP_START) ||
          (rsp >= HEAP_START && rsp < kernel_heap.max_addr) ||
          kstack_is_in_region(rsp))) {
        return 0;
    }
    if (!task_owns_irq_frame(task, rsp)) return 0;
    frame = (registers_t*)rsp;
    es_val = frame->es & 0xFFFF;
    ds_val = frame->ds & 0xFFFF;
    cs_val = frame->cs & 0xFFFF;
    rip_val = frame->rip;
    valid_es = (es_val == 0x10 || es_val == 0x23 || es_val == 0);
    valid_ds = (ds_val == 0x10 || ds_val == 0x23 || ds_val == 0);
    valid_cs = (cs_val == 0x08 || cs_val == 0x1B);
    valid_rip = (cs_val == 0x08 &&
                 rip_val >= (uint64_t)_kernel_text_start &&
                 rip_val < (uint64_t)_kernel_text_end) ||
                (cs_val == 0x1B && task->is_user &&
                 rip_val >= 0x1000 && rip_val < KERNEL_VMA);
    if (!valid_es || !valid_ds || !valid_cs || !valid_rip) return 0;
    if (frame_out) *frame_out = frame;
    return 1;
}

static int cpu_idle_frame_valid(cpu_info_t *cpu, registers_t *frame) {
    uint64_t base;
    uint64_t expected;

    if (!cpu || !cpu->kernel_stack || !frame) return 0;
    base = (uint64_t)cpu->kernel_stack;
    expected = base + KSTACK_RUNTIME_SIZE;
    if (expected < base || (uint64_t)frame != expected) return 0;
    if (frame->es != 0x10 || frame->ds != 0x10) return 0;
    if (frame->rip != (uint64_t)task_cpu_idle_entry) return 0;
    if (frame->cs != 0x08 || frame->rflags != 0x2) return 0;
    return 1;
}

static int task_cpu_available(task_t *task, int cpu_id) {
    int owner;

    if (!task) return 0;
    owner = task->running_cpu;
    if (owner != -1 && owner != -(cpu_id + 2)) return 0;
    if (task_is_current_on_any_cpu(task)) return 0;
    if (owner == -1 && !task_cpu_allowed(task, cpu_id)) return 0;
    return 1;
}

static void task_release_from_cpu(task_t *task, registers_t *frame,
                                  int cpu_id, int preserve_cpu) {
    if (!task || task->running_cpu != cpu_id) return;
    if ((preserve_cpu || task->kernel_cpu_pinned) && frame &&
        (frame->cs & 0x3) == 0)
        task->running_cpu = -(cpu_id + 2);
    else
        task->running_cpu = -1;
}

static int bootstrap_idle_frame_valid(task_t *task, cpu_info_t *cpu,
                                      registers_t *frame) {
    uint64_t address;

    if (!task || task != bootstrap_task || !cpu || !cpu->bsp || !frame)
        return 0;
    address = (uint64_t)frame;
    if (address != task->regs.rsp) return 0;
    if (!task_owns_irq_frame(task, address)) return 0;
    if ((frame->cs & 0xFFFF) != 0x08) return 0;
    if (frame->rip < (uint64_t)_kernel_text_start ||
        frame->rip >= (uint64_t)_kernel_text_end) return 0;
    return 1;
}

typedef struct {
    task_t *task;
    registers_t *frame;
    int rank;
    int spare;
    uint64_t vruntime;
} task_pick_t;

static void task_rq_scan_locked(task_pick_t *pick, task_t *start,
                                task_t *prev, cpu_info_t *cpu, int cpu_id) {
    registers_t *frame;
    task_t *t;
    int rank;

    for (t = start; t; t = t->next == start ? NULL : t->next) {
        if (t->state != TASK_READY || t == prev || t->resources_released)
            continue;
        if (t->running_cpu == -1 && (t->id != 0 || t->is_user))
            pick->spare++;
        if (!task_cpu_available(t, cpu_id) ||
            (!cpu->bsp && t->id == 0 && !t->is_user))
            continue;
        rank = task_scheduler_rank(t);
        if ((!pick->task || rank > pick->rank ||
             (rank == pick->rank && t->sched_policy == TASK_SCHED_OTHER &&
              t->vruntime < pick->vruntime)) &&
            task_irq_return_frame(t, &frame)) {
            pick->task = t;
            pick->frame = frame;
            pick->rank = rank;
            pick->vruntime = t->vruntime;
            if (rank == 200) return;
        }
    }
}

registers_t* schedule_from_irq(registers_t* regs) {
    uint64_t entry_cr3;
    uint64_t kernel_cr3;
    int must_switch;
    int dead_switch;
    bool is_idle;
    uint64_t rsp0;
    int got_lock;
    int run_deferred;
    task_t* prev_task;
    task_t* next;
    task_t* start;
    registers_t* return_frame;
    task_pick_t pick;
    cpu_info_t *this_cpu;
    registers_t *result;
    int selectable;
    int forced_state_switch;
    task_t *idle_task;
    int switch_to_cpu_idle;
    int cpu_id;
    task_t *candidate;
    int best_rank;
    int forced_reschedule;
    int migrate;
    int spare;
    int i;

    idle_task = NULL;
    switch_to_cpu_idle = 0;
    this_cpu = smp_this_cpu();
    if (!cpus || !this_cpu) return regs;
    cpu_id = (int)(this_cpu - cpus);
    if (cpu_id < 0 || cpu_id >= cpu_count) return regs;
    if (this_cpu->scheduler_lock_depth == 0)
        descriptor_ready_try_drain_irq();
    if (!scheduler_initialized || !smp_scheduling_enabled()) return regs;

    prev_task = this_cpu->running_task;
    migrate = prev_task && prev_task->state == TASK_RUNNING &&
              (regs->cs & 3) == 3 &&
              (cpu_id < 64 ?
               !(__atomic_load_n(&prev_task->affinity64, __ATOMIC_RELAXED) &
                 (1ULL << cpu_id)) :
               __atomic_load_n(&prev_task->affinity_nwords, __ATOMIC_RELAXED) >= 0);
    forced_reschedule = this_cpu->schedule_force;
    if (prev_task && prev_task->kernel_cpu_pinned &&
        prev_task->state == TASK_RUNNING && regs->int_no != 48) {
        return regs;
    }
    run_deferred = 0;
    forced_state_switch = prev_task &&
                          (prev_task->state == TASK_DEAD ||
                           prev_task->state == TASK_BLOCKED ||
                           prev_task->state == TASK_STOPPED || migrate);

    if (this_cpu->scheduler_lock_depth > 0) return regs;

    if (!forced_reschedule && !forced_state_switch && prev_task &&
        prev_task->state == TASK_RUNNING && prev_task->is_user &&
        (regs->cs & 3) == 3 && !prev_task->syscall_frame &&
        prev_task->sched_policy != TASK_SCHED_FIFO && prev_task->time_slice > 1 &&
        !__atomic_load_n(&task_deferred_pending, __ATOMIC_ACQUIRE)) {
        prev_task->time_slice--;
        watchdog_kick();
        return regs;
    }

    got_lock = spin_trylock(&sched_lock);
    if (!got_lock) {
        if (!forced_state_switch) return regs;
        sched_lock_acquire();
    }
    migrate = prev_task && prev_task->state == TASK_RUNNING &&
              (regs->cs & 3) == 3 &&
              !task_cpu_allowed(prev_task, cpu_id);
    descriptor_ready_drain_irq_locked();
    result = regs;

    entry_cr3 = regs->entry_cr3;
    if (!entry_cr3) {
        __asm__ volatile ("mov %%cr3, %0" : "=r"(entry_cr3));
    }
    regs->entry_cr3 = entry_cr3;
    regs->return_cr3 = entry_cr3;
    kernel_cr3 = vmm_get_kernel_cr3();
    
    dead_switch = prev_task && prev_task->state == TASK_DEAD;
    must_switch = prev_task &&
                  (dead_switch || prev_task->state == TASK_BLOCKED ||
                   prev_task->state == TASK_STOPPED || migrate);
    if (migrate) prev_task->state = TASK_READY;

    if (prev_task && regs->int_no != 48 && prev_task->syscall_frame && !must_switch) {
        goto out_restore_cr3;
    }
    watchdog_kick();

    if (prev_task && !dead_switch) {
        save_irq_frame_into_task(prev_task, regs, (uint64_t)regs, entry_cr3);
        if (prev_task == bootstrap_task && this_cpu->bsp)
            this_cpu->idle_frame = regs;
    }

    is_idle = !prev_task || (prev_task->id == 0 && !prev_task->is_user);
    
    if (prev_task && !this_cpu->schedule_force && !must_switch && !is_idle &&
        prev_task->sched_policy != TASK_SCHED_FIFO) {
        if (prev_task->time_slice > 0) prev_task->time_slice--;
        if (prev_task->time_slice != 0) {
            goto out_restore_cr3;
        }
    }
    this_cpu->schedule_force = 0;

    if (prev_task && !must_switch) {
        prev_task->time_slice = prev_task->base_time_slice;
    }

    pick.task = NULL;
    pick.frame = NULL;
    pick.rank = -1;
    pick.vruntime = 0;
    pick.spare = 0;
    start = this_cpu->rq_head;
    if (!must_switch && prev_task && prev_task->next &&
        prev_task->preferred_cpu == cpu_id)
        start = prev_task->next;
    task_rq_scan_locked(&pick, start, prev_task, this_cpu, cpu_id);
    spare = pick.spare;
    for (i = 1; !pick.task && i < cpu_count; i++)
        task_rq_scan_locked(&pick, cpus[(cpu_id + i) % cpu_count].rq_head,
                            prev_task, this_cpu, cpu_id);
    candidate = pick.task;
    return_frame = pick.frame;
    best_rank = pick.rank;
    if (candidate) task_rq_move_locked(candidate, cpu_id);
    next = candidate;
    if (spare > (next ? 1 : 0)) task_kick_stopped_cpu_locked(NULL);
    if (next && next->sched_policy == TASK_SCHED_OTHER) {
        uint64_t weight = 1024;
        int nice = next->nice_value;
        if (nice < -20) nice = -20;
        if (nice > 19) nice = 19;
        if (nice > 0) weight = 1024 / (1 + (uint64_t)nice);
        else if (nice < 0) weight = 1024 + (uint64_t)(-nice) * 64;
        next->vruntime += 1024 * 1000 / (weight ? weight : 1);
        if (next->vruntime > this_cpu->vruntime_min)
            this_cpu->vruntime_min = next->vruntime;
    }
    if (next && prev_task && !must_switch &&
        !forced_reschedule &&
        prev_task->sched_policy == TASK_SCHED_FIFO &&
        task_scheduler_rank(prev_task) >= best_rank) {
        goto out_restore_cr3;
    }

    if (!next && must_switch) {
        if (this_cpu->bsp)
            idle_task = task_find_idle_locked();
        selectable = 0;
        if (idle_task == prev_task && is_idle) {
            return_frame = regs;
            selectable = 1;
        } else if (idle_task &&
                   !task_is_current_on_any_cpu(idle_task)) {
            selectable = task_irq_return_frame(idle_task, &return_frame);
            if (!selectable &&
                bootstrap_idle_frame_valid(idle_task, this_cpu,
                                           this_cpu->idle_frame)) {
                return_frame = this_cpu->idle_frame;
                selectable = 1;
            }
        }
        if (idle_task && selectable) {
            sleepq_remove(idle_task);
            idle_task->wake_deadline_ns = 0;
            idle_task->state = TASK_READY;
            add_task_to_runqueue(idle_task);
            next = idle_task;
        } else if (!this_cpu->bsp &&
                   cpu_idle_frame_valid(this_cpu, this_cpu->idle_frame)) {
            return_frame = this_cpu->idle_frame;
            switch_to_cpu_idle = 1;
        }
    }

    if (!next && must_switch && !switch_to_cpu_idle) {
        spin_unlock(&sched_lock);
        kernel_panic_custom("SCHEDULER IDLE FAILURE",
                            "cpu=%d prev=%d state=%d idle=%p rsp=0x%lX",
                            cpu_id, prev_task->id, prev_task->state,
                            idle_task, idle_task ? idle_task->regs.rsp : 0);
    }
    
    if (!next && !switch_to_cpu_idle) {
        goto out_restore_cr3;
    }

    if (prev_task && !dead_switch && prev_task->fpu_state)
        fpu_save_area(prev_task->fpu_state);
    if (switch_to_cpu_idle) {
        if (!this_cpu->switching_task)
            __atomic_store_n(&this_cpu->switching_task, prev_task, __ATOMIC_RELEASE);
        task_release_from_cpu(prev_task, regs, cpu_id, must_switch);
        task_rq_rebalance_locked(prev_task);
        this_cpu->running_task = NULL;
        this_cpu->irq_cr3 = 0;
        return_frame->entry_cr3 = kernel_cr3;
        return_frame->return_cr3 = kernel_cr3;
        rsp0 = (uint64_t)this_cpu->kernel_stack + KSTACK_RUNTIME_SIZE;
        tss_set_rsp0(rsp0);
        result = return_frame;
        spin_unlock(&sched_lock);
        task_kick_idle_cpus();
        return result;
    }
    if (next->fpu_state) fpu_restore_area(next->fpu_state);

    if (next != prev_task) {
        task_context_switches++;
        if (prev_task && prev_task->is_user) {
            if (must_switch && prev_task->state == TASK_BLOCKED)
                prev_task->voluntary_context_switches++;
            else
                prev_task->involuntary_context_switches++;
        }
    }

    if (!this_cpu->switching_task)
        __atomic_store_n(&this_cpu->switching_task, prev_task, __ATOMIC_RELEASE);
    if (prev_task && prev_task->state == TASK_RUNNING) {
        prev_task->state = TASK_READY;
        task_kick_stopped_cpu_locked(prev_task);
    }
    task_release_from_cpu(prev_task, regs, cpu_id, must_switch);
    task_rq_rebalance_locked(prev_task);
    next->state = TASK_RUNNING;
    task_set_current(next);
    if (this_cpu->bsp && (next->id != 0 || next->is_user))
        smp_tick_resume();

    if (next->is_user && ((return_frame->cs & 0x3) == 0x3)) {
        return_frame->ds = return_frame->es = 0x23;
        return_frame->cs = 0x1B;
        return_frame->ss = 0x23;
    }

    if (next->kernel_stack_base && next->kernel_stack_size) {
        rsp0 = (uint64_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_rsp0(rsp0);
    }

    if (next->is_user) {
        task_write_fs_base(next->tls_base);
    }

    {
        uint64_t target_cr3 = next->cr3;
        int sandboxed_cr3 = 0;
        vring_t *ring = NULL;
        if (next->pml4_phys) target_cr3 = next->pml4_phys;
        if (next->vring_minor != 0) {
            ring = vring_get(next->vring_minor);
            if (ring && ring->vring_pml4 == target_cr3) sandboxed_cr3 = 1;
        }
        this_cpu->irq_cr3 = sandboxed_cr3 ? kernel_cr3 : 0;
        return_frame->entry_cr3 = target_cr3;
        return_frame->return_cr3 = target_cr3;
    }

    spin_unlock(&sched_lock);
    task_kick_idle_cpus();
    return return_frame;

out_restore_cr3:
    regs->return_cr3 = entry_cr3;
    if (!this_cpu->switching_task &&
        __atomic_load_n(&task_deferred_pending, __ATOMIC_ACQUIRE) &&
        (!current_task || current_task->state != TASK_DEAD)) {
        run_deferred = 1;
    }
    spin_unlock(&sched_lock);
    task_kick_idle_cpus();
    if (run_deferred) {
        task_deferred_work();
    }
    return result;
}

static int task_cpu_has_work_locked(int cpu_id) {
    task_t *start;
    task_t *t;
    int i;

    for (i = 0; i < cpu_count; i++) {
        start = cpus[(cpu_id + i) % cpu_count].rq_head;
        for (t = start; t; t = t->next == start ? NULL : t->next)
            if (t->state == TASK_READY && !t->resources_released &&
                (t->id != 0 || t->is_user) && task_cpu_available(t, cpu_id))
                return 1;
    }
    return 0;
}

int task_cpu_has_work(int cpu_id) {
    int work;

    if (!spin_trylock(&sched_lock)) return 1;
    work = task_cpu_has_work_locked(cpu_id);
    spin_unlock(&sched_lock);
    return work;
}

void task_cpu_tick_stop(void) {
    cpu_info_t *cpu;
    int cpu_id;
    int work;

    cpu = smp_this_cpu();
    if (!cpus || !cpu || cpu->bsp || cpu->running_task ||
        cpu->scheduler_lock_depth || !smp_scheduling_enabled() ||
        !smp_percpu_irq_ready)
        return;
    cpu_id = (int)(cpu - cpus);
    cpu->tick_stop_tick = tick_count;
    __atomic_store_n(&cpu->tick_stopped, 1, __ATOMIC_SEQ_CST);
    sched_lock_acquire();
    work = task_cpu_has_work_locked(cpu_id);
    spin_unlock(&sched_lock);
    if (work) {
        __atomic_store_n(&cpu->tick_stopped, 0, __ATOMIC_RELEASE);
        return;
    }
    smp_tick_stop();
}

void set_syscall_frame(registers_t *frame) {
    if (current_task) {
        current_task->syscall_frame = frame;
    }
}

void clear_syscall_frame(void) {
    if (current_task) {
        current_task->syscall_frame = NULL;
    }
}

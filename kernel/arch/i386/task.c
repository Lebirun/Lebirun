#include <kernel/task.h>
#include <kernel/debug.h>
#include <kernel/registers.h>
#include <kernel/common.h>
#include <kernel/mem_map.h>
#include <kernel/gdt.h>
#include <kernel/tty.h>
#include <string.h>
#include <stdio.h>

static int scheduler_lock = 0;
extern volatile uint32_t tick_count;

extern void switch_to_asm(uint32_t* old_esp, uint32_t new_esp);

task_t* current_task;
task_t* ready_queue_head;
static task_t* sleep_queue_head = NULL;
static task_t* dead_queue_head = NULL;
static volatile int schedule_force = 0;

#define TASK_STACK_SIZE 16384
#define SCHED_DEFAULT_TIMESLICE 5
#define KERNEL_STACK_SIZE 4096

#define USER_STACK_TOP 0x00800000u

static void sleepq_remove(task_t* t) {
    if (!t || !t->in_sleep_queue) return;
    task_t** ptr = &sleep_queue_head;
    while (*ptr) {
        if (*ptr == t) {
            *ptr = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleep_queue = 0;
            return;
        }
        ptr = &(*ptr)->sleep_next;
    }
}

pid_t getpid(void) {
    return current_task ? (pid_t)current_task->pid : 0;
}

task_t* task_find(pid_t pid) {
    lock_scheduler();
    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->pid == pid) {
                unlock_scheduler();
                return t;
            }
            t = t->next;
        } while (t && t != ready_queue_head);
    }

    task_t* d = dead_queue_head;
    while (d) {
        if (d->pid == pid) {
            unlock_scheduler();
            return d;
        }
        d = d->wait_next;
    }
    unlock_scheduler();
    return NULL;
}

void waitq_init(wait_queue_t* q) {
    if (!q) return;
    q->head = NULL;
}

void waitq_add(wait_queue_t* q, task_t* t) {
    if (!q || !t) return;
    if (t->in_wait_queue || t->waiting_queue) return;
    t->wait_next = q->head;
    q->head = t;
    t->in_wait_queue = 1;
    t->waiting_queue = q;
}

task_t* waitq_pop(wait_queue_t* q) {
    if (!q || !q->head) return NULL;
    task_t* t = q->head;
    while (t && t->state == TASK_DEAD) {
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        t = q->head;
    }
    if (!t) return NULL;
    q->head = t->wait_next;
    t->wait_next = NULL;
    t->in_wait_queue = 0;
    t->waiting_queue = NULL;
    return t;
}

void waitq_wake_all(wait_queue_t* q) {
    if (!q) return;
    task_t* t;
    while ((t = waitq_pop(q))) {
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
    }
}

void waitq_remove(wait_queue_t* q, task_t* t) {
    if (!q || !t || !q->head) return;
    if (q->head == t) {
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        return;
    }
    task_t* prev = q->head;
    task_t* cur = prev->wait_next;
    while (cur) {
        if (cur == t) {
            prev->wait_next = cur->wait_next;
            cur->wait_next = NULL;
            cur->in_wait_queue = 0;
            cur->waiting_queue = NULL;
            return;
        }
        prev = cur;
        cur = cur->wait_next;
    }
}

void init_tasks(void) {
    current_task = (task_t*)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));
    current_task->id = 0;
    current_task->pid = 0;
    current_task->state = TASK_RUNNING;
    current_task->regs.cr3 = read_cr3();
    current_task->cr3 = read_cr3();
    current_task->next = NULL;
    current_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->stack_base = NULL;
    current_task->stack_size = 0;
    current_task->kernel_stack_base = NULL;
    current_task->kernel_stack_size = 0;
    current_task->wake_tick = 0;
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
    uint32_t boot_esp = 0;
    asm volatile ("movl %%esp, %0" : "=r" (boot_esp));
    current_task->regs.esp = boot_esp;
    tss_set_esp0(boot_esp);

    current_task->regs.eip = 0;
    current_task->regs.eflags = 0x202;
    current_task->regs.cs = 0x08;
    current_task->regs.ds = current_task->regs.es = current_task->regs.fs = current_task->regs.gs = 0x10;
    ready_queue_head = NULL;
}

void lock_scheduler(void) {
    asm volatile ("cli");
    scheduler_lock++;
}

void unlock_scheduler(void) {
    if (--scheduler_lock == 0) {
        asm volatile ("sti");
    }
}

static inline void add_task_to_runqueue(task_t* new_task) {
    if (!ready_queue_head) {
        ready_queue_head = current_task;
        current_task->next = new_task;
        new_task->next = current_task;
    } else {
        task_t* tail = ready_queue_head;
        while (tail->next != ready_queue_head) tail = tail->next;
        tail->next = new_task;
        new_task->next = ready_queue_head;
    }
}

static inline void remove_task_from_runqueue(task_t* task) {
    if (!ready_queue_head) return;
    if (task == current_task && task == ready_queue_head && task->next == task) {
        ready_queue_head = NULL;
        return;
    }
    task_t* prev = ready_queue_head;
    task_t* cur = ready_queue_head;
    do {
        if (cur == task) break;
        prev = cur;
        cur = cur->next;
    } while (cur != ready_queue_head);
    if (cur != task) return;
    prev->next = cur->next;
    if (cur == ready_queue_head) ready_queue_head = cur->next;
}

task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode) {
    return create_task_with_cr3(entry, initial_state, user_mode, read_cr3());
}

task_t* create_task_with_cr3(void (*entry)(void), task_state_t initial_state, bool user_mode, uint32_t cr3) {
    if (!entry) return NULL;

    printf("create_task_with_cr3: verify heap before alloc\n");
    heap_verify();

    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    uint8_t* stack_base = user_mode ? NULL : (uint8_t*)kmalloc(TASK_STACK_SIZE);
    uint8_t* kernel_stack_base = (uint8_t*)kmalloc(KERNEL_STACK_SIZE);
    if (!new_task || (!user_mode && !stack_base) || !kernel_stack_base) {
        printf("Task alloc fail!\n");
        heap_verify();
        if (new_task) kfree(new_task);
        if (stack_base) kfree(stack_base);
        if (kernel_stack_base) kfree(kernel_stack_base);
        return NULL;
    }
    memset(new_task, 0, sizeof(task_t));
    if (stack_base) memset(stack_base, 0, TASK_STACK_SIZE);
    memset(kernel_stack_base, 0, KERNEL_STACK_SIZE);

    static uint32_t next_id = 1;
    new_task->id = next_id;
    new_task->pid = next_id;
    next_id++;
    new_task->state = initial_state;
    new_task->next = NULL;
    new_task->regs.cr3 = cr3;
    new_task->cr3 = cr3;
    printf("create_task_with_cr3: new task id=%u cr3=0x%08X\n", new_task->id, cr3);
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->stack_base = stack_base;
    new_task->stack_size = TASK_STACK_SIZE;
    new_task->kernel_stack_base = kernel_stack_base;
    new_task->kernel_stack_size = KERNEL_STACK_SIZE;
    new_task->wake_tick = 0;
    new_task->sleep_next = NULL;
    new_task->in_sleep_queue = 0;
    new_task->in_wait_queue = 0;
    new_task->wait_next = NULL;
    new_task->waiting_queue = NULL;
    new_task->join_target = NULL;
    waitq_init(&new_task->join_waiters);
    new_task->join_refs = 0;
    new_task->exit_code = 0;
    new_task->is_user = user_mode;
    new_task->syscall_frame = NULL;

    if (user_mode) {
        uint32_t* kesp = (uint32_t*)(kernel_stack_base + KERNEL_STACK_SIZE);
        
        uint32_t user_esp = USER_STACK_TOP - 16;
        
        *--kesp = 0x23;
        *--kesp = user_esp;
        *--kesp = 0x202;
        *--kesp = 0x1B;
        *--kesp = (uint32_t)entry;
        
        *--kesp = 0;
        *--kesp = 0;
        
        *--kesp = 0;
        *--kesp = 0;
        *--kesp = 0;
        *--kesp = 0;
        *--kesp = 0;
        *--kesp = 0;
        *--kesp = 0;
        *--kesp = 0;
        
        *--kesp = 0x23;
        *--kesp = 0x23;
        *--kesp = 0x23;
        *--kesp = 0x23;

        new_task->regs.esp = (uint32_t)kesp;
        new_task->regs.eip = (uint32_t)entry;
        new_task->regs.eflags = 0x202;
        new_task->regs.cs = 0x1B;
        new_task->regs.ds = new_task->regs.es = new_task->regs.fs = new_task->regs.gs = 0x23;
        
        registers_t* frame = (registers_t*)kesp;
        printf("User task frame at 0x%08X:\n", (uint32_t)kesp);
        printf("  EIP=0x%08X CS=0x%04X EFLAGS=0x%08X\n", frame->eip, frame->cs, frame->eflags);
        printf("  useresp=0x%08X SS=0x%04X\n", frame->useresp, frame->ss);
        printf("  DS=0x%04X ES=0x%04X FS=0x%04X GS=0x%04X\n", frame->ds, frame->es, frame->fs, frame->gs);
    } else {
        uint32_t* esp = (uint32_t*)(stack_base + TASK_STACK_SIZE);
        *--esp = 0x202;
        *--esp = 0x08;
        *--esp = (uint32_t)entry;
        *--esp = 0;
        *--esp = 48;
        *--esp = 0; *--esp = 0; *--esp = 0; *--esp = 0; *--esp = 0;
        *--esp = 0; *--esp = 0; *--esp = 0;
        *--esp = 0x10; *--esp = 0x10; *--esp = 0x10; *--esp = 0x10;
        new_task->regs.esp = (uint32_t)esp;
        new_task->regs.eip = (uint32_t)entry;
        new_task->regs.eflags = 0x202;
        new_task->regs.cs = 0x08;
        new_task->regs.ds = new_task->regs.es = new_task->regs.fs = new_task->regs.gs = 0x10;
    }

    lock_scheduler();
    add_task_to_runqueue(new_task);
    unlock_scheduler();

    DPRINTF2("Task created: id=%u pid=%u EIP=0x%08X ESP=0x%08X ptr=0x%08X%s\n", new_task->id, new_task->pid, (uint32_t)entry, new_task->regs.esp, (uint32_t)new_task, user_mode ? " (USER)" : "");

    return new_task;
}

void sleep_ticks(uint32_t ticks) {
    if (!current_task || ticks == 0) return;
    lock_scheduler();
    uint32_t new_wake = tick_count + ticks;
    if (current_task->in_sleep_queue) {
        current_task->wake_tick = new_wake;
        unlock_scheduler();
        schedule();
        return;
    }
    current_task->wake_tick = new_wake;
    current_task->state = TASK_BLOCKED;
    current_task->in_sleep_queue = 1;
    current_task->sleep_next = sleep_queue_head;
    sleep_queue_head = current_task;
    unlock_scheduler();
    schedule();
}

void wake_sleeping_tasks(void) {
    if (!sleep_queue_head) return;
    lock_scheduler();
    task_t** ptr = &sleep_queue_head;
    int safety = 0;
    while (*ptr) {
        if (++safety > 10000) {
            *ptr = NULL;
            break;
        }
        task_t* t = *ptr;
        if (t->wake_tick <= tick_count) {
            *ptr = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleep_queue = 0;
            if (t->state == TASK_BLOCKED) t->state = TASK_READY;
        } else {
            ptr = &(*ptr)->sleep_next;
        }
    }
    unlock_scheduler();
}

void task_exit(uint32_t exit_code) {
    if (!current_task) return;
    lock_scheduler();
    current_task->exit_code = exit_code;
    sleepq_remove(current_task);
    if (current_task->waiting_queue) {
        waitq_remove(current_task->waiting_queue, current_task);
    }
    if (current_task->join_target) {
        if (current_task->join_target->join_refs) current_task->join_target->join_refs--;
        waitq_remove(&current_task->join_target->join_waiters, current_task);
        current_task->join_target = NULL;
    }
    current_task->state = TASK_DEAD;
    waitq_wake_all(&current_task->join_waiters);
    remove_task_from_runqueue(current_task);
    current_task->wait_next = dead_queue_head;
    dead_queue_head = current_task;
    unlock_scheduler();
    schedule();
    for (;;) asm volatile ("hlt");
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) return;
    extern uint32_t pit_freq;
    uint32_t ticks = (ms * pit_freq + 999) / 1000; 
    if (ticks == 0) ticks = 1;
    sleep_ticks(ticks);
}

void task_free_user_memory(task_t* t) {
    if (!t) return;

    if (t->user_pages && t->user_pages_count > 0) {
        for (uint32_t i = 0; i < t->user_pages_count; i++) {
            if (t->user_pages[i]) {
                pfa_free(t->user_pages[i]);
            }
        }
        kfree(t->user_pages);
        t->user_pages = NULL;
        t->user_pages_count = 0;
    }

    if (t->pd_phys) {
        vmm_free_page_directory(t->pd_phys);
        t->pd_phys = 0;
    }
}

void reap_dead_tasks(void) {
    if (!dead_queue_head) return;
    lock_scheduler();
    task_t* t = dead_queue_head;
    dead_queue_head = NULL;
    unlock_scheduler();

    task_t* keep = NULL;
    while (t) {
        task_t* next = t->wait_next;
        t->wait_next = NULL;
        if (t->join_refs != 0 || t->in_wait_queue) {
            t->wait_next = keep;
            keep = t;
            t = next;
            continue;
        }
        task_free_user_memory(t);
        if (t->stack_base) kfree(t->stack_base);
        if (t->kernel_stack_base) kfree(t->kernel_stack_base);
        kfree(t);
        t = next;
    }

    if (keep) {
        lock_scheduler();
        task_t* k = keep;
        while (k) {
            task_t* knext = k->wait_next;
            k->wait_next = dead_queue_head;
            dead_queue_head = k;
            k = knext;
        }
        unlock_scheduler();
    }
}

void switch_to(task_t* next) {
    if (next == current_task) return;

    task_t* prev = current_task;
    current_task = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->kernel_stack_base && next->kernel_stack_size) {
        uint32_t esp0 = (uint32_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_esp0(esp0);
    }

    if (next->regs.esp != 0 && 
        !((next->regs.esp >= 0xC0000000 && next->regs.esp < 0xD0000000) ||
          (next->regs.esp >= HEAP_START && next->regs.esp < (HEAP_START + HEAP_MAX_SIZE)))) {
        printf("Switch guard: bad esp 0x%08X for task %d\n", next->regs.esp, next->id);
        return;
    }

    prev->regs.cr3 = read_cr3();

    switch_to_asm(&prev->regs.esp, next->regs.esp);

    next->regs.cr3 = read_cr3();
}

void schedule(void) {
    schedule_force = 1;
    asm volatile ("int $48");
}

void yield(void) {
    schedule();
}

void block_current(void) {
    lock_scheduler();
    if (current_task) current_task->state = TASK_BLOCKED;
    unlock_scheduler();
    schedule_force = 1;
    schedule();
}

void wake_task(task_t* task) {
    if (!task) return;
    lock_scheduler();
    if (task->state == TASK_BLOCKED) task->state = TASK_READY;
    unlock_scheduler();
}

void task_kill(task_t* task, uint32_t exit_code) {
    if (!task) return;
    if (task == current_task) {
        task_exit(exit_code);
        return;
    }
    lock_scheduler();
    task->exit_code = exit_code;
    sleepq_remove(task);
    if (task->waiting_queue) {
        waitq_remove(task->waiting_queue, task);
    }
    if (task->join_target) {
        if (task->join_target->join_refs) task->join_target->join_refs--;
        waitq_remove(&task->join_target->join_waiters, task);
        task->join_target = NULL;
    }
    task->state = TASK_DEAD;
    waitq_wake_all(&task->join_waiters);
    remove_task_from_runqueue(task);
    task->wait_next = dead_queue_head;
    dead_queue_head = task;
    unlock_scheduler();
}

int task_join(task_t* task, uint32_t* exit_code) {
    if (!task || task == current_task) return -1;

    lock_scheduler();
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

    schedule();

    lock_scheduler();
    if (exit_code) *exit_code = task->exit_code;
    if (task->join_refs) task->join_refs--;
    current_task->join_target = NULL;
    unlock_scheduler();
    return 0;
}

static inline void save_irq_frame_into_task(task_t* task, const registers_t* regs, uint32_t regs_ptr) {
    task->regs.gs = regs->gs;
    task->regs.fs = regs->fs;
    task->regs.es = regs->es;
    task->regs.ds = regs->ds;

    task->regs.edi = regs->edi;
    task->regs.esi = regs->esi;
    task->regs.ebp = regs->ebp;
    task->regs.esp = regs_ptr;
    task->regs.ebx = regs->ebx;
    task->regs.edx = regs->edx;
    task->regs.ecx = regs->ecx;
    task->regs.eax = regs->eax;

    task->regs.int_no = regs->int_no;
    task->regs.err_code = regs->err_code;
    task->regs.eip = regs->eip;
    task->regs.cs = regs->cs;
    task->regs.eflags = regs->eflags;
    task->regs.cr3 = read_cr3();
}

registers_t* schedule_from_irq(registers_t* regs) {
    if (!current_task || !ready_queue_head) return regs;

    if (!current_task->syscall_frame) {
        save_irq_frame_into_task(current_task, regs, (uint32_t)regs);
    }

    if (!schedule_force) {
        if (current_task->time_slice > 0) current_task->time_slice--;
        if (current_task->time_slice != 0) return regs;
    }
    schedule_force = 0;

    current_task->time_slice = current_task->base_time_slice;

    task_t* next = current_task->next;
    if (!next) return regs;

    int i = 0;
    int safety = 0;
    while (next && next->state != TASK_READY) {
        if (next == current_task) return regs;
        next = next->next;
        if (++i > 256) return regs;
        if (++safety > 10000) {
            return regs;
        }
    }
    if (!next || next == current_task) return regs;

    if (current_task->state == TASK_RUNNING) current_task->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    if (next->kernel_stack_base && next->kernel_stack_size) {
        uint32_t esp0 = (uint32_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_esp0(esp0);
    }

    if (next->cr3 && next->cr3 != read_cr3()) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
    }

    registers_t* return_frame;
    if (next->syscall_frame) {
        return_frame = next->syscall_frame;
        next->syscall_frame = NULL;
        DPRINTF4("Resuming from syscall frame at 0x%08X\n", (uint32_t)return_frame);
    } else {
        if (next->regs.esp == 0) return regs;
        return_frame = (registers_t*)next->regs.esp;
    }
    
    if (next->is_user) {
        DPRINTF4("Switch to user: EIP=0x%08X CS=0x%04X ESP=0x%08X SS=0x%04X\n",
               return_frame->eip, return_frame->cs, return_frame->useresp, return_frame->ss);
        DPRINTF4("  Segs: DS=0x%04X ES=0x%04X FS=0x%04X GS=0x%04X\n",
               return_frame->ds, return_frame->es, return_frame->fs, return_frame->gs);
        
        return_frame->ds = return_frame->es = return_frame->fs = return_frame->gs = 0x23;
        return_frame->cs = 0x1B;
        return_frame->ss = 0x23;
    }
    
    return return_frame;
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
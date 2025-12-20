#include <kernel/task.h>
#include <kernel/debug.h>
#include <kernel/registers.h>
#include <kernel/common.h>
#include <kernel/mem_map.h>
#include <kernel/gdt.h>
#include <kernel/tty.h>
#include <kernel/elf.h>
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
#define USER_STACK_SIZE 0x10000

static uint32_t next_task_id = 1;

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
    if (!ready_queue_head || !task) return;
    
    if (task->next == task) {
        if (task == ready_queue_head) {
            ready_queue_head = NULL;
        }
        return;
    }
    
    task_t* prev = ready_queue_head;
    while (prev->next != task) {
        prev = prev->next;
        if (prev == ready_queue_head) {
            return;
        }
    }
    
    prev->next = task->next;
    
    if (task == ready_queue_head) {
        ready_queue_head = task->next;
    }
}

task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode) {
    return create_task_with_cr3(entry, initial_state, user_mode, read_cr3());
}

task_t* create_task_with_cr3(void (*entry)(void), task_state_t initial_state, bool user_mode, uint32_t cr3) {
    if (!entry) return NULL;

    DPRINTF3("create_task_with_cr3: verify heap before alloc\n");
    if (debugMode && debugLevel >= 3) heap_verify();

    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    uint8_t* stack_base = user_mode ? NULL : (uint8_t*)kmalloc(TASK_STACK_SIZE);
    uint8_t* kernel_stack_base = (uint8_t*)kmalloc(KERNEL_STACK_SIZE);
    if (!new_task || (!user_mode && !stack_base) || !kernel_stack_base) {
        printf("Task alloc fail!\n");
        if (debugMode && debugLevel >= 3) heap_verify();
        if (new_task) kfree(new_task);
        if (stack_base) kfree(stack_base);
        if (kernel_stack_base) kfree(kernel_stack_base);
        return NULL;
    }
    memset(new_task, 0, sizeof(task_t));
    if (stack_base) memset(stack_base, 0, TASK_STACK_SIZE);
    memset(kernel_stack_base, 0, KERNEL_STACK_SIZE);

    new_task->id = next_task_id;
    new_task->pid = next_task_id;
    next_task_id++;
    new_task->state = initial_state;
    new_task->next = NULL;
    new_task->regs.cr3 = cr3;
    new_task->cr3 = cr3;
    new_task->console_id = -1; 
    DPRINTF3("create_task_with_cr3: new task id=%u cr3=0x%08X\n", new_task->id, cr3);
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
        new_task->user_brk = 0;
        
        registers_t* frame = (registers_t*)kesp;
        DPRINTF3("User task frame at 0x%08X:\n", (uint32_t)kesp);
        DPRINTF3("  EIP=0x%08X CS=0x%04X EFLAGS=0x%08X\n", frame->eip, frame->cs, frame->eflags);
        DPRINTF3("  useresp=0x%08X SS=0x%04X\n", frame->useresp, frame->ss);
        DPRINTF3("  DS=0x%04X ES=0x%04X FS=0x%04X GS=0x%04X\n", frame->ds, frame->es, frame->fs, frame->gs);
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
                t->user_pages[i] = 0;
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
        if (t->is_user) {
            t->wait_next = keep;
            keep = t;
            t = next;
            continue;
        }

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

static void free_dead_task_resources(task_t* t) {
    if (!t || t->state != TASK_DEAD) return;
    
    if (dead_queue_head == t) {
        dead_queue_head = t->wait_next;
    } else {
        task_t* prev = dead_queue_head;
        while (prev && prev->wait_next != t) prev = prev->wait_next;
        if (prev) prev->wait_next = t->wait_next;
    }
    t->wait_next = NULL;
    
    unlock_scheduler();
    
    if (t->user_pages && t->user_pages_count > 0) {
        for (uint32_t i = 0; i < t->user_pages_count; i++) {
            if (t->user_pages[i]) {
                pfa_free(t->user_pages[i]);
                t->user_pages[i] = 0;
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
    
    if (t->stack_base) {
        kfree(t->stack_base);
        t->stack_base = NULL;
    }
    if (t->kernel_stack_base) {
        kfree(t->kernel_stack_base);
        t->kernel_stack_base = NULL;
    }
    kfree(t);
    
    lock_scheduler();
}

int task_join(task_t* task, uint32_t* exit_code) {
    if (!task || task == current_task) return -1;

    lock_scheduler();
    if (task->state == TASK_DEAD) {
        if (exit_code) *exit_code = task->exit_code;
        if (task->join_refs == 0 && !task->in_wait_queue) {
            free_dead_task_resources(task);
        }
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
    
    if (task->join_refs == 0 && task->state == TASK_DEAD && !task->in_wait_queue) {
        free_dead_task_resources(task);
    }
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

    // If we're currently executing inside a syscall, do not preempt on timer IRQs.
    // The scheduler can only safely switch using a fully saved IRQ frame; when
    // `syscall_frame` is set we must let the syscall run to a safe point (or
    // explicitly yield via int 48).
    if (regs->int_no != 48 && current_task->syscall_frame) {
        return regs;
    }

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

        uint32_t check_virt[2] = { return_frame->eip & ~0xFFFu, return_frame->useresp & ~0xFFFu };
        for (int ci = 0; ci < 2; ci++) {
            uint32_t v = check_virt[ci];
            uint32_t pd_idx = v >> 22;
            uint32_t pt_idx = (v >> 12) & 0x3FF;

            uint32_t temp_pd = 0xF7000000;
            uint32_t temp_pt = 0xF7001000;

            vmm_temp_map_raw(temp_pd, next->pd_phys);
            uint32_t pde = ((uint32_t *)temp_pd)[pd_idx];
            if (pde & 1) {
                uint32_t pt_phys = pde & ~0xFFF;
                vmm_temp_map_raw(temp_pt, pt_phys);
                uint32_t pte = ((uint32_t *)temp_pt)[pt_idx];
                vmm_temp_unmap_raw(temp_pt);
                DPRINTF4("switch-check: virt=0x%08X pd_idx=%u pt_idx=%u pde=0x%08X pte=0x%08X\n", v, pd_idx, pt_idx, pde, pte);
            } else {
                DPRINTF4("switch-check: virt=0x%08X pd_idx=%u pde=0x%08X (no PT)\n", v, pd_idx, pde);
            }
            vmm_temp_unmap_raw(temp_pd);
        }

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

#define FORK_MIN_FREE_PAGES 64

pid_t task_fork(registers_t *parent_regs) {
    if (!current_task || !current_task->is_user) {
        printf("task_fork: can only fork user tasks\n");
        return -1;
    }

    uint32_t free_pages = pfa_count_free();
    uint32_t needed_pages = current_task->user_pages_count + FORK_MIN_FREE_PAGES;
    if (free_pages < needed_pages) {
        printf("task_fork: insufficient memory (free=%u, need~%u)\n", free_pages, needed_pages);
        return -1;
    }

    uint32_t *child_user_pages = NULL;
    uint32_t child_user_pages_count = 0;

    uint32_t child_pd = vmm_clone_page_directory(current_task->pd_phys, &child_user_pages, &child_user_pages_count);
    if (!child_pd) {
        printf("task_fork: failed to clone page directory\n");
        return -1;
    }

    DPRINTF2("task_fork: creating child task with cloned pd=0x%08X\n", child_pd);

    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    uint8_t* kernel_stack_base = (uint8_t*)kmalloc(KERNEL_STACK_SIZE);
    if (!child || !kernel_stack_base) {
        printf("task_fork: allocation failed\n");
        if (child) kfree(child);
        if (kernel_stack_base) kfree(kernel_stack_base);
        if (child_user_pages) {
            for (uint32_t i = 0; i < child_user_pages_count; i++) {
                pfa_free(child_user_pages[i]);
            }
            kfree(child_user_pages);
        }
        vmm_free_page_directory(child_pd);
        return -1;
    }
    memset(child, 0, sizeof(task_t));
    memset(kernel_stack_base, 0, KERNEL_STACK_SIZE);

    child->id = next_task_id;
    child->pid = next_task_id;
    next_task_id++;
    child->state = TASK_READY;
    child->next = NULL;
    child->cr3 = child_pd;
    child->regs.cr3 = child_pd;
    child->time_slice = SCHED_DEFAULT_TIMESLICE;
    child->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    child->stack_base = NULL;
    child->stack_size = 0;
    child->kernel_stack_base = kernel_stack_base;
    child->kernel_stack_size = KERNEL_STACK_SIZE;
    child->wake_tick = 0;
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
    child->syscall_frame = NULL;
    child->pd_phys = child_pd;
    child->user_pages = child_user_pages;
    child->user_pages_count = child_user_pages_count;
    child->user_brk = current_task->user_brk;
    child->console_id = current_task->console_id;

    registers_t *child_frame = (registers_t *)((uint8_t *)kernel_stack_base + KERNEL_STACK_SIZE - sizeof(registers_t));
    memcpy(child_frame, parent_regs, sizeof(registers_t));
    child_frame->eax = 0;
    child_frame->int_no = 0;
    child_frame->err_code = 0;

    child->regs.esp = (uint32_t)child_frame;
    child->regs.eip = child_frame->eip;
    child->regs.eflags = child_frame->eflags;
    child->regs.cs = child_frame->cs;
    child->regs.ds = child->regs.es = child->regs.fs = child->regs.gs = child_frame->ds;

    lock_scheduler();
    add_task_to_runqueue(child);
    unlock_scheduler();

    DPRINTF2("task_fork: parent pid=%d, child pid=%d\n", current_task->pid, child->pid);

    return child->pid;
}

int task_exec(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs) {
    if (!current_task || !current_task->is_user) {
        printf("task_exec: can only exec in user tasks\n");
        return -1;
    }
    if (!bin_start || bin_size == 0) {
        printf("task_exec: invalid binary\n");
        return -1;
    }

    uint32_t free_pages = pfa_count_free();
    uint32_t needed_estimate = 20;
    if (free_pages < needed_estimate) {
        printf("task_exec: insufficient memory (free=%u)\n", free_pages);
        return -1;
    }

    int elf_valid = elf_validate(bin_start, bin_size);
    if (elf_valid != 0) {
        printf("task_exec: ELF validation failed (%d)\n", elf_valid);
        return -1;
    }

    DPRINTF2("task_exec: replacing task %d with ELF binary (%u bytes)\n", current_task->pid, bin_size);

    uint32_t old_pd = current_task->pd_phys;
    uint32_t *old_user_pages = current_task->user_pages;
    uint32_t old_user_pages_count = current_task->user_pages_count;

    uint32_t stack_top = 0x00800000;
    uint32_t stack_size = 0x4000;

    uint32_t new_pd = vmm_create_page_directory();
    if (!new_pd) {
        printf("task_exec: failed to create page directory\n");
        return -1;
    }

    elf_info_t elf_info;
    uint32_t *elf_pages = NULL;
    uint32_t elf_page_count = 0;

    int load_result = elf_load_to_pd(new_pd, bin_start, bin_size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        printf("task_exec: ELF loading failed (%d)\n", load_result);
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        return -1;
    }

    uint32_t stack_page_count = 0;
    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(new_pd, stack_top - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        printf("task_exec: failed to map stack\n");
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        return -1;
    }

    current_task->pd_phys = new_pd;
    current_task->cr3 = new_pd;
    current_task->regs.cr3 = new_pd;
    vmm_set_cr3(new_pd);

    if (old_user_pages && old_user_pages_count > 0) {
        for (uint32_t i = 0; i < old_user_pages_count; i++) {
            if (old_user_pages[i]) {
                pfa_free(old_user_pages[i]);
            }
        }
        kfree(old_user_pages);
    }
    if (old_pd) {
        vmm_free_page_directory(old_pd);
    }

    current_task->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;

    uint32_t total_pages = elf_page_count + stack_page_count;
    if (total_pages == 0 || total_pages > 65536) {
        printf("task_exec: suspicious total_pages=%u\n", total_pages);
        kfree(elf_pages);
        kfree(stack_pages);
        vmm_free_page_directory(new_pd);
        return -1;
    }

    current_task->user_pages = (uint32_t *)kmalloc(total_pages * sizeof(uint32_t));
    if (current_task->user_pages) {
        memcpy(current_task->user_pages, elf_pages, elf_page_count * sizeof(uint32_t));
        memcpy(current_task->user_pages + elf_page_count, stack_pages, stack_page_count * sizeof(uint32_t));
        current_task->user_pages_count = total_pages;
    } else {
        current_task->user_pages_count = 0;
    }

    kfree(elf_pages);
    kfree(stack_pages);

    regs->eip = elf_info.entry_point;
    regs->useresp = stack_top - 16;
    regs->ebp = 0;
    regs->eax = 0;
    regs->ebx = 0;
    regs->ecx = 0;
    regs->edx = 0;
    regs->esi = 0;
    regs->edi = 0;
    regs->cs = 0x1B;
    regs->ds = regs->es = regs->fs = regs->gs = regs->ss = 0x23;
    regs->eflags = 0x202;

    DPRINTF2("task_exec: new ELF entry at 0x%08X, stack at 0x%08X\n", elf_info.entry_point, regs->useresp);

    return 0;
}
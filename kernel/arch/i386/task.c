#include <kernel/task.h>
#include <kernel/pipe.h>
#include <kernel/debug.h>
#include <kernel/registers.h>
#include <kernel/common.h>
#include <kernel/mem_map.h>
#include <kernel/gdt.h>
#include <kernel/tty.h>
#include <kernel/elf.h>
#include <kernel/console.h>
#include <kernel/creds.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define KERR_EPERM   1
#define KERR_EIO     5
#define KERR_ENOEXEC 8
#define KERR_ENOMEM  12

static void task_error(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (current_task && current_task->console_id >= 0 && console_is_initialized()) {
        console_write_to(current_task->console_id, buf, (size_t)n);
    }
}

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
#define USER_STACK_SIZE 0x10000u
#define USER_STACK_GAP  0x1000u
#define USER_STACK_INIT_ESP (USER_STACK_TOP - USER_STACK_GAP - 16u)

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

static int task_ptr_valid(task_t *t) {
    if (!t) return 0;
    uint32_t a = (uint32_t)t;
    if ((a & 0xFFFF0000u) == 0xFEFE0000u) return 0;
    if (a < 0xC0000000) return 0;
    return 1;
}

task_t* task_find(pid_t pid) {
    lock_scheduler();
    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (!task_ptr_valid(t)) break;
            if (t->pid == pid) {
                unlock_scheduler();
                return t;
            }
            t = t->next;
        } while (t && t != ready_queue_head);
    }

    task_t* d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->pid == pid) {
            unlock_scheduler();
            return d;
        }
        d = d->wait_next;
    }
    unlock_scheduler();
    return NULL;
}

int task_has_child_of(pid_t parent_pid, pid_t pgid_filter) {
    int found = 0;
    lock_scheduler();

    task_t *t = ready_queue_head;
    if (t) {
        do {
            if (!task_ptr_valid(t)) break;
            if (t->ppid == parent_pid) {
                if (pgid_filter <= 0 || t->pgid == pgid_filter) {
                    found = 1;
                    break;
                }
            }
            t = t->next;
        } while (t && t != ready_queue_head);
    }

    if (!found) {
        task_t *d = dead_queue_head;
        while (d && task_ptr_valid(d)) {
            if (d->ppid == parent_pid) {
                if (pgid_filter <= 0 || d->pgid == pgid_filter) {
                    found = 1;
                    break;
                }
            }
            d = d->wait_next;
        }
    }

    unlock_scheduler();
    return found;
}

task_t* task_find_dead_child_of(pid_t parent_pid, pid_t pgid_filter) {
    lock_scheduler();
    task_t *d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->ppid == parent_pid) {
            if (pgid_filter <= 0 || d->pgid == pgid_filter) {
                unlock_scheduler();
                return d;
            }
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
    while (t) {
        if (!task_ptr_valid(t)) {
            q->head = NULL;
            return NULL;
        }
        if (t->state != TASK_DEAD) break;
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        t = q->head;
    }
    if (!t || !task_ptr_valid(t)) return NULL;
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

void waitq_wake_one(wait_queue_t *q) {
    if (!q) return;
    task_t *t = waitq_pop(q);
    if (t && t->state == TASK_BLOCKED) t->state = TASK_READY;
}

void waitq_wait(wait_queue_t *q) {
    if (!q || !current_task) return;
    waitq_add(q, current_task);
    current_task->state = TASK_BLOCKED;
    yield();
}

void waitq_remove(wait_queue_t* q, task_t* t) {
    if (!q || !t || !q->head) return;
    if (!task_ptr_valid(q->head)) {
        q->head = NULL;
        return;
    }
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
        if (!task_ptr_valid(cur)) {
            prev->wait_next = NULL;
            return;
        }
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
    current_task->next = current_task;
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
    ready_queue_head = current_task;
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
        ready_queue_head = new_task;
        new_task->next = new_task;
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
    new_task->cwd[0] = '/';
    new_task->cwd[1] = '\0';
    task_init_fds(new_task);
    if (stack_base) memset(stack_base, 0, TASK_STACK_SIZE);
    memset(kernel_stack_base, 0, KERNEL_STACK_SIZE);

    new_task->id = next_task_id;
    new_task->pid = next_task_id;
    next_task_id++;

    creds_init_task(new_task->pid);

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
    new_task->waiting_for_any_child = 0;
    new_task->is_user = user_mode;
    new_task->syscall_frame = NULL;

    if (user_mode) {
        uint32_t* kesp = (uint32_t*)(kernel_stack_base + KERNEL_STACK_SIZE);
        
        uint32_t user_esp = USER_STACK_INIT_ESP;
        
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
    task_exit_deferred(exit_code);
    schedule();
    for (;;) asm volatile ("hlt");
}

void task_exit_deferred(uint32_t exit_code) {
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

    if (current_task->ppid > 0) {
        task_t *parent = task_find((pid_t)current_task->ppid);
        if (parent && parent->waiting_for_any_child && parent->state == TASK_BLOCKED) {
            parent->state = TASK_READY;
        }
    }

    unlock_scheduler();
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
    while (t && task_ptr_valid(t)) {
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
        while (k && task_ptr_valid(k)) {
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
    
    if (regs->int_no != 48 && current_task->syscall_frame) {
        return regs;
    }

    if (current_task->state != TASK_DEAD) {
        save_irq_frame_into_task(current_task, regs, (uint32_t)regs);
    }

    int must_switch = (current_task->state == TASK_DEAD);

    if (!schedule_force && !must_switch) {
        if (current_task->time_slice > 0) current_task->time_slice--;
        if (current_task->time_slice != 0) return regs;
    }
    schedule_force = 0;

    if (!must_switch) {
        current_task->time_slice = current_task->base_time_slice;
    }

    task_t* next = ready_queue_head;
    task_t* start = next;
    int safety = 0;
    
    while (next) {
        if ((next->state == TASK_READY || next->state == TASK_RUNNING) && next != current_task) {
            break;
        }
        next = next->next;
        if (next == start || ++safety > 10000) {
            next = NULL;
            break;
        }
    }

    if (!next && must_switch) {
        printf("schedule: no runnable tasks, system halted\n");
        for (;;) asm volatile ("hlt");
    }
    
    if (!next) return regs;

    if (current_task->state == TASK_RUNNING) current_task->state = TASK_READY;
    next->state = TASK_RUNNING;
    
    if (next->id == 0 && next->regs.eip == 0) {
        current_task = next;
        return regs;
    }
    
    current_task = next;

    if (next->kernel_stack_base && next->kernel_stack_size) {
        uint32_t esp0 = (uint32_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_esp0(esp0);
    }

    if (next->cr3 && next->cr3 != read_cr3()) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
    }

    if (next->tls_base) {
        gdt_set_tls(GDT_TLS_ENTRY_1, next->tls_base, next->tls_limit);
    }

    registers_t* return_frame;
    if (next->regs.esp == 0) return regs;
    return_frame = (registers_t*)next->regs.esp;
    
    if (next->syscall_frame) {
        DPRINTF4("Resuming kernel context for blocked syscall at 0x%08X\n", return_frame->eip);
    } else {
        DPRINTF4("Resuming from IRQ frame at 0x%08X\n", return_frame->eip);
        
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

            return_frame->ds = return_frame->es = return_frame->fs = 0x23;
            if (next->tls_base) {
                return_frame->gs = gdt_get_tls_selector(GDT_TLS_ENTRY_1);
            } else {
                return_frame->gs = 0x23;
            }
            return_frame->cs = 0x1B;
            return_frame->ss = 0x23;
        }
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

void task_init_fds(task_t *task) {
    if (!task) return;
    memset(task->fds, 0, sizeof(task->fds));
    task->fds[0].in_use = 1;
    task->fds[0].type = FD_TYPE_STDIN;
    task->fds[0].ref_count = 1;
    task->fds[1].in_use = 1;
    task->fds[1].type = FD_TYPE_STDOUT;
    task->fds[1].ref_count = 1;
    task->fds[2].in_use = 1;
    task->fds[2].type = FD_TYPE_STDERR;
    task->fds[2].ref_count = 1;
}

int task_fd_alloc(task_t *task) {
    if (!task) return -1;
    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (!task->fds[i].in_use) {
            task->fds[i].in_use = 1;
            task->fds[i].ref_count = 1;
            return i;
        }
    }
    return -1;
}

void task_fd_free(task_t *task, int fd) {
    if (!task || fd < 0 || fd >= TASK_MAX_FDS) return;
    if (!task->fds[fd].in_use) return;
    task->fds[fd].in_use = 0;
    task->fds[fd].ref_count = 0;
    task->fds[fd].node = NULL;
    task->fds[fd].offset = 0;
    task->fds[fd].flags = 0;
    task->fds[fd].private_data = NULL;
}

task_fd_t *task_fd_get(task_t *task, int fd) {
    if (!task || fd < 0 || fd >= TASK_MAX_FDS) return NULL;
    if (!task->fds[fd].in_use) return NULL;
    return &task->fds[fd];
}

void task_fd_close_all(task_t *task) {
    if (!task) return;
    for (int i = 3; i < TASK_MAX_FDS; i++) {
        task_fd_free(task, i);
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

    creds_copy_task(current_task->pid, child->pid);

    child->ppid = current_task->pid;
    child->pgid = current_task->pgid ? current_task->pgid : current_task->pid;
    child->sid = current_task->sid ? current_task->sid : current_task->pid;

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
    child->tls_base = current_task->tls_base;
    child->tls_limit = current_task->tls_limit;
    child->waiting_for_any_child = 0;
    memcpy(child->cwd, current_task->cwd, sizeof(child->cwd));
    memcpy(child->fds, current_task->fds, sizeof(child->fds));
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (child->fds[i].in_use && child->fds[i].ref_count > 0) {
            child->fds[i].ref_count++;
        }
        if (child->fds[i].in_use && (child->fds[i].type == FD_TYPE_PIPE_R || child->fds[i].type == FD_TYPE_PIPE_W) && child->fds[i].private_data) {
            pipe_t *p = (pipe_t *)child->fds[i].private_data;
            if (child->fds[i].type == FD_TYPE_PIPE_R) p->readers++;
            else p->writers++;
        }
    }
    child->envp = NULL;
    child->envc = 0;

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
        task_error("task_exec: can only exec in user tasks\n");
        return -1;
    }
    if (!bin_start || bin_size == 0) {
        task_error("task_exec: invalid binary\n");
        return -1;
    }

    uint32_t free_pages = pfa_count_free();
    uint32_t needed_estimate = 20 + (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (free_pages < needed_estimate) {
        task_error("task_exec: insufficient memory (free=%u need=%u)\n", free_pages, needed_estimate);
        return -1;
    }

    uint8_t *kernel_bin = (uint8_t *)kmalloc(bin_size);
    if (!kernel_bin) {
        task_error("task_exec: failed to allocate kernel buffer (%u bytes)\n", bin_size);
        return -1;
    }
    memcpy(kernel_bin, bin_start, bin_size);

    int elf_valid = elf_validate(kernel_bin, bin_size);
    if (elf_valid != 0) {
        task_error("task_exec: ELF validation failed (%d)\n", elf_valid);
        kfree(kernel_bin);
        return -1;
    }

    DPRINTF2("task_exec: replacing task %d with ELF binary (%u bytes)\n", current_task->pid, bin_size);

    uint32_t old_pd = current_task->pd_phys;
    uint32_t *old_user_pages = current_task->user_pages;
    uint32_t old_user_pages_count = current_task->user_pages_count;

    uint32_t stack_top = USER_STACK_TOP;
    uint32_t stack_size = USER_STACK_SIZE;

    uint32_t new_pd = vmm_create_page_directory();
    if (!new_pd) {
        task_error("task_exec: failed to create page directory\n");
        kfree(kernel_bin);
        return -1;
    }

    elf_info_t elf_info;
    uint32_t *elf_pages = NULL;
    uint32_t elf_page_count = 0;

    int load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        task_error("task_exec: ELF loading failed (%d)\n", load_result);
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        kfree(kernel_bin);
        return -1;
    }

    kfree(kernel_bin);

    uint32_t stack_page_count = 0;
    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(new_pd, stack_top - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec: failed to map stack\n");
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
        task_error("task_exec: suspicious total_pages=%u\n", total_pages);
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

    current_task->tls_base = 0;
    current_task->tls_limit = 0;

    uint32_t sp = stack_top - USER_STACK_GAP - 16;
    uint32_t zero = 0;
    
    const char *prog_name = "program";
    int prog_len = 0;
    while (prog_name[prog_len]) prog_len++;
    sp -= (prog_len + 1 + 3) & ~3;
    uint32_t prog_addr = sp;
    vmm_copy_to_pd(new_pd, sp, prog_name, prog_len + 1);
    
    sp = sp & ~0xF;
    
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &zero, sizeof(uint32_t));
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &zero, sizeof(uint32_t));
    
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &zero, sizeof(uint32_t));
    
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &prog_addr, sizeof(uint32_t));
    
    sp -= 4;
    uint32_t argc_val = 1;
    vmm_copy_to_pd(new_pd, sp, &argc_val, sizeof(uint32_t));

    regs->eip = elf_info.entry_point;
    regs->useresp = sp;
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

    DPRINTF2("task_exec: new ELF entry at 0x%08X, stack at 0x%08X\n", elf_info.entry_point, sp);

    return 0;
}

int task_exec_with_args(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs,
                        int argc, char **argv, int envc, char **envp) {
    if (!current_task || !current_task->is_user) {
        task_error("task_exec_with_args: can only exec in user tasks\n");
        return -KERR_EPERM;
    }
    if (!bin_start || bin_size == 0) {
        task_error("task_exec_with_args: invalid binary\n");
        return -KERR_ENOEXEC;
    }

    uint32_t free_pages = pfa_count_free();
    uint32_t needed_estimate = 20 + (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (free_pages < needed_estimate) {
        task_error("task_exec_with_args: insufficient memory\n");
        return -KERR_ENOMEM;
    }

    uint8_t *kernel_bin = (uint8_t *)kmalloc(bin_size);
    if (!kernel_bin) {
        task_error("task_exec_with_args: failed to allocate kernel buffer\n");
        return -KERR_ENOMEM;
    }
    memcpy(kernel_bin, bin_start, bin_size);

    char **k_argv = NULL;
    char **k_envp = NULL;
    
    if (argc > 0 && argv) {
        k_argv = (char **)kmalloc((argc + 1) * sizeof(char *));
        if (!k_argv) { kfree(kernel_bin); return -KERR_ENOMEM; }
        for (int i = 0; i < argc; i++) {
            int len = 0;
            while (argv[i][len]) len++;
            k_argv[i] = (char *)kmalloc(len + 1);
            if (!k_argv[i]) {
                for (int j = 0; j < i; j++) kfree(k_argv[j]);
                kfree(k_argv); kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (int j = 0; j <= len; j++) k_argv[i][j] = argv[i][j];
        }
        k_argv[argc] = NULL;
    }
    
    if (envc > 0 && envp) {
        k_envp = (char **)kmalloc((envc + 1) * sizeof(char *));
        if (!k_envp) {
            if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
            kfree(kernel_bin);
            return -KERR_ENOMEM;
        }
        for (int i = 0; i < envc; i++) {
            int len = 0;
            while (envp[i][len]) len++;
            k_envp[i] = (char *)kmalloc(len + 1);
            if (!k_envp[i]) {
                for (int j = 0; j < i; j++) kfree(k_envp[j]);
                kfree(k_envp);
                if (k_argv) { for (int j = 0; j < argc; j++) kfree(k_argv[j]); kfree(k_argv); }
                kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (int j = 0; j <= len; j++) k_envp[i][j] = envp[i][j];
        }
        k_envp[envc] = NULL;
    }

    int elf_valid = elf_validate(kernel_bin, bin_size);
    if (elf_valid != 0) {
        task_error("task_exec_with_args: ELF validation failed\n");
        if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
        if (k_envp) { for (int i = 0; i < envc; i++) kfree(k_envp[i]); kfree(k_envp); }
        kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    uint32_t old_pd = current_task->pd_phys;
    uint32_t *old_user_pages = current_task->user_pages;
    uint32_t old_user_pages_count = current_task->user_pages_count;

    uint32_t stack_top = USER_STACK_TOP;
    uint32_t stack_size = USER_STACK_SIZE;

    uint32_t new_pd = vmm_create_page_directory();
    if (!new_pd) {
        task_error("task_exec_with_args: failed to create page directory\n");
        if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
        if (k_envp) { for (int i = 0; i < envc; i++) kfree(k_envp[i]); kfree(k_envp); }
        kfree(kernel_bin);
        return -KERR_ENOMEM;
    }

    elf_info_t elf_info;
    uint32_t *elf_pages = NULL;
    uint32_t elf_page_count = 0;

    int load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        task_error("task_exec_with_args: ELF loading failed\n");
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
        if (k_envp) { for (int i = 0; i < envc; i++) kfree(k_envp[i]); kfree(k_envp); }
        kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    kfree(kernel_bin);

    uint32_t stack_page_count = 0;
    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(new_pd, stack_top - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec_with_args: failed to map stack\n");
        if (elf_pages) {
            for (uint32_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
        if (k_envp) { for (int i = 0; i < envc; i++) kfree(k_envp[i]); kfree(k_envp); }
        return -KERR_ENOMEM;
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
        task_error("task_exec_with_args: suspicious total_pages=%u\n", total_pages);
        kfree(elf_pages);
        kfree(stack_pages);
        if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
        if (k_envp) { for (int i = 0; i < envc; i++) kfree(k_envp[i]); kfree(k_envp); }
        vmm_free_page_directory(new_pd);
        return -KERR_ENOEXEC;
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

    uint32_t sp = stack_top - USER_STACK_GAP - 16;
    
    uint32_t *envp_ptrs = NULL;
    uint32_t *argv_ptrs = NULL;
    
    if (envc > 0 && k_envp) {
        envp_ptrs = (uint32_t *)kmalloc((envc + 1) * sizeof(uint32_t));
        for (int i = envc - 1; i >= 0; i--) {
            int len = 0;
            while (k_envp[i][len]) len++;
            sp -= (len + 1 + 3) & ~3;
            char *dst = (char *)sp;
            for (int j = 0; j <= len; j++) dst[j] = k_envp[i][j];
            envp_ptrs[i] = sp;
        }
        envp_ptrs[envc] = 0;
    }
    
    if (argc > 0 && k_argv) {
        argv_ptrs = (uint32_t *)kmalloc((argc + 1) * sizeof(uint32_t));
        for (int i = argc - 1; i >= 0; i--) {
            int len = 0;
            while (k_argv[i][len]) len++;
            sp -= (len + 1 + 3) & ~3;
            char *dst = (char *)sp;
            for (int j = 0; j <= len; j++) dst[j] = k_argv[i][j];
            argv_ptrs[i] = sp;
        }
        argv_ptrs[argc] = 0;
    }
    
    sp = sp & ~0xF;
    
    uint32_t zero = 0;
    sp -= 4;
    *(uint32_t *)sp = zero;
    sp -= 4;
    *(uint32_t *)sp = zero;
    
    if (envp_ptrs) {
        for (int i = envc; i >= 0; i--) {
            sp -= 4;
            *(uint32_t *)sp = envp_ptrs[i];
        }
    } else {
        sp -= 4;
        *(uint32_t *)sp = zero;
    }
    
    if (argv_ptrs) {
        for (int i = argc; i >= 0; i--) {
            sp -= 4;
            *(uint32_t *)sp = argv_ptrs[i];
        }
    } else {
        sp -= 4;
        *(uint32_t *)sp = zero;
    }
    
    sp -= 4;
    *(uint32_t *)sp = (uint32_t)argc;
    
    if (argv_ptrs) kfree(argv_ptrs);
    if (envp_ptrs) kfree(envp_ptrs);
    if (k_argv) { for (int i = 0; i < argc; i++) kfree(k_argv[i]); kfree(k_argv); }
    if (k_envp) { for (int i = 0; i < envc; i++) kfree(k_envp[i]); kfree(k_envp); }

    regs->eip = elf_info.entry_point;
    regs->useresp = sp;
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

    current_task->tls_base = 0;
    current_task->tls_limit = 0;

    return 0;
}

pid_t task_create_thread(void (*entry)(void)) {
    if (!current_task || !current_task->is_user) {
        return -1;
    }
    
    task_t *new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    
    new_task->kernel_stack_base = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!new_task->kernel_stack_base) {
        kfree(new_task);
        return -1;
    }
    memset(new_task->kernel_stack_base, 0, KERNEL_STACK_SIZE);
    new_task->kernel_stack_size = KERNEL_STACK_SIZE;
    
    new_task->id = next_task_id++;
    new_task->pid = new_task->id;
    new_task->state = TASK_READY;
    new_task->is_user = true;
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    
    new_task->pd_phys = current_task->pd_phys;
    new_task->cr3 = current_task->cr3;
    new_task->user_pages = NULL;
    new_task->user_pages_count = 0;
    new_task->user_brk = current_task->user_brk;
    new_task->console_id = current_task->console_id;
    
    for (int i = 0; i < 256; i++) {
        new_task->cwd[i] = current_task->cwd[i];
    }
    
    uint32_t thread_stack_size = 0x4000;
    uint32_t thread_stack_base = (current_task->user_brk + 0xFFF) & ~0xFFF;
    uint32_t thread_stack_top = thread_stack_base + thread_stack_size;
    
    uint32_t stack_page_count = 0;
    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(current_task->pd_phys, thread_stack_base, thread_stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        kfree(new_task->kernel_stack_base);
        kfree(new_task);
        return -1;
    }
    
    new_task->user_pages = stack_pages;
    new_task->user_pages_count = stack_page_count;
    
    current_task->user_brk = thread_stack_top + 0x1000;
    
    uint32_t *stack_ptr = (uint32_t *)(new_task->kernel_stack_base + KERNEL_STACK_SIZE);
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = thread_stack_top - 16;
    stack_ptr--;
    *stack_ptr = 0x202;
    stack_ptr--;
    *stack_ptr = 0x1B;
    stack_ptr--;
    *stack_ptr = (uint32_t)entry;
    
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
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = 0x23;
    
    new_task->regs.esp = (uint32_t)stack_ptr;

    new_task->regs.cr3 = new_task->cr3;
    new_task->regs.eip = (uint32_t)entry;
    new_task->regs.useresp = thread_stack_top - 16;
    new_task->regs.cs = 0x1B;
    new_task->regs.ds = new_task->regs.es = new_task->regs.fs = new_task->regs.gs = new_task->regs.ss = 0x23;
    new_task->regs.eflags = 0x202;
    
    lock_scheduler();
    add_task_to_runqueue(new_task);
    unlock_scheduler();
    
    return new_task->pid;
}

pid_t task_create_thread_with_arg(void *(*entry)(void *), void *arg) {
    if (!current_task || !current_task->is_user) {
        return -1;
    }
    
    task_t *new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    
    new_task->kernel_stack_base = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!new_task->kernel_stack_base) {
        kfree(new_task);
        return -1;
    }
    memset(new_task->kernel_stack_base, 0, KERNEL_STACK_SIZE);
    new_task->kernel_stack_size = KERNEL_STACK_SIZE;
    
    new_task->id = next_task_id++;
    new_task->pid = new_task->id;
    new_task->state = TASK_READY;
    new_task->is_user = true;
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    
    new_task->pd_phys = current_task->pd_phys;
    new_task->cr3 = current_task->cr3;
    new_task->user_pages = NULL;
    new_task->user_pages_count = 0;
    new_task->user_brk = current_task->user_brk;
    new_task->console_id = current_task->console_id;
    
    for (int i = 0; i < 256; i++) {
        new_task->cwd[i] = current_task->cwd[i];
    }
    
    uint32_t thread_stack_size = 0x4000;
    uint32_t thread_stack_base = (current_task->user_brk + 0xFFF) & ~0xFFF;
    uint32_t thread_stack_top = thread_stack_base + thread_stack_size;
    
    uint32_t stack_page_count = 0;
    uint32_t *stack_pages = vmm_map_range_in_pd_tracked(current_task->pd_phys, thread_stack_base, thread_stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        kfree(new_task->kernel_stack_base);
        kfree(new_task);
        return -1;
    }
    
    new_task->user_pages = stack_pages;
    new_task->user_pages_count = stack_page_count;
    
    current_task->user_brk = thread_stack_top + 0x1000;
    
    uint32_t zero_val = 0;
    uint32_t arg_val = (uint32_t)arg;
    vmm_copy_to_pd(current_task->pd_phys, thread_stack_top - 16, &zero_val, 4);
    vmm_copy_to_pd(current_task->pd_phys, thread_stack_top - 12, &arg_val, 4);
    
    uint32_t *stack_ptr = (uint32_t *)(new_task->kernel_stack_base + KERNEL_STACK_SIZE);
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = thread_stack_top - 16;
    stack_ptr--;
    *stack_ptr = 0x202;
    stack_ptr--;
    *stack_ptr = 0x1B;
    stack_ptr--;
    *stack_ptr = (uint32_t)entry;
    
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
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = 0x23;
    
    new_task->regs.esp = (uint32_t)stack_ptr;

    new_task->regs.cr3 = new_task->cr3;
    new_task->regs.eip = (uint32_t)entry;
    new_task->regs.useresp = thread_stack_top - 16;
    new_task->regs.cs = 0x1B;
    new_task->regs.ds = new_task->regs.es = new_task->regs.fs = new_task->regs.gs = new_task->regs.ss = 0x23;
    new_task->regs.eflags = 0x202;
    
    lock_scheduler();
    add_task_to_runqueue(new_task);
    unlock_scheduler();
    
    return new_task->pid;
}
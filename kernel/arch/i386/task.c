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
#include <kernel/vring.h>
#include <kernel/kstack.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define KERR_EPERM   1
#define KERR_EIO     5
#define KERR_ENOEXEC 8
#define KERR_ENOMEM  12

#define TASK_STACK_SIZE 4096
#define SCHED_DEFAULT_TIMESLICE 5
#define KERNEL_STACK_SIZE 8192

#define USER_STACK_TOP 0x00800000u
#define USER_STACK_SIZE 0x4000u
#define USER_STACK_GAP  0x1000u
#define USER_STACK_INIT_ESP (USER_STACK_TOP - USER_STACK_GAP - 16u)

static int scheduler_lock = 0;
int scheduler_initialized = 0;
extern volatile uint32_t tick_count;

extern void switch_to_asm(uint32_t* old_esp, uint32_t new_esp);

task_t* current_task = NULL;
task_t* ready_queue_head = NULL;
static task_t* sleep_queue_head = NULL;
static task_t* dead_queue_head = NULL;
static volatile int schedule_force = 0;

static uint32_t next_task_id = 1;

static void serial_print(const char *s) {
    while (*s) {
        serial_putchar(*s++);
    }
}

static void serial_hex(uint32_t v) {
    int i;
    int d;
    
    serial_print("0x");
    for (i = 28; i >= 0; i -= 4) {
        d = (v >> i) & 0xF;
        serial_putchar(d < 10 ? '0' + d : 'A' + d - 10);
    }
}

static void task_error(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;
    
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (current_task && current_task->console_id >= 0 && console_is_initialized()) {
        console_write_to(current_task->console_id, buf, (size_t)n);
    }
}

static void sleepq_remove(task_t* t) {
    task_t **ptr;

    if (!t || !t->in_sleep_queue) return;
    ptr = &sleep_queue_head;
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
    uint32_t a;

    if (!t) return 0;
    a = (uint32_t)t;
    if ((a & 0xFFFF0000u) == 0xFEFE0000u) return 0;
    if (a < 0xC0000000) return 0;
    return 1;
}

task_t* task_find(pid_t pid) {
    task_t *t;
    task_t *d;

    lock_scheduler();
    t = ready_queue_head;
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

    d = dead_queue_head;
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
    task_t *t;
    task_t *d;
    int found = 0;

    lock_scheduler();

    t = ready_queue_head;
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
        d = dead_queue_head;
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
    unlock_scheduler();
    return NULL;
}

void waitq_init(wait_queue_t* q) {
    if (!q) return;
    q->head = NULL;
}

void waitq_add(wait_queue_t* q, task_t* t) {
    uint32_t flags;

    if (!q || !t) return;
    if (t->in_wait_queue || t->waiting_queue) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    t->wait_next = q->head;
    q->head = t;
    t->in_wait_queue = 1;
    t->waiting_queue = q;
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

task_t* waitq_pop(wait_queue_t* q) {
    task_t *t;

    if (!q || !q->head) return NULL;
    t = q->head;
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
    task_t *t;
    uint32_t flags;

    if (!q) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    while ((t = waitq_pop(q))) {
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
    }
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

void waitq_wake_one(wait_queue_t *q) {
    task_t *t;
    uint32_t flags;

    if (!q) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    t = waitq_pop(q);
    if (t && t->state == TASK_BLOCKED) t->state = TASK_READY;
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

void waitq_wait(wait_queue_t *q) {
    uint32_t flags;
    if (!q || !current_task) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    waitq_add(q, current_task);
    current_task->state = TASK_BLOCKED;
    __asm__ volatile("push %0; popf" : : "r"(flags));
    yield();
}

void waitq_remove(wait_queue_t* q, task_t* t) {
    task_t *prev;
    task_t *cur;

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
    prev = q->head;
    cur = prev->wait_next;
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
    uint8_t *task0_kstack;
    uint32_t boot_esp = 0;

    current_task = (task_t*)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));
    current_task->id = 0;
    current_task->pid = 0;
    current_task->state = TASK_RUNNING;
    current_task->cr3 = read_cr3();
    current_task->next = current_task;
    current_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->stack_base = NULL;
    current_task->stack_size = 0;
    
    task0_kstack = kstack_alloc();
    if (task0_kstack) {
        current_task->kernel_stack_base = task0_kstack;
        current_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    } else {
        current_task->kernel_stack_base = kmalloc(KERNEL_STACK_SIZE);
        if (current_task->kernel_stack_base) {
            current_task->kernel_stack_size = KERNEL_STACK_SIZE;
        } else {
            current_task->kernel_stack_size = 0;
        }
    }
    
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
    asm volatile ("movl %%esp, %0" : "=r" (boot_esp));
    current_task->regs.esp = boot_esp;
    
    if (current_task->kernel_stack_base) {
        tss_set_esp0((uint32_t)current_task->kernel_stack_base + current_task->kernel_stack_size);
    } else {
        tss_set_esp0(boot_esp);
    }

    current_task->regs.eip = 0;
    current_task->regs.eflags = 0x202;
    current_task->regs.cs = 0x08;
    current_task->regs.ds = current_task->regs.es = current_task->regs.fs = current_task->regs.gs = 0x10;
    current_task->vring_minor = 0;
    current_task->is_kernel_task = false;
    ready_queue_head = current_task;
    
    scheduler_initialized = 1;
}

void lock_scheduler(void) {
    __asm__ __volatile__ ("cli" ::: "memory");
    scheduler_lock++;
}

void unlock_scheduler(void) {
    if (--scheduler_lock == 0) {
        __asm__ __volatile__ ("sti" ::: "memory");
    }
}

void add_task_to_runqueue(task_t* new_task) {
    task_t *tail;

    if (!ready_queue_head) {
        ready_queue_head = new_task;
        new_task->next = new_task;
    } else {
        tail = ready_queue_head;
        while (tail->next != ready_queue_head) tail = tail->next;
        tail->next = new_task;
        new_task->next = ready_queue_head;
    }
}

static inline void remove_task_from_runqueue(task_t* task) {
    task_t *prev;

    if (!ready_queue_head || !task) return;
    
    if (task->next == task) {
        if (task == ready_queue_head) {
            ready_queue_head = NULL;
        }
        return;
    }
    
    prev = ready_queue_head;
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
    task_t *new_task;
    uint8_t *stack_base;
    uint8_t *kernel_stack_base;
    uint32_t *kesp;
    uint32_t *esp;
    uint32_t user_esp;

    if (!entry) return NULL;

    if (debug_task) {
        static uint32_t last_verify_tick = 0;
        static uint32_t last_seen_tick = 0;
        static uint32_t same_tick_calls = 0;
        uint32_t now;
        
        now = tick_count;
        if (now != last_seen_tick) {
            last_seen_tick = now;
            same_tick_calls = 0;
        }
        if ((now - last_verify_tick) >= 250 || (same_tick_calls++ & 0x3F) == 0) {
            heap_verify();
            last_verify_tick = tick_count;
        }
    }

    new_task = (task_t*)kmalloc(sizeof(task_t));
    stack_base = user_mode ? NULL : (uint8_t*)kmalloc(TASK_STACK_SIZE);
    kernel_stack_base = kstack_alloc();
    if (!new_task || (!user_mode && !stack_base) || !kernel_stack_base) {
        printf("Task alloc fail!\n");
        if (debug_task) {
            static uint32_t last_verify_tick_fail = 0;
            uint32_t now;
            
            now = tick_count;
            if ((now - last_verify_tick_fail) >= 50) {
                heap_verify();
                last_verify_tick_fail = now;
            }
        }
        if (new_task) kfree(new_task);
        if (stack_base) kfree(stack_base);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        return NULL;
    }
    memset(new_task, 0, sizeof(task_t));
    new_task->cwd[0] = '/';
    new_task->cwd[1] = '\0';
    task_init_fds(new_task);
    if (stack_base) memset(stack_base, 0, TASK_STACK_SIZE);

    new_task->id = next_task_id;
    new_task->pid = next_task_id;
    next_task_id++;

    creds_init_task(new_task->pid);

    new_task->state = initial_state;
    new_task->next = NULL;
    new_task->cr3 = cr3;
    new_task->console_id = -1; 
    DEBUG_TASK("new task id=%u cr3=0x%08X\n", new_task->id, cr3);
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->stack_base = stack_base;
    new_task->stack_size = TASK_STACK_SIZE;
    new_task->kernel_stack_base = kernel_stack_base;
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
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
    new_task->vring_minor = 0;
    new_task->is_kernel_task = false;

    if (user_mode) {
        kesp = (uint32_t*)(kernel_stack_base + KSTACK_USABLE_SIZE);
        
        user_esp = USER_STACK_INIT_ESP;
        
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
        new_task->mmap_next_addr = 0x10000000;
        
    } else {
        esp = (uint32_t*)(stack_base + TASK_STACK_SIZE);
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

    DEBUG_TASK("Task created: id=%u pid=%u EIP=0x%08X ESP=0x%08X%s\n", new_task->id, new_task->pid, (uint32_t)entry, new_task->regs.esp, user_mode ? " (USER)" : "");

    return new_task;
}

void sleep_ticks(uint32_t ticks) {
    uint32_t new_wake;

    if (!current_task || ticks == 0) return;
    lock_scheduler();
    new_wake = tick_count + ticks;
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
    task_t **ptr;
    task_t *t;
    int safety = 0;

    if (!sleep_queue_head) return;
    lock_scheduler();
    ptr = &sleep_queue_head;
    while (*ptr) {
        if (++safety > 10000) {
            *ptr = NULL;
            break;
        }
        t = *ptr;
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
    task_t *parent;

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
        parent = task_find((pid_t)current_task->ppid);
        if (parent && parent->waiting_for_any_child && parent->state == TASK_BLOCKED) {
            parent->state = TASK_READY;
        }
    }

    unlock_scheduler();
}

void sleep_ms(uint32_t ms) {
    extern uint32_t pit_freq;
    uint32_t ticks;

    if (ms == 0) return;
    ticks = (ms * pit_freq + 999) / 1000; 
    if (ticks == 0) ticks = 1;
    sleep_ticks(ticks);
}

void task_free_user_memory(task_t* t) {
    uint32_t i;

    if (!t) return;

    if (t->user_pages && t->user_pages_count > 0) {
        for (i = 0; i < t->user_pages_count; i++) {
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
    task_t *t;
    task_t *keep = NULL;
    task_t *next;
    task_t *k;
    task_t *knext;

    if (!dead_queue_head) return;
    lock_scheduler();
    t = dead_queue_head;
    dead_queue_head = NULL;
    unlock_scheduler();

    while (t && task_ptr_valid(t)) {
        next = t->wait_next;
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
        if (t->kernel_stack_base) kstack_free(t->kernel_stack_base);
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
            k = knext;
        }
        unlock_scheduler();
    }
}

void switch_to(task_t* next) {
    task_t *prev;
    uint32_t esp0;

    if (next == current_task) return;

    prev = current_task;
    current_task = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->kernel_stack_base && next->kernel_stack_size) {
        esp0 = (uint32_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_esp0(esp0);
    }

    if (next->regs.esp != 0 && 
        !((next->regs.esp >= 0xC0000000 && next->regs.esp < 0xD0000000) ||
          (next->regs.esp >= HEAP_START && next->regs.esp < (HEAP_START + HEAP_MAX_SIZE)) ||
          kstack_is_in_region(next->regs.esp))) {
        printf("Switch guard: bad esp 0x%08X for task %d\n", next->regs.esp, next->id);
        return;
    }

    prev->cr3 = read_cr3();

    switch_to_asm(&prev->regs.esp, next->regs.esp);

    next->cr3 = read_cr3();
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
    task_t* prev;
    uint32_t i;
    
    if (!t || t->state != TASK_DEAD) return;
    
    if (dead_queue_head == t) {
        dead_queue_head = t->wait_next;
    } else {
        prev = dead_queue_head;
        while (prev && prev->wait_next != t) prev = prev->wait_next;
        if (prev) prev->wait_next = t->wait_next;
    }
    t->wait_next = NULL;
    
    unlock_scheduler();
    
    if (t->user_pages && t->user_pages_count > 0) {
        for (i = 0; i < t->user_pages_count; i++) {
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
        kstack_free(t->kernel_stack_base);
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

static inline void save_irq_frame_into_task(task_t* task, const registers_t* regs, uint32_t regs_ptr, uint32_t entry_cr3) {
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
    task->cr3 = entry_cr3;
}

registers_t* schedule_from_irq(registers_t* regs) {
    task_t* prev_task;
    uint32_t entry_cr3;
    uint32_t kernel_cr3;
    int must_switch;
    bool is_idle;
    task_t* next;
    task_t* start;
    int safety;
    registers_t* return_frame;
    uint32_t next_esp;
    uint32_t gs_val;
    uint32_t fs_val;
    uint32_t es_val;
    uint32_t ds_val;
    uint32_t cs_val;
    int valid_gs;
    int valid_fs;
    int valid_es;
    int valid_ds;
    int valid_cs;
    uint32_t esp0;

    if (!current_task || !ready_queue_head) return regs;

    prev_task = current_task;
    
    __asm__ volatile ("mov %%cr3, %0" : "=r"(entry_cr3));
    kernel_cr3 = vmm_get_kernel_cr3();
    if (kernel_cr3 && entry_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }
    
    must_switch = (prev_task->state == TASK_DEAD);

    if (regs->int_no != 48 && prev_task->syscall_frame && !must_switch) {
        if (kernel_cr3 && entry_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
        }
        return regs;
    }

    if (!must_switch) {
        save_irq_frame_into_task(prev_task, regs, (uint32_t)regs, entry_cr3);
    }

    is_idle = (prev_task->id == 0 && !prev_task->is_user);
    
    if (!schedule_force && !must_switch && !is_idle) {
        if (prev_task->time_slice > 0) prev_task->time_slice--;
        if (prev_task->time_slice != 0) {
            if (kernel_cr3 && entry_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
            }
            return regs;
        }
    }
    schedule_force = 0;

    if (!must_switch) {
        prev_task->time_slice = prev_task->base_time_slice;
    }

    next = prev_task->next ? prev_task->next : ready_queue_head;
    start = next;
    safety = 0;
    
    while (next) {
        if ((next->state == TASK_READY || next->state == TASK_RUNNING) && next != prev_task) {
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
    
    if (!next) {
        if (kernel_cr3 && entry_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
        }
        return regs;
    }

    if (next->regs.esp == 0) {
        if (must_switch) {
            printf("schedule: next task has null frame, system halted\n");
            for (;;) asm volatile ("hlt");
        }
        if (kernel_cr3 && entry_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
        }
        return regs;
    }

    next_esp = next->regs.esp;
    if (!((next_esp >= 0xC0000000u && next_esp < 0xD0000000u) ||
          (next_esp >= HEAP_START && next_esp < (HEAP_START + HEAP_MAX_SIZE)) ||
          kstack_is_in_region(next_esp))) {
        if (kernel_cr3 && entry_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
        }
        return regs;
    }
    return_frame = (registers_t*)next_esp;

    gs_val = return_frame->gs & 0xFFFF;
    fs_val = return_frame->fs & 0xFFFF;
    es_val = return_frame->es & 0xFFFF;
    ds_val = return_frame->ds & 0xFFFF;
    cs_val = return_frame->cs & 0xFFFF;

    valid_gs = (gs_val == 0x10 || gs_val == 0x23 || gs_val == 0x33 || gs_val == 0x3B || gs_val == 0);
    valid_fs = (fs_val == 0x10 || fs_val == 0x23 || fs_val == 0);
    valid_es = (es_val == 0x10 || es_val == 0x23 || es_val == 0);
    valid_ds = (ds_val == 0x10 || ds_val == 0x23 || ds_val == 0);
    valid_cs = (cs_val == 0x08 || cs_val == 0x1B);

    if (!valid_gs || !valid_fs || !valid_es || !valid_ds || !valid_cs) {
        if (next->id == 0) {
            if (kernel_cr3 && entry_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
            }
            return regs;
        }
        serial_print("SCHED: BAD FRAME esp=");
        serial_hex(next->regs.esp);
        serial_print(" gs=");
        serial_hex(gs_val);
        serial_print(" fs=");
        serial_hex(fs_val);
        serial_print(" es=");
        serial_hex(es_val);
        serial_print(" ds=");
        serial_hex(ds_val);
        serial_print(" cs=");
        serial_hex(cs_val);
        serial_print(" task=");
        serial_hex(next->id);
        serial_print("\n");

        task_kill(next, 0xBADF00D);
        if (kernel_cr3 && entry_cr3 != kernel_cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(entry_cr3) : "memory");
        }
        return regs;
    }

    if (prev_task->state == TASK_RUNNING) prev_task->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    if (next->is_user && ((return_frame->cs & 0x3) == 0x3)) {
        return_frame->ds = return_frame->es = return_frame->fs = 0x23;
        if (next->tls_base) {
            return_frame->gs = gdt_get_tls_selector(GDT_TLS_ENTRY_1);
        } else {
            return_frame->gs = 0x23;
        }
        return_frame->cs = 0x1B;
        return_frame->ss = 0x23;
    }

    if (next->kernel_stack_base && next->kernel_stack_size) {
        esp0 = (uint32_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_esp0(esp0);
    }

    if (next->tls_base) {
        gdt_set_tls(GDT_TLS_ENTRY_1, next->tls_base, next->tls_limit);
    }

    {
        uint32_t target_cr3 = next->cr3;
        if (next->pd_phys) target_cr3 = next->pd_phys;
        if (target_cr3 && target_cr3 != read_cr3()) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
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
    int i;
    
    if (!task) return -1;
    for (i = 3; i < TASK_MAX_FDS; i++) {
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
    int i;
    
    if (!task) return;
    for (i = 3; i < TASK_MAX_FDS; i++) {
        task_fd_free(task, i);
    }
}

#define FORK_MIN_FREE_PAGES 64

pid_t task_fork(registers_t *parent_regs) {
    uint32_t free_pages;
    uint32_t needed_pages;
    uint32_t *child_user_pages;
    uint32_t child_user_pages_count;
    uint32_t child_pd;
    task_t* child;
    uint8_t* kernel_stack_base;
    uint32_t i;
    registers_t *child_frame;

    if (!current_task || !current_task->is_user) {
        printf("task_fork: can only fork user tasks\n");
        return -1;
    }

    free_pages = pfa_count_free();
    needed_pages = current_task->user_pages_count + FORK_MIN_FREE_PAGES;
    if (free_pages < needed_pages) {
        printf("task_fork: insufficient memory (free=%u, need~%u)\n", free_pages, needed_pages);
        return -1;
    }

    child_user_pages = NULL;
    child_user_pages_count = 0;

    child_pd = vmm_clone_page_directory(current_task->pd_phys, &child_user_pages, &child_user_pages_count);
    if (!child_pd) {
        printf("task_fork: failed to clone page directory\n");
        return -1;
    }

    DEBUG_TASK("task_fork: creating child task with cloned pd=0x%08X\n", child_pd);

    child = (task_t*)kmalloc(sizeof(task_t));
    kernel_stack_base = kstack_alloc();
    if (!child || !kernel_stack_base) {
        printf("task_fork: allocation failed\n");
        if (child) kfree(child);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        if (child_user_pages) {
            for (i = 0; i < child_user_pages_count; i++) {
                pfa_free(child_user_pages[i]);
            }
            kfree(child_user_pages);
        }
        vmm_free_page_directory(child_pd);
        return -1;
    }
    memset(child, 0, sizeof(task_t));

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
    child->time_slice = SCHED_DEFAULT_TIMESLICE;
    child->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    child->stack_base = NULL;
    child->stack_size = 0;
    child->kernel_stack_base = kernel_stack_base;
    child->kernel_stack_size = KSTACK_USABLE_SIZE;
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
    for (i = 0; i < TASK_MAX_FDS; i++) {
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

    child_frame = (registers_t *)((uint8_t *)kernel_stack_base + KSTACK_USABLE_SIZE - sizeof(registers_t));
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

    DEBUG_TASK("task_fork: parent pid=%d, child pid=%d\n", current_task->pid, child->pid);

    return child->pid;
}

int task_exec(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs) {
    uint32_t free_pages;
    uint32_t needed_estimate;
    uint8_t *kernel_bin;
    int elf_valid;
    uint32_t old_pd;
    uint32_t *old_user_pages;
    uint32_t old_user_pages_count;
    uint32_t stack_top;
    uint32_t stack_size;
    uint32_t new_pd;
    elf_info_t elf_info;
    uint32_t *elf_pages;
    uint32_t elf_page_count;
    int load_result;
    uint32_t stack_page_count;
    uint32_t *stack_pages;
    uint32_t total_pages;
    uint32_t sp;
    uint32_t zero;
    const char *prog_name;
    int prog_len;
    uint32_t prog_addr;
    uint8_t random_bytes[16];
    uint32_t random_addr;
    uint32_t argc_val;
    uint32_t i;

    if (!current_task || !current_task->is_user) {
        task_error("task_exec: can only exec in user tasks\n");
        return -1;
    }
    if (!bin_start || bin_size == 0) {
        task_error("task_exec: invalid binary\n");
        return -1;
    }

    free_pages = pfa_count_free();
    needed_estimate = 20 + (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (free_pages < needed_estimate) {
        task_error("task_exec: insufficient memory (free=%u need=%u)\n", free_pages, needed_estimate);
        return -1;
    }

    kernel_bin = (uint8_t *)kmalloc(bin_size);
    if (!kernel_bin) {
        task_error("task_exec: failed to allocate kernel buffer (%u bytes)\n", bin_size);
        return -1;
    }
    memcpy(kernel_bin, bin_start, bin_size);

    elf_valid = elf_validate(kernel_bin, bin_size);
    if (elf_valid != 0) {
        task_error("task_exec: ELF validation failed (%d)\n", elf_valid);
        kfree(kernel_bin);
        return -1;
    }

    DEBUG_TASK("task_exec: replacing task %d with ELF binary (%u bytes)\n", current_task->pid, bin_size);

    old_pd = current_task->pd_phys;
    old_user_pages = current_task->user_pages;
    old_user_pages_count = current_task->user_pages_count;

    stack_top = USER_STACK_TOP;
    stack_size = USER_STACK_SIZE;

    new_pd = vmm_create_page_directory();
    if (!new_pd) {
        task_error("task_exec: failed to create page directory\n");
        kfree(kernel_bin);
        return -1;
    }

    elf_pages = NULL;
    elf_page_count = 0;

    load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        task_error("task_exec: ELF loading failed (%d)\n", load_result);
        if (elf_pages) {
            for (i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        kfree(kernel_bin);
        return -1;
    }

    kfree(kernel_bin);

    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pd_tracked(new_pd, stack_top - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec: failed to map stack\n");
        if (elf_pages) {
            for (i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        return -1;
    }

    current_task->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;

    total_pages = elf_page_count + stack_page_count;
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

    sp = stack_top - USER_STACK_GAP - 16;
    zero = 0;
    
    prog_name = "program";
    prog_len = 0;
    while (prog_name[prog_len]) prog_len++;
    sp -= (prog_len + 1 + 3) & ~3;
    prog_addr = sp;
    vmm_copy_to_pd(new_pd, sp, prog_name, prog_len + 1);
    
    random_bytes[0] = 0x12; random_bytes[1] = 0x34; random_bytes[2] = 0x56; random_bytes[3] = 0x78;
    random_bytes[4] = 0x9A; random_bytes[5] = 0xBC; random_bytes[6] = 0xDE; random_bytes[7] = 0xF0;
    random_bytes[8] = 0x11; random_bytes[9] = 0x22; random_bytes[10] = 0x33; random_bytes[11] = 0x44;
    random_bytes[12] = 0x55; random_bytes[13] = 0x66; random_bytes[14] = 0x77; random_bytes[15] = 0x88;
    sp -= 16;
    random_addr = sp;
    vmm_copy_to_pd(new_pd, sp, random_bytes, 16);
    
    sp = sp & ~0xF;
    
#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25

#define PUSH_AUXV_PD(pd, type, val) do { \
    uint32_t _t = (type), _v = (val); \
    sp -= 4; vmm_copy_to_pd(pd, sp, &_v, sizeof(uint32_t)); \
    sp -= 4; vmm_copy_to_pd(pd, sp, &_t, sizeof(uint32_t)); \
} while(0)

    PUSH_AUXV_PD(new_pd, AT_NULL, 0);
    PUSH_AUXV_PD(new_pd, AT_RANDOM, random_addr);
    PUSH_AUXV_PD(new_pd, AT_EGID, current_task->egid);
    PUSH_AUXV_PD(new_pd, AT_GID, current_task->gid);
    PUSH_AUXV_PD(new_pd, AT_EUID, current_task->euid);
    PUSH_AUXV_PD(new_pd, AT_UID, current_task->uid);
    PUSH_AUXV_PD(new_pd, AT_PAGESZ, 4096);
    PUSH_AUXV_PD(new_pd, AT_ENTRY, elf_info.entry_point);
    PUSH_AUXV_PD(new_pd, AT_PHNUM, elf_info.phnum);
    PUSH_AUXV_PD(new_pd, AT_PHENT, elf_info.phent);
    PUSH_AUXV_PD(new_pd, AT_PHDR, elf_info.phdr_vaddr);

#undef PUSH_AUXV_PD
#undef AT_NULL
#undef AT_PHDR
#undef AT_PHENT
#undef AT_PHNUM
#undef AT_PAGESZ
#undef AT_ENTRY
#undef AT_UID
#undef AT_EUID
#undef AT_GID
#undef AT_EGID
#undef AT_RANDOM
    
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &zero, sizeof(uint32_t));
    
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &prog_addr, sizeof(uint32_t));
    
    sp -= 4;
    argc_val = 1;
    vmm_copy_to_pd(new_pd, sp, &argc_val, sizeof(uint32_t));

    {
        uint32_t final_entry;
        uint32_t final_sp;
        
        final_entry = elf_info.entry_point;
        final_sp = sp;

        current_task->pd_phys = new_pd;
        current_task->cr3 = new_pd;

        regs->ds = 0x23;
        regs->es = 0x23;
        regs->fs = 0x23;
        regs->gs = 0x23;
        regs->ss = 0x23;
        regs->cs = 0x1B;
        regs->eax = 0;
        regs->ebx = 0;
        regs->ecx = 0;
        regs->edx = 0;
        regs->esi = 0;
        regs->edi = 0;
        regs->ebp = 0;
        regs->eflags = 0x202;
        regs->useresp = final_sp;
        regs->int_no = 0;
        regs->err_code = 0;
        regs->eip = final_entry;

        current_task->regs.eip = final_entry;
        current_task->regs.useresp = final_sp;
        current_task->regs.ebp = 0;
        current_task->regs.eax = 0;
        current_task->regs.ebx = 0;
        current_task->regs.ecx = 0;
        current_task->regs.edx = 0;
        current_task->regs.esi = 0;
        current_task->regs.edi = 0;
        current_task->regs.cs = 0x1B;
        current_task->regs.ds = 0x23;
        current_task->regs.es = 0x23;
        current_task->regs.fs = 0x23;
        current_task->regs.gs = 0x23;
        current_task->regs.ss = 0x23;
        current_task->regs.eflags = 0x202;
        current_task->regs.int_no = 0;
        current_task->regs.err_code = 0;
        current_task->regs.esp = (uint32_t)regs;

        current_task->exec_old_pd = old_pd;
        current_task->exec_old_pages = old_user_pages;
        current_task->exec_old_pages_count = old_user_pages_count;

        DEBUG_TASK("task_exec: new ELF entry at 0x%08X, stack at 0x%08X new_pd=0x%08X (CR3 switch deferred)\n", 
                 final_entry, final_sp, new_pd);
    }

    return 0;
}

int task_exec_with_args(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs,
                        int argc, char **argv, int envc, char **envp) {
    uint32_t free_pages;
    uint32_t needed_estimate;
    uint8_t *kernel_bin;
    char **k_argv;
    char **k_envp;
    int i, j, len;
    int elf_valid;
    uint32_t old_pd;
    uint32_t *old_user_pages;
    uint32_t old_user_pages_count;
    uint32_t stack_top;
    uint32_t stack_size;
    uint32_t new_pd;
    elf_info_t elf_info;
    uint32_t *elf_pages;
    uint32_t elf_page_count;
    int load_result;
    uint32_t stack_page_count;
    uint32_t *stack_pages;
    uint32_t total_pages;
    uint32_t sp;
    uint32_t *envp_ptrs;
    uint32_t *argv_ptrs;
    uint8_t random_bytes[16];
    uint32_t random_addr;
    uint32_t zero;
    uint32_t tmp_val;
    uint32_t entry_to_use;
    uint32_t stack_ptr;
    uint32_t check_stack_base;
    uint32_t check_stack_top;
    uint32_t final_entry;
    uint32_t final_sp;

    __asm__ volatile ("mov %%esp, %0" : "=r"(stack_ptr));
    if (current_task && current_task->kernel_stack_base) {
        check_stack_base = (uint32_t)current_task->kernel_stack_base;
        check_stack_top = check_stack_base + current_task->kernel_stack_size;
        if (stack_ptr < check_stack_base || stack_ptr > check_stack_top) {
            DEBUG_TASK("task_exec_with_args: STACK OVERFLOW esp=0x%08X base=0x%08X top=0x%08X\n",
                     stack_ptr, check_stack_base, check_stack_top);
        }
    }

    if (!current_task || !current_task->is_user) {
        task_error("task_exec_with_args: can only exec in user tasks\n");
        return -KERR_EPERM;
    }
    if (!bin_start || bin_size == 0) {
        task_error("task_exec_with_args: invalid binary\n");
        return -KERR_ENOEXEC;
    }

    free_pages = pfa_count_free();
    needed_estimate = 20 + (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (free_pages < needed_estimate) {
        task_error("task_exec_with_args: insufficient memory\n");
        return -KERR_ENOMEM;
    }

    kernel_bin = (uint8_t *)kmalloc(bin_size);
    if (!kernel_bin) {
        task_error("task_exec_with_args: failed to allocate kernel buffer\n");
        return -KERR_ENOMEM;
    }
    memcpy(kernel_bin, bin_start, bin_size);

    k_argv = NULL;
    k_envp = NULL;

    if (argc > 0 && argv) {
        k_argv = (char **)kmalloc((argc + 1) * sizeof(char *));
        if (!k_argv) {
            kfree(kernel_bin);
            return -KERR_ENOMEM;
        }
        for (i = 0; i < argc; i++) {
            len = 0;
            while (argv[i][len]) len++;
            k_argv[i] = (char *)kmalloc(len + 1);
            if (!k_argv[i]) {
                for (j = 0; j < i; j++) kfree(k_argv[j]);
                kfree(k_argv);
                kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (j = 0; j <= len; j++) k_argv[i][j] = argv[i][j];
        }
        k_argv[argc] = NULL;
    }

    if (envc > 0 && envp) {
        k_envp = (char **)kmalloc((envc + 1) * sizeof(char *));
        if (!k_envp) {
            if (k_argv) {
                for (i = 0; i < argc; i++) kfree(k_argv[i]);
                kfree(k_argv);
            }
            kfree(kernel_bin);
            return -KERR_ENOMEM;
        }
        for (i = 0; i < envc; i++) {
            len = 0;
            while (envp[i][len]) len++;
            k_envp[i] = (char *)kmalloc(len + 1);
            if (!k_envp[i]) {
                for (j = 0; j < i; j++) kfree(k_envp[j]);
                kfree(k_envp);
                if (k_argv) {
                    for (j = 0; j < argc; j++) kfree(k_argv[j]);
                    kfree(k_argv);
                }
                kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (j = 0; j <= len; j++) k_envp[i][j] = envp[i][j];
        }
        k_envp[envc] = NULL;
    }

    elf_valid = elf_validate(kernel_bin, bin_size);
    if (elf_valid != 0) {
        task_error("task_exec_with_args: ELF validation failed\n");
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    old_pd = current_task->pd_phys;
    old_user_pages = current_task->user_pages;
    old_user_pages_count = current_task->user_pages_count;

    stack_top = USER_STACK_TOP;
    stack_size = USER_STACK_SIZE;

    new_pd = vmm_create_page_directory();
    if (!new_pd) {
        task_error("task_exec_with_args: failed to create page directory\n");
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        kfree(kernel_bin);
        return -KERR_ENOMEM;
    }

    elf_pages = NULL;
    elf_page_count = 0;

    load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        task_error("task_exec_with_args: ELF loading failed\n");
        if (elf_pages) {
            for (i = 0; i < (int)elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    {
        uint32_t entry_phys;
        uint32_t entry_page_addr;

        entry_page_addr = elf_info.entry_point & ~0xFFFu;
        entry_phys = vmm_get_phys_in_pd(new_pd, entry_page_addr);
        if (entry_phys == 0) {
            DEBUG_TASK("task_exec_with_args: ERROR: entry page not mapped after elf_load\n");
        }
    }

    kfree(kernel_bin);

    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pd_tracked(new_pd, stack_top - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec_with_args: failed to map stack\n");
        if (elf_pages) {
            for (i = 0; i < (int)elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_page_directory(new_pd);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        return -KERR_ENOMEM;
    }

    current_task->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;

    total_pages = elf_page_count + stack_page_count;
    if (total_pages == 0 || total_pages > 65536) {
        task_error("task_exec_with_args: suspicious total_pages=%u\n", total_pages);
        kfree(elf_pages);
        kfree(stack_pages);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
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

    sp = stack_top - USER_STACK_GAP - 16;

    envp_ptrs = NULL;
    argv_ptrs = NULL;

    if (envc > 0 && k_envp) {
        envp_ptrs = (uint32_t *)kmalloc((envc + 1) * sizeof(uint32_t));
        for (i = envc - 1; i >= 0; i--) {
            len = 0;
            while (k_envp[i][len]) len++;
            sp -= (len + 1 + 3) & ~3;
            vmm_copy_to_pd(new_pd, sp, k_envp[i], len + 1);
            envp_ptrs[i] = sp;
        }
        envp_ptrs[envc] = 0;
    }

    if (argc > 0 && k_argv) {
        argv_ptrs = (uint32_t *)kmalloc((argc + 1) * sizeof(uint32_t));
        for (i = argc - 1; i >= 0; i--) {
            len = 0;
            while (k_argv[i][len]) len++;
            sp -= (len + 1 + 3) & ~3;
            vmm_copy_to_pd(new_pd, sp, k_argv[i], len + 1);
            argv_ptrs[i] = sp;
        }
        argv_ptrs[argc] = 0;
    }

    random_bytes[0] = 0x12; random_bytes[1] = 0x34; random_bytes[2] = 0x56; random_bytes[3] = 0x78;
    random_bytes[4] = 0x9A; random_bytes[5] = 0xBC; random_bytes[6] = 0xDE; random_bytes[7] = 0xF0;
    random_bytes[8] = 0x11; random_bytes[9] = 0x22; random_bytes[10] = 0x33; random_bytes[11] = 0x44;
    random_bytes[12] = 0x55; random_bytes[13] = 0x66; random_bytes[14] = 0x77; random_bytes[15] = 0x88;
    sp -= 16;
    random_addr = sp;
    vmm_copy_to_pd(new_pd, sp, random_bytes, 16);

    sp = sp & ~0xF;

    zero = 0;

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25

#define PUSH_AUXV(type, val) do { \
    tmp_val = (val); \
    sp -= 4; vmm_copy_to_pd(new_pd, sp, &tmp_val, 4); \
    tmp_val = (type); \
    sp -= 4; vmm_copy_to_pd(new_pd, sp, &tmp_val, 4); \
} while(0)

    PUSH_AUXV(AT_NULL, 0);
    PUSH_AUXV(AT_RANDOM, random_addr);
    PUSH_AUXV(AT_EGID, current_task->egid);
    PUSH_AUXV(AT_GID, current_task->gid);
    PUSH_AUXV(AT_EUID, current_task->euid);
    PUSH_AUXV(AT_UID, current_task->uid);
    PUSH_AUXV(AT_PAGESZ, 4096);
    PUSH_AUXV(AT_ENTRY, elf_info.entry_point);
    PUSH_AUXV(AT_PHNUM, elf_info.phnum);
    PUSH_AUXV(AT_PHENT, elf_info.phent);
    PUSH_AUXV(AT_PHDR, elf_info.phdr_vaddr);

#undef PUSH_AUXV
#undef AT_NULL
#undef AT_PHDR
#undef AT_PHENT
#undef AT_PHNUM
#undef AT_PAGESZ
#undef AT_ENTRY
#undef AT_UID
#undef AT_EUID
#undef AT_GID
#undef AT_EGID
#undef AT_RANDOM

    if (envp_ptrs) {
        for (i = envc; i >= 0; i--) {
            sp -= 4;
            vmm_copy_to_pd(new_pd, sp, &envp_ptrs[i], 4);
        }
    } else {
        sp -= 4;
        vmm_copy_to_pd(new_pd, sp, &zero, 4);
    }

    if (argv_ptrs) {
        for (i = argc; i >= 0; i--) {
            sp -= 4;
            vmm_copy_to_pd(new_pd, sp, &argv_ptrs[i], 4);
        }
    } else {
        sp -= 4;
        vmm_copy_to_pd(new_pd, sp, &zero, 4);
    }

    tmp_val = (uint32_t)argc;
    sp -= 4;
    vmm_copy_to_pd(new_pd, sp, &tmp_val, 4);

    if (argv_ptrs) kfree(argv_ptrs);
    if (envp_ptrs) kfree(envp_ptrs);
    if (k_argv) {
        for (i = 0; i < argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
    }
    if (k_envp) {
        for (i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_envp);
    }

    entry_to_use = elf_info.entry_point;

    final_entry = entry_to_use;
    final_sp = sp;

    regs->ds = 0x23;
    regs->es = 0x23;
    regs->fs = 0x23;
    regs->gs = 0x23;
    regs->ss = 0x23;
    regs->cs = 0x1B;
    regs->eax = 0;
    regs->ebx = 0;
    regs->ecx = 0;
    regs->edx = 0;
    regs->esi = 0;
    regs->edi = 0;
    regs->ebp = 0;
    regs->eflags = 0x202;
    regs->useresp = final_sp;
    regs->int_no = 0;
    regs->err_code = 0;
    regs->eip = final_entry;

    current_task->pd_phys = new_pd;
    current_task->cr3 = new_pd;
    current_task->regs.eip = final_entry;
    current_task->regs.useresp = final_sp;
    current_task->regs.ebp = 0;
    current_task->regs.eax = 0;
    current_task->regs.ebx = 0;
    current_task->regs.ecx = 0;
    current_task->regs.edx = 0;
    current_task->regs.esi = 0;
    current_task->regs.edi = 0;
    current_task->regs.cs = 0x1B;
    current_task->regs.ds = 0x23;
    current_task->regs.es = 0x23;
    current_task->regs.fs = 0x23;
    current_task->regs.gs = 0x23;
    current_task->regs.ss = 0x23;
    current_task->regs.eflags = 0x202;
    current_task->regs.int_no = 0;
    current_task->regs.err_code = 0;
    current_task->regs.esp = (uint32_t)regs;

    current_task->exec_old_pd = old_pd;
    current_task->exec_old_pages = old_user_pages;
    current_task->exec_old_pages_count = old_user_pages_count;

    current_task->tls_base = 0;
    current_task->tls_limit = 0;

    DEBUG_TASK("task_exec_with_args: entry=0x%08X esp=0x%08X new_pd=0x%08X\n", 
             final_entry, final_sp, new_pd);

    {
        uint32_t entry_page;
        uint32_t phys;

        entry_page = final_entry & ~0xFFFu;
        phys = vmm_get_phys_in_pd(new_pd, entry_page);
        
        if (phys == 0) {
            DEBUG_TASK("task_exec_with_args: FATAL: entry page not mapped in new_pd\n");
            if (current_task->user_pages) {
                for (i = 0; i < (int)current_task->user_pages_count; i++) {
                    if (current_task->user_pages[i]) {
                        pfa_free(current_task->user_pages[i]);
                    }
                }
                kfree(current_task->user_pages);
            }
            vmm_free_page_directory(new_pd);
            current_task->user_pages = old_user_pages;
            current_task->user_pages_count = old_user_pages_count;
            current_task->pd_phys = old_pd;
            current_task->cr3 = old_pd;
            current_task->exec_old_pd = 0;
            current_task->exec_old_pages = NULL;
            current_task->exec_old_pages_count = 0;
            return -KERR_ENOEXEC;
        }
    }

    return 0;
}

pid_t task_create_thread(void (*entry)(void)) {
    task_t *new_task;
    int i;
    uint32_t thread_stack_size;
    uint32_t thread_stack_base;
    uint32_t thread_stack_top;
    uint32_t stack_page_count;
    uint32_t *stack_pages;
    uint32_t *stack_ptr;

    if (!current_task || !current_task->is_user) {
        return -1;
    }
    
    new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    
    new_task->kernel_stack_base = kstack_alloc();
    if (!new_task->kernel_stack_base) {
        kfree(new_task);
        return -1;
    }
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    
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
    
    for (i = 0; i < 128; i++) {
        new_task->cwd[i] = current_task->cwd[i];
    }
    
    thread_stack_size = 0x4000;
    thread_stack_base = (current_task->user_brk + 0xFFF) & ~0xFFF;
    thread_stack_top = thread_stack_base + thread_stack_size;
    
    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pd_tracked(current_task->pd_phys, thread_stack_base, thread_stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        kstack_free(new_task->kernel_stack_base);
        kfree(new_task);
        return -1;
    }
    
    new_task->user_pages = stack_pages;
    new_task->user_pages_count = stack_page_count;
    
    current_task->user_brk = thread_stack_top + 0x1000;
    
    stack_ptr = (uint32_t *)(new_task->kernel_stack_base + KSTACK_USABLE_SIZE);
    
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
    task_t *new_task;
    int i;
    uint32_t thread_stack_size;
    uint32_t thread_stack_base;
    uint32_t thread_stack_top;
    uint32_t stack_page_count;
    uint32_t *stack_pages;
    uint32_t zero_val;
    uint32_t arg_val;
    uint32_t *stack_ptr;

    if (!current_task || !current_task->is_user) {
        return -1;
    }
    
    new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    
    new_task->kernel_stack_base = kstack_alloc();
    if (!new_task->kernel_stack_base) {
        kfree(new_task);
        return -1;
    }
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    
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
    
    for (i = 0; i < 128; i++) {
        new_task->cwd[i] = current_task->cwd[i];
    }
    
    thread_stack_size = 0x4000;
    thread_stack_base = (current_task->user_brk + 0xFFF) & ~0xFFF;
    thread_stack_top = thread_stack_base + thread_stack_size;
    
    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pd_tracked(current_task->pd_phys, thread_stack_base, thread_stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        kstack_free(new_task->kernel_stack_base);
        kfree(new_task);
        return -1;
    }
    
    new_task->user_pages = stack_pages;
    new_task->user_pages_count = stack_page_count;
    
    current_task->user_brk = thread_stack_top + 0x1000;
    
    zero_val = 0;
    arg_val = (uint32_t)arg;
    vmm_copy_to_pd(current_task->pd_phys, thread_stack_top - 16, &zero_val, 4);
    vmm_copy_to_pd(current_task->pd_phys, thread_stack_top - 12, &arg_val, 4);
    
    stack_ptr = (uint32_t *)(new_task->kernel_stack_base + KSTACK_USABLE_SIZE);
    
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

bool task_is_kernel_pid(int32_t pid) {
    return pid < 0;
}

void task_set_vring(task_t *task, uint8_t vring_minor) {
    if (!task) return;
    task->vring_minor = vring_minor;
    task->is_kernel_task = (vring_minor != 0);
}
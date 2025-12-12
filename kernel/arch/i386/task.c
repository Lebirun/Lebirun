#include <kernel/task.h>
#include <kernel/registers.h>
#include <kernel/common.h>
#include <kernel/mem_map.h>
#include <string.h>

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

void init_tasks(void) {
    current_task = (task_t*)kmalloc(sizeof(task_t));
    heap_block_t *current_block = (heap_block_t *)((uint8_t *)current_task - sizeof(heap_block_t));
    printf("init_tasks: current_task user_ptr=0x%08X block_hdr=0x%08X magic=0x%08X\n", (uint32_t)current_task, (uint32_t)current_block, current_block->magic);

    memset(current_task, 0, sizeof(task_t));
    current_task->id = 0;
    current_task->state = TASK_RUNNING;
    current_task->regs.cr3 = read_cr3();
    current_task->next = NULL;
    current_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->stack_base = NULL;
    current_task->stack_size = 0;
    current_task->wake_tick = 0;
    current_task->sleep_next = NULL;
    current_task->in_sleep_queue = 0;
    current_task->wait_next = NULL;
    current_task->join_waiter = NULL;
    current_task->exit_code = 0;
    uint32_t boot_esp = 0;
    asm volatile ("movl %%esp, %0" : "=r" (boot_esp));
    current_task->regs.esp = boot_esp;

    current_task->regs.eip = 0;
    current_task->regs.eflags = 0x202;
    current_task->regs.cs = 0x08;
    current_task->regs.ds = current_task->regs.es = current_task->regs.fs = current_task->regs.gs = 0x10;
    ready_queue_head = NULL;
    printf("Task 0 created (kernel main), ESP=0x%08X\n", current_task->regs.esp);
    printf("Init done: current=%d, head=%p\n", current_task->id, (void*)ready_queue_head);
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

task_t* create_task(void (*entry)(void), task_state_t initial_state) {
    if (!entry) return NULL;

    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    uint8_t* stack_base = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!new_task || !stack_base) {
        printf("Task alloc fail!\n");
        if (new_task) kfree(new_task);
        if (stack_base) kfree(stack_base);
        return NULL;
    }
    memset(new_task, 0, sizeof(task_t));
    memset(stack_base, 0, TASK_STACK_SIZE);

    static uint32_t next_id = 1;
    new_task->id = next_id++;
    new_task->state = initial_state;
    new_task->next = NULL;
    new_task->regs.cr3 = read_cr3();
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->stack_base = stack_base;
    new_task->stack_size = TASK_STACK_SIZE;
    new_task->wake_tick = 0;
    new_task->sleep_next = NULL;
    new_task->in_sleep_queue = 0;
    new_task->wait_next = NULL;
    new_task->join_waiter = NULL;
    new_task->exit_code = 0;

    uint32_t* esp = (uint32_t*)(stack_base + TASK_STACK_SIZE);

    *--esp = 0x202;           
    *--esp = 0x08;            
    *--esp = (uint32_t)entry;  

    *--esp = 0;               
    *--esp = 48;               

    *--esp = 0; 
    *--esp = 0; 
    *--esp = 0; 
    *--esp = 0; 
    *--esp = 0;
    *--esp = 0; 
    *--esp = 0; 
    *--esp = 0;

    *--esp = 0x10; 
    *--esp = 0x10;
    *--esp = 0x10; 
    *--esp = 0x10;

    new_task->regs.esp = (uint32_t)esp;
    new_task->regs.eip = (uint32_t)entry;
    new_task->regs.eflags = 0x202;
    new_task->regs.cs = 0x08;
    new_task->regs.ds = new_task->regs.es = new_task->regs.fs = new_task->regs.gs = 0x10;

    lock_scheduler();
    add_task_to_runqueue(new_task);
    unlock_scheduler();
    printf("Queue after create: head=%p (id=%d)\n", (void*)ready_queue_head, ready_queue_head ? ready_queue_head->id : -1);

    printf("Task %d created at EIP 0x%08X, ESP 0x%08X\n", new_task->id, (uint32_t)entry, new_task->regs.esp);

    heap_block_t *new_block_hdr = (heap_block_t *)((uint8_t *)new_task - sizeof(heap_block_t));
    heap_block_t *stack_block_hdr = (heap_block_t *)((uint8_t *)stack_base - sizeof(heap_block_t));
    printf(" new_task hdr=0x%08X magic=0x%08X size=%u\n", (uint32_t)new_block_hdr, new_block_hdr->magic, new_block_hdr->size);
    printf(" stack_base hdr=0x%08X magic=0x%08X size=%u\n", (uint32_t)stack_block_hdr, stack_block_hdr->magic, stack_block_hdr->size);

    if ((uintptr_t)new_task & 0x3) {
        printf("WARNING: new_task pointer unaligned 0x%08X\n", (uint32_t)new_task);
        heap_verify();
    }
    if ((new_task->regs.esp & 0x3) != 0) {
        printf("WARNING: new_task ESP unaligned 0x%08X\n", new_task->regs.esp);
    }

    heap_verify();
    task_t* t = ready_queue_head;
    if (t) {
        int i = 0;
        printf("Task list dump:\n");
        do {
            printf("  [%d] ptr=0x%08X id=%d next=0x%08X\n", i, (uint32_t)t, t->id, (uint32_t)t->next);
            t = t->next;
            i++;
            if (i > 10) break;
        } while (t && t != ready_queue_head);
    }

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
    current_task->state = TASK_DEAD;
    if (current_task->join_waiter && current_task->join_waiter->state == TASK_BLOCKED) {
        current_task->join_waiter->state = TASK_READY;
    }
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

void reap_dead_tasks(void) {
    if (!dead_queue_head) return;
    lock_scheduler();
    task_t* t = dead_queue_head;
    dead_queue_head = NULL;
    unlock_scheduler();
    while (t) {
        task_t* next = t->wait_next;
        if (t->stack_base) kfree(t->stack_base);
        kfree(t);
        t = next;
    }
}

void switch_to(task_t* next) {
    if (next == current_task) return;

    task_t* prev = current_task;
    current_task = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

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
    task->state = TASK_DEAD;
    if (task->join_waiter && task->join_waiter->state == TASK_BLOCKED) {
        task->join_waiter->state = TASK_READY;
    }
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

    if (task->join_waiter && task->join_waiter != current_task) {
        unlock_scheduler();
        return -1;
    }
    task->join_waiter = current_task;
    current_task->state = TASK_BLOCKED;
    unlock_scheduler();

    schedule();

    lock_scheduler();
    if (exit_code) *exit_code = task->exit_code;

    if (task->join_waiter == current_task) task->join_waiter = NULL;
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

    save_irq_frame_into_task(current_task, regs, (uint32_t)regs);

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

    if (next->regs.esp == 0) return regs;
    return (registers_t*)next->regs.esp;
}
#include <kernel/task.h>
#include <kernel/registers.h>
#include <kernel/common.h>
#include <kernel/mem_map.h>
#include <string.h>

static int scheduler_lock = 0;

extern void switch_to_asm(uint32_t* old_esp, uint32_t new_esp);

task_t* current_task;
task_t* ready_queue_head;

#define TASK_STACK_SIZE 16384

void init_tasks(void) {
    current_task = (task_t*)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));
    current_task->id = 0;
    current_task->state = TASK_RUNNING;
    current_task->regs.cr3 = read_cr3();
    current_task->next = NULL;
    current_task->regs.esp = 0;
    current_task->regs.eip = 0;
    current_task->regs.eflags = 0x202;
    current_task->regs.cs = 0x08;
    current_task->regs.ds = current_task->regs.es = current_task->regs.fs = current_task->regs.gs = 0x10;
    ready_queue_head = NULL;
    printf("Task 0 created (kernel main)\n");
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

    uint32_t* esp = (uint32_t*)(stack_base + TASK_STACK_SIZE);

    *--esp = (uint32_t)entry;
    
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

    if (!ready_queue_head) {
        ready_queue_head = new_task;
        new_task->next = new_task;
    } else {
        task_t* tail = ready_queue_head;
        while (tail->next != ready_queue_head) tail = tail->next;
        tail->next = new_task;
        new_task->next = ready_queue_head;
    }
    printf("Queue after create: head=%p (id=%d)\n", (void*)ready_queue_head, ready_queue_head ? ready_queue_head->id : -1);

    printf("Task %d created at EIP 0x%08X\n", new_task->id, (uint32_t)entry);
    return new_task;
}

void switch_to(task_t* next) {
    if (next == current_task) return;

    task_t* prev = current_task;
    current_task = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->regs.esp < 0xc0000000 || next->regs.esp > 0xd0000000) {
        printf("Switch guard: bad esp 0x%08X for task %d\n", next->regs.esp, next->id);
        return;
    }

    switch_to_asm(&prev->regs.esp, next->regs.esp);
}

void schedule(void) {
    lock_scheduler();

    if (!ready_queue_head || !current_task || current_task->id < 0 || current_task->id > 10) {
        printf("Schedule guard: bad current=0x%08X (id=%d) or head=0x%08X\n", (uint32_t)current_task, current_task ? current_task->id : -1, (uint32_t)ready_queue_head);
        return;
    }

    task_t* next = ready_queue_head->next;
    if (next == current_task || !next || next->id < 0 || next->id > 10) {
        printf("Schedule guard: bad next=0x%08X (id=%d)\n", (uint32_t)next, next ? next->id : -1);
        return;
    }

    ready_queue_head = next;

    current_task->state = TASK_READY;
    next->state = TASK_RUNNING;

    task_t* tail = next;
    while (tail->next != next) {
        if (!tail->next || tail->next->id < 0 || tail->next->id > 10) {
            printf("Schedule guard: bad tail->next=0x%08X in loop\n", (uint32_t)tail->next);
            break;
        }
        tail = tail->next;
    }
    tail->next = current_task;
    current_task->next = next;

    switch_to(next);
    unlock_scheduler();
}
#ifndef TASK_H
#define TASK_H

#include <kernel/common.h>
#include <kernel/registers.h>

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct task {
    task_state_t state;
    registers_t regs;
    uint32_t id;
    struct task* next;
    uint32_t time_slice;
    uint32_t base_time_slice;
    void* stack_base;
    uint32_t stack_size;
    uint32_t wake_tick;
    struct task* sleep_next;
    uint8_t in_sleep_queue;
    struct task* wait_next;
    struct task* join_waiter; 
    uint32_t exit_code;
} task_t;

extern task_t* current_task;
extern task_t* ready_queue_head;

void init_tasks(void);
task_t* create_task(void (*entry)(void), task_state_t initial_state);
void schedule(void);
registers_t* schedule_from_irq(registers_t* regs);
extern void save_context(void);
extern void switch_to(task_t* next);
void lock_scheduler(void);
void unlock_scheduler(void);

void yield(void);
void block_current(void);
void wake_task(task_t* task);
void task_kill(task_t* task, uint32_t exit_code);
void sleep_ticks(uint32_t ticks);
void wake_sleeping_tasks(void);
void reap_dead_tasks(void);
void task_exit(uint32_t exit_code);
void sleep_ms(uint32_t ms);
int task_join(task_t* task, uint32_t* exit_code);

#endif
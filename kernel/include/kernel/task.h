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
} task_t;

extern task_t* current_task;
extern task_t* ready_queue_head;

void init_tasks(void);
task_t* create_task(void (*entry)(void), task_state_t initial_state);
void schedule(void);
extern void save_context(void);
extern void switch_to(task_t* next);
void lock_scheduler(void);
void unlock_scheduler(void);

#endif
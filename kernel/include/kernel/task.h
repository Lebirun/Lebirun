#ifndef TASK_H
#define TASK_H

#include <kernel/common.h>
#include <kernel/registers.h>
#include <stdint.h>

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct wait_queue {
    struct task* head;
} wait_queue_t;

typedef struct task {
    uint32_t id;
    pid_t pid; 
    task_state_t state;
    struct task *next;
    struct task *sleep_next;
    int in_sleep_queue;
    int in_wait_queue;
    struct task *wait_next;
    wait_queue_t *waiting_queue;
    struct task *join_target;
    wait_queue_t join_waiters;
    int join_refs;
    uint32_t exit_code;
    registers_t regs;
    uint32_t cr3;
    int time_slice;
    int base_time_slice;
    uint8_t *stack_base;
    uint32_t stack_size;
    uint8_t *kernel_stack_base;
    uint32_t kernel_stack_size;
    uint32_t wake_tick;
    bool is_user;
    uint32_t user_brk;
    registers_t *syscall_frame;
    uint32_t pd_phys;
    uint32_t *user_pages;
    uint32_t user_pages_count;
    int console_id;
} task_t;

extern task_t* current_task;
extern task_t* ready_queue_head;

pid_t getpid(void);
task_t* task_find(pid_t pid);

void init_tasks(void);
task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode);
task_t* create_task_with_cr3(void (*entry)(void), task_state_t initial_state, bool user_mode, uint32_t cr3);
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

void waitq_init(wait_queue_t* q);
void waitq_add(wait_queue_t* q, task_t* t);
task_t* waitq_pop(wait_queue_t* q);
void waitq_wake_all(wait_queue_t* q);
void waitq_remove(wait_queue_t* q, task_t* t);

void task_free_user_memory(task_t* t);

void set_syscall_frame(registers_t *frame);
void clear_syscall_frame(void);

pid_t task_fork(registers_t *parent_regs);
int task_exec(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs);

#endif
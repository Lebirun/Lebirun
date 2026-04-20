#ifndef MULTITHREAD_H
#define MULTITHREAD_H

#include <lebirun/common.h>
#include <lebirun/registers.h>
#include <stdint.h>

#define THREAD_STACK_SIZE 4096
#define MAX_THREADS 32

typedef enum {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD
} thread_state_t;

typedef struct thread {
    uint64_t id;
    thread_state_t state;
    struct thread *next;
    registers_t regs;
    uint64_t cr3;
    uint8_t *stack_base;
    uint64_t stack_size;
    uint64_t wake_tick;
    void (*entry)(void *arg);
    void *arg;
    int exit_code;
    uint64_t tls_base;
    uint64_t tls_limit;
} thread_t;

extern thread_t *current_thread;
extern thread_t *thread_ready_head;

void init_threads(void);
thread_t *create_thread(void (*entry)(void *arg), void *arg);
void schedule_thread(void);
registers_t *schedule_thread_from_irq(registers_t *regs);
void yield_thread(void);
void block_current_thread(void);
void wake_thread(thread_t *thread);
void thread_exit(int code);
int thread_join(thread_t *thread, int *exit_code);

#endif
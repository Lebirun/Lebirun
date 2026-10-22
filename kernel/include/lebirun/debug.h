#ifndef LEBIRUN_DEBUG_H
#define LEBIRUN_DEBUG_H

#include <lebirun/task.h>

#define TASK_WAIT_STOPPED (1ULL << 63)
#define TASK_WAIT_CORE (1ULL << 62)

typedef struct debug_registers {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax, rip;
    uint64_t cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
} debug_registers_t;

void task_debug_registers(task_t *task, registers_t *frame,
                           debug_registers_t *registers);
#if CONFIG_KERNEL_PTRACE
int64_t task_ptrace(uint64_t request, uint64_t pid, uint64_t address,
                    uint64_t data, uint64_t unused1, uint64_t unused2);
int task_trace_signal(registers_t *frame, int signal);
int task_trace_resume(registers_t *frame);
int task_trace_stop_pending(task_t *task);
int task_trace_take_stop(task_t *task);
int task_is_traced(task_t *task);
int task_debug_busy(task_t *task);
int task_debug_defer_kill(task_t *task, uint64_t exit_code);
void task_debug_exit(task_t *task);
void task_debug_release(task_t *task);
void task_trace_exec(void);
#else
static inline int task_trace_signal(registers_t *frame, int signal) {
    (void)frame;
    (void)signal;
    return 0;
}

static inline int task_trace_resume(registers_t *frame) {
    (void)frame;
    return 0;
}

static inline int task_trace_stop_pending(task_t *task) {
    (void)task;
    return 0;
}

static inline int task_trace_take_stop(task_t *task) {
    (void)task;
    return 0;
}

static inline int task_is_traced(task_t *task) {
    (void)task;
    return 0;
}

static inline int task_debug_busy(task_t *task) {
    (void)task;
    return 0;
}

static inline int task_debug_defer_kill(task_t *task, uint64_t exit_code) {
    (void)task;
    (void)exit_code;
    return 0;
}

static inline void task_debug_exit(task_t *task) {
    (void)task;
}

static inline void task_debug_release(task_t *task) {
    (void)task;
}

static inline void task_trace_exec(void) {
}
#endif

int task_core_dump(registers_t *frame, int signal);
void task_user_exception(registers_t *frame, int signal);

#endif

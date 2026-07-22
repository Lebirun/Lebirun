#ifndef KERNEL_PIPE_H
#define KERNEL_PIPE_H

#include <lebirun/task.h>
#include <lebirun/spinlock.h>
#include <stdint.h>

#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t *buffer;
    uint64_t buf_size;
    uint64_t read_pos;
    uint64_t write_pos;
    uint64_t count;
    int readers;
    int writers;
    wait_queue_t read_waitq;
    wait_queue_t write_waitq;
    spinlock_t lock;
} pipe_t;

static inline uint64_t pipe_lock_irqsave(pipe_t *pipe) {
    uint64_t flags;

    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    spin_lock(&pipe->lock);
    return flags;
}

static inline void pipe_unlock_irqrestore(pipe_t *pipe, uint64_t flags) {
    spin_unlock(&pipe->lock);
    __asm__ volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static inline void pipe_retain_reference(pipe_t *pipe, int type) {
    uint64_t flags;

    if (!pipe) return;
    flags = pipe_lock_irqsave(pipe);
    if (type == FD_TYPE_PIPE_R) pipe->readers++;
    else pipe->writers++;
    pipe_unlock_irqrestore(pipe, flags);
}

static inline int pipe_release_reference(pipe_t *pipe, int type) {
    uint64_t flags;
    int release;

    if (!pipe) return 0;
    flags = pipe_lock_irqsave(pipe);
    if (type == FD_TYPE_PIPE_R) pipe->readers--;
    else pipe->writers--;
    waitq_wake_all(&pipe->read_waitq);
    waitq_wake_all(&pipe->write_waitq);
    release = pipe->readers <= 0 && pipe->writers <= 0;
    pipe_unlock_irqrestore(pipe, flags);
    return release;
}

#endif

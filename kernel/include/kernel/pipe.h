#ifndef KERNEL_PIPE_H
#define KERNEL_PIPE_H

#include <kernel/task.h>
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
} pipe_t;

#endif

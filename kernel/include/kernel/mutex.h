#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <stdint.h>
#include <kernel/task.h>

typedef struct mutex {
    int locked;
    uint32_t owner;
    task_t* wait_head;
} mutex_t;

void mutex_init(mutex_t* m);
void mutex_lock(mutex_t* m);
void mutex_unlock(mutex_t* m);

#endif

#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <stdint.h>
#include <lebirun/task.h>

void mutex_init(mutex_t* m);
void mutex_lock(mutex_t* m);
int mutex_trylock(mutex_t* m);
void mutex_unlock(mutex_t* m);

#endif

#include <stddef.h>
#include <lebirun/mutex.h>
#include <lebirun/task.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->owner = 0;
    waitq_init(&m->waiters);
}

void mutex_lock(mutex_t* m) {
    extern int scheduler_initialized;
    cpu_info_t *cpu;
    task_t *task;
    volatile int i;
    uint64_t owner;

    if (!m) return;

    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : current_task;
    owner = task ? task->id : 0;
    if (!scheduler_initialized) {
        while (!__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            __asm__ __volatile__ ("pause");
        }
        m->owner = 0;
        return;
    }

    while (1) {
        if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            m->owner = owner;
            return;
        }

        if (task && m->owner == owner && owner != 0) {
            return;
        }

        for (i = 0; i < 64 && m->locked; i++) {
            __asm__ volatile ("pause" ::: "memory");
        }
        if (m->locked && task) {
            schedule();
        }
    }
}

void mutex_unlock(mutex_t* m) {
    extern int scheduler_initialized;
    cpu_info_t *cpu;
    task_t *task;

    if (!m) return;

    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : current_task;
    
    if (!scheduler_initialized) {
        if (m->locked) {
            m->owner = 0;
            __sync_lock_release(&m->locked);
        }
        return;
    }

    if (!m->locked) {
        return;
    }
    if (m->owner != (task ? task->id : 0)) {
        return;
    }

    m->owner = 0;
    __sync_lock_release(&m->locked);
}

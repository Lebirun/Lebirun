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

    if (!m) return;

    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : current_task;
    if (!scheduler_initialized) {
        while (m->locked) {
            __asm__ __volatile__ ("pause");
        }
        m->locked = 1;
        m->owner = 0;
        return;
    }

    while (1) {
        lock_scheduler();

        if (!m->locked) {
            m->locked = 1;
            m->owner = task ? task->id : 0;
            unlock_scheduler();
            return;
        }

        if (task && m->owner == task->id && task->id != 0) {
            unlock_scheduler();
            return;
        }

        unlock_scheduler();
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
            m->locked = 0;
            m->owner = 0;
        }
        return;
    }

    lock_scheduler();
    if (!m->locked) {
        unlock_scheduler();
        return;
    }
    if (m->owner != (task ? task->id : 0)) {
        unlock_scheduler();
        return;
    }

    m->locked = 0;
    m->owner = 0;
    unlock_scheduler();
}

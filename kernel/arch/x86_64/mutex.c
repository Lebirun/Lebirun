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
    extern uint64_t tick_count;
    cpu_info_t *cpu;
    task_t *task;
    task_t *t;
    uint64_t wait_start;
    uint64_t last_print;
    uint64_t spin_start;
    uint64_t spin_last_warn;
    volatile int i;

    if (!m) return;

    cpu = smp_this_cpu();
    task = cpu ? cpu->current_task : current_task;
    
    if (!scheduler_initialized) {
        while (m->locked) {
            __asm__ __volatile__ ("pause");
        }
        m->locked = 1;
        m->owner = 0;
        return;
    }

    wait_start = tick_count;
    last_print = wait_start;

    while (1) {
        lock_scheduler();

        if (!m->locked) {
            m->locked = 1;
            m->owner = task ? task->id : 0;
            unlock_scheduler();
            return;
        }

        t = task;
        if (!t) {
            unlock_scheduler();
            spin_start = tick_count;
            spin_last_warn = spin_start;
            while (m->locked) {
                __asm__ volatile ("pause" ::: "memory");
                if (tick_count - spin_last_warn >= 2000) {
                    spin_last_warn = tick_count;
                    printf("[WARN] mutex_lock NULL-task spin >%lums m=%p owner=%d locked=%d\n",
                           tick_count - spin_start, (void*)m, m->owner, m->locked);
                }
            }
            continue;
        }

        if (m->owner == t->id && t->id != 0) {
            unlock_scheduler();
            return;
        }

        if (t->in_wait_queue || t->waiting_queue) {
            unlock_scheduler();
            for (i = 0; i < 100; i++);
            continue;
        }

        if (tick_count - last_print >= 3000) {
            last_print = tick_count;
            printf("[WARN] mutex_lock task=%d wait >%lums m=%p owner=%d locked=%d\n",
                   t->id, tick_count - wait_start, (void*)m, m->owner, m->locked);
        }

        waitq_add(&m->waiters, t);
        t->state = TASK_BLOCKED;
        unlock_scheduler();
        schedule();
    }
}

void mutex_unlock(mutex_t* m) {
    extern int scheduler_initialized;
    cpu_info_t *cpu;
    task_t *task;
    task_t *waiter;

    if (!m) return;

    cpu = smp_this_cpu();
    task = cpu ? cpu->current_task : current_task;
    
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

    waiter = waitq_pop(&m->waiters);
    if (!waiter) {
        m->locked = 0;
        m->owner = 0;
        unlock_scheduler();
        return;
    }
    m->owner = waiter->id;
    if (waiter->state != TASK_DEAD) {
        waiter->state = TASK_READY;
    }
    unlock_scheduler();
}

#include <stddef.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/common.h>

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->owner = 0;
    waitq_init(&m->waiters);
}

void mutex_lock(mutex_t* m) {
    if (!m) return;

    extern int scheduler_initialized;
    
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
            m->owner = current_task ? current_task->id : 0;
            unlock_scheduler();
            return;
        }

        if (m->owner == (current_task ? current_task->id : 0)) {
            unlock_scheduler();
            return;
        }

        task_t* t = current_task;
        if (!t) {
            unlock_scheduler();
            return;
        }

        if (t->in_wait_queue || t->waiting_queue) {
            unlock_scheduler();
            for (volatile int i = 0; i < 100; i++);
            continue;
        }

        waitq_add(&m->waiters, t);
        t->state = TASK_BLOCKED;
        unlock_scheduler();
        schedule();
    }
}

void mutex_unlock(mutex_t* m) {
    if (!m) return;

    extern int scheduler_initialized;
    
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
    if (m->owner != (current_task ? current_task->id : 0)) {
        unlock_scheduler();
        return;
    }

    task_t* waiter = waitq_pop(&m->waiters);
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

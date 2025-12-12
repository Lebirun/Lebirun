#include <stddef.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/common.h>

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->owner = 0;
    m->wait_head = NULL;
}

void mutex_lock(mutex_t* m) {
    if (!m) return;
    lock_scheduler();
    if (!m->locked) {
        m->locked = 1;
        m->owner = current_task ? current_task->id : 0;
        unlock_scheduler();
        return;
    }

    task_t* t = current_task;
    if (!t) { unlock_scheduler(); return; }
    if (!m->wait_head) {
        m->wait_head = t;
        t->wait_next = t;
    } else {
        task_t* tail = m->wait_head;
        while (tail->wait_next && tail->wait_next != m->wait_head) tail = tail->wait_next;
        tail->wait_next = t;
        t->wait_next = m->wait_head;
    }
    unlock_scheduler();
    block_current();
}

void mutex_unlock(mutex_t* m) {
    if (!m) return;
    lock_scheduler();
    if (!m->locked) { unlock_scheduler(); return; }
    if (m->owner != (current_task ? current_task->id : 0)) { unlock_scheduler(); return; }

    if (!m->wait_head) {
        m->locked = 0;
        m->owner = 0;
        unlock_scheduler();
        return;
    }
    task_t* waiter = m->wait_head;
    task_t* tail = waiter;
    while (tail->wait_next && tail->wait_next != waiter) tail = tail->wait_next;
    if (tail == waiter) {
        m->wait_head = NULL;
    } else {
        m->wait_head = waiter->wait_next;
        tail->wait_next = m->wait_head;
    }
    waiter->wait_next = NULL;
    m->owner = waiter->id;
    if (waiter->state == TASK_BLOCKED) waiter->state = TASK_READY;
    unlock_scheduler();
}

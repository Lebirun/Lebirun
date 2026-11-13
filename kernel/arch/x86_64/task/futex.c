#include "task_priv.h"

int task_futex_wait(uint64_t key, const int *uaddr, int expected,
                    uint64_t timeout_ticks, uint32_t bitset) {
    int value;
    int timed_out;
    uint64_t wake_deadline_ns;

    if (!current_task || !uaddr || key == 0) return -KERR_EFAULT;
    if (bitset == 0) return -KERR_EINVAL;
    lock_scheduler();
    if (copy_from_user(&value, uaddr, sizeof(value)) < 0) {
        unlock_scheduler();
        return -KERR_EFAULT;
    }
    if (value != expected) {
        unlock_scheduler();
        return -KERR_EAGAIN;
    }
    if (timeout_ticks == 0) {
        unlock_scheduler();
        return -KERR_ETIMEDOUT;
    }
    if (current_task->in_wait_queue || current_task->waiting_queue) {
        unlock_scheduler();
        return -KERR_EAGAIN;
    }
    current_task->waiting_queue = (wait_queue_t *)(uintptr_t)key;
    current_task->in_wait_queue = TASK_WAIT_QUEUE_FUTEX;
    current_task->futex_bitset = bitset;
    current_task->state = TASK_BLOCKED;
    if (timeout_ticks != UINT64_MAX) {
        wake_deadline_ns = sleepq_deadline_after_ticks(timeout_ticks);
        current_task->wake_deadline_ns = wake_deadline_ns;
        sleepq_insert(current_task);
    }
    unlock_scheduler();
    schedule();

    lock_scheduler();
    timed_out = current_task->in_wait_queue == TASK_WAIT_QUEUE_FUTEX;
    task_remove_wait_locked(current_task);
    current_task->futex_bitset = 0;
    sleepq_remove(current_task);
    unlock_scheduler();
    if (task_has_pending_signals()) return -KERR_EINTR;
    if (timed_out) return -KERR_ETIMEDOUT;
    return 0;
}

int task_futex_wake(uint64_t key, int count) {
    return task_futex_wake_bitset(key, count, 0xFFFFFFFFU);
}

int task_futex_wake_bitset(uint64_t key, int count, uint32_t bitset) {
    task_t *task;
    int woken;

    if (key == 0) return -KERR_EFAULT;
    if (bitset == 0) return -KERR_EINVAL;
    if (count < 0) return -KERR_EINVAL;
    if (count == 0) return 0;
    woken = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task && task_ptr_valid(task) && woken < count) {
        if (task->in_wait_queue == TASK_WAIT_QUEUE_FUTEX &&
            (uint64_t)(uintptr_t)task->waiting_queue == key &&
            (task->futex_bitset & bitset) != 0) {
            task_remove_wait_locked(task);
            sleepq_remove(task);
            task_mark_ready_locked(task);
            woken++;
        }
        task = task->all_next;
    }
    unlock_scheduler();
    return woken;
}

int task_futex_requeue(uint64_t old_key, uint64_t new_key, int wake_count,
                       int requeue_count) {
    task_t *task;
    int woken;
    int requeued;

    if (!old_key || !new_key) return -KERR_EFAULT;
    if (wake_count < 0 || requeue_count < 0) return -KERR_EINVAL;
    woken = 0;
    requeued = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task && task_ptr_valid(task)) {
        if (task->in_wait_queue == TASK_WAIT_QUEUE_FUTEX &&
            (uint64_t)(uintptr_t)task->waiting_queue == old_key) {
            if (woken < wake_count) {
                task_remove_wait_locked(task);
                sleepq_remove(task);
                task_mark_ready_locked(task);
                woken++;
            } else if (requeued < requeue_count) {
                task->waiting_queue = (wait_queue_t *)(uintptr_t)new_key;
                requeued++;
            }
        }
        task = task->all_next;
    }
    unlock_scheduler();
    return woken + requeued;
}

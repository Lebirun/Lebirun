#include "task_priv.h"

#define FUTEX_KEY_PRIVATE (1ULL << 63)

uint64_t task_futex_key(task_t *task, const int *uaddr, int private_futex) {
    uint64_t address;
    uint64_t pml4;
    uint64_t flags;
    uint64_t physical;
    int value;

    address = (uint64_t)(uintptr_t)uaddr;
    if (!task || address < 0x1000 || address >= 0x0000800000000000ULL ||
        (address & (sizeof(int) - 1))) return 0;
    if (private_futex) return address | FUTEX_KEY_PRIVATE;
    pml4 = task->pml4_phys ? task->pml4_phys : task->cr3;
    flags = vmm_get_flags_in_pml4(pml4, address & ~(PAGE_SIZE - 1));
    if (!(flags & VMM_PTE_PRESENT) && task == current_task &&
        copy_from_user(&value, uaddr, sizeof(value)) == 0)
        flags = vmm_get_flags_in_pml4(pml4, address & ~(PAGE_SIZE - 1));
    if (!(flags & (VMM_PTE_SHARED | VMM_PTE_NOFREE)))
        return address | FUTEX_KEY_PRIVATE;
    physical = vmm_get_phys_in_pml4(pml4, address & ~(PAGE_SIZE - 1));
    return physical ? physical + (address & (PAGE_SIZE - 1)) : 0;
}

static task_t *futex_buckets[16];
static task_t *futex_tails[16];

static int futex_bucket(uint64_t key) {
    key ^= key >> 29;
    key *= 0x9E3779B97F4A7C15ULL;
    return key >> 60;
}

static void futex_append_locked(task_t *task, uint64_t key) {
    int b;

    b = futex_bucket(key);
    task->wait_next = NULL;
    task->waiting_queue = (wait_queue_t *)(uintptr_t)key;
    task->in_wait_queue = TASK_WAIT_QUEUE_FUTEX;
    if (futex_tails[b]) futex_tails[b]->wait_next = task;
    else futex_buckets[b] = task;
    futex_tails[b] = task;
}

static void futex_detach_locked(int b, task_t **link, task_t *prev) {
    task_t *task;

    task = *link;
    *link = task->wait_next;
    if (futex_tails[b] == task) futex_tails[b] = prev;
    task->wait_next = NULL;
    task->in_wait_queue = 0;
    task->waiting_queue = NULL;
}

void futex_unlink_locked(task_t *task) {
    task_t **link;
    task_t *prev;
    int b;

    b = futex_bucket((uint64_t)(uintptr_t)task->waiting_queue);
    link = &futex_buckets[b];
    prev = NULL;
    while (*link && *link != task) {
        prev = *link;
        link = &prev->wait_next;
    }
    if (*link) futex_detach_locked(b, link, prev);
    task->wait_next = NULL;
    task->in_wait_queue = 0;
    task->waiting_queue = NULL;
}

static int futex_key_match(task_t *task, uint64_t key, task_vm_t *vm) {
    return (uint64_t)(uintptr_t)task->waiting_queue == key &&
           (!(key & FUTEX_KEY_PRIVATE) || task->vm == vm);
}

static void futex_wake_locked(int b, task_t **link, task_t *prev) {
    task_t *task;

    task = *link;
    futex_detach_locked(b, link, prev);
    task->futex_bitset = 0;
    sleepq_remove(task);
    task_mark_ready_locked(task);
}

static int futex_read_locked(const int *uaddr, int *value) {
    for (;;) {
        if (copy_from_user(value, uaddr, sizeof(*value)) < 0)
            return -KERR_EFAULT;
        lock_scheduler();
        if (vmm_copy_user_page(current_task->pml4_phys,
                               (uint64_t)(uintptr_t)uaddr, value,
                               sizeof(*value), 0) == 0) return 0;
        unlock_scheduler();
    }
}

int task_futex_wait(uint64_t key, const int *uaddr, int expected,
                    uint64_t timeout_ticks, uint32_t bitset) {
    int value;
    int timed_out;
    int woken;
    uint64_t wake_deadline_ns;

    if (!current_task || !uaddr || key == 0) return -KERR_EFAULT;
    if (bitset == 0) return -KERR_EINVAL;
    if (futex_read_locked(uaddr, &value) < 0) return -KERR_EFAULT;
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
    futex_append_locked(current_task, key);
    current_task->futex_bitset = bitset;
    current_task->state = TASK_BLOCKED;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (task_has_pending_signals()) {
        task_remove_wait_locked(current_task);
        current_task->futex_bitset = 0;
        current_task->state = TASK_RUNNING;
        unlock_scheduler();
        return -KERR_EINTR;
    }
    if (timeout_ticks != UINT64_MAX) {
        wake_deadline_ns = sleepq_deadline_after_ticks(timeout_ticks);
        current_task->wake_deadline_ns = wake_deadline_ns;
        sleepq_insert(current_task);
    }
    unlock_scheduler();
    schedule();

    lock_scheduler();
    timed_out = current_task->in_wait_queue == TASK_WAIT_QUEUE_FUTEX;
    woken = !timed_out && current_task->futex_bitset == 0;
    task_remove_wait_locked(current_task);
    current_task->futex_bitset = 0;
    sleepq_remove(current_task);
    unlock_scheduler();
    if (woken) return 0;
    if (task_has_pending_signals()) return -KERR_EINTR;
    if (timed_out) return -KERR_ETIMEDOUT;
    return 0;
}

static int futex_wake(task_vm_t *vm, uint64_t key, int count,
                      uint32_t bitset) {
    task_t **link;
    task_t *task;
    task_t *prev;
    int woken;
    int b;

    if (key == 0) return -KERR_EFAULT;
    if (bitset == 0) return -KERR_EINVAL;
    if (count < 0) return -KERR_EINVAL;
    if (count == 0) return 0;
    woken = 0;
    prev = NULL;
    lock_scheduler();
    b = futex_bucket(key);
    link = &futex_buckets[b];
    while ((task = *link) && woken < count) {
        if (futex_key_match(task, key, vm) &&
            (task->futex_bitset & bitset) != 0) {
            futex_wake_locked(b, link, prev);
            woken++;
        } else {
            prev = task;
            link = &task->wait_next;
        }
    }
    unlock_scheduler();
    return woken;
}

int task_futex_wake(uint64_t key, int count) {
    return task_futex_wake_bitset(key, count, 0xFFFFFFFFU);
}

int task_futex_wake_bitset(uint64_t key, int count, uint32_t bitset) {
    if (!current_task) return -KERR_EFAULT;
    return futex_wake(current_task->vm, key, count, bitset);
}

int task_futex_wake_in(task_t *task, const int *uaddr, int count) {
    if (!task) return -KERR_EFAULT;
    return futex_wake(task->vm, task_futex_key(task, uaddr, 0), count,
                      0xFFFFFFFFU);
}

int task_futex_requeue(uint64_t old_key, uint64_t new_key, int wake_count,
                       int requeue_count, const int *cmp_uaddr, int expected) {
    task_t **link;
    task_t *task;
    task_t *prev;
    task_t *moved;
    task_t **moved_tail;
    task_vm_t *vm;
    int value;
    int woken;
    int requeued;
    int b;

    if (!old_key || !new_key || !current_task) return -KERR_EFAULT;
    if (wake_count < 0 || requeue_count < 0) return -KERR_EINVAL;
    vm = current_task->vm;
    woken = 0;
    requeued = 0;
    moved = NULL;
    moved_tail = &moved;
    if (cmp_uaddr) {
        if (futex_read_locked(cmp_uaddr, &value) < 0) return -KERR_EFAULT;
        if (value != expected) {
            unlock_scheduler();
            return -KERR_EAGAIN;
        }
    } else {
        lock_scheduler();
    }
    b = futex_bucket(old_key);
    link = &futex_buckets[b];
    prev = NULL;
    while ((task = *link) && (woken < wake_count || requeued < requeue_count)) {
        if (!futex_key_match(task, old_key, vm)) {
            prev = task;
            link = &task->wait_next;
        } else if (woken < wake_count) {
            futex_wake_locked(b, link, prev);
            woken++;
        } else {
            futex_detach_locked(b, link, prev);
            *moved_tail = task;
            moved_tail = &task->wait_next;
            requeued++;
        }
    }
    while ((task = moved)) {
        moved = task->wait_next;
        futex_append_locked(task, new_key);
    }
    unlock_scheduler();
    return woken + requeued;
}

#include "task_priv.h"

static task_t* sleep_queue_head = NULL;
static uint64_t sleep_queue_deadline_ns = UINT64_MAX;
static void waitq_remove_locked(wait_queue_t *q, task_t *t);

void sleepq_remove(task_t* t) {
    task_t **ptr;
    task_t *cur;

    if (!t) return;
    ptr = &sleep_queue_head;
    while (*ptr) {
        cur = *ptr;
        if (!task_ptr_valid(cur)) {
            *ptr = NULL;
            __atomic_store_n(&sleep_queue_deadline_ns,
                sleep_queue_head ? sleep_queue_head->wake_deadline_ns : UINT64_MAX,
                __ATOMIC_RELEASE);
            return;
        }
        if (cur == t) {
            *ptr = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleep_queue = 0;
            __atomic_store_n(&sleep_queue_deadline_ns,
                sleep_queue_head ? sleep_queue_head->wake_deadline_ns : UINT64_MAX,
                __ATOMIC_RELEASE);
            return;
        }
        ptr = &cur->sleep_next;
    }
}

uint64_t sleepq_deadline_after_ticks(uint64_t ticks) {
    uint64_t now;
    uint64_t frequency;
    uint64_t seconds;
    uint64_t duration;
    uint64_t fraction;

    now = timekeeping_monotonic_ns();
    frequency = pit_get_frequency();
    seconds = ticks / frequency;
    if (seconds > (UINT64_MAX - now) / 1000000000ULL) return UINT64_MAX;
    duration = seconds * 1000000000ULL;
    fraction = ((ticks % frequency) * 1000000000ULL + frequency - 1) / frequency;
    if (fraction > UINT64_MAX - now - duration) return UINT64_MAX;
    return now + duration + fraction;
}

void sleepq_insert(task_t *task) {
    task_t **link;
    task_t *current;

    if (!task) return;
    link = &sleep_queue_head;
    while (*link) {
        current = *link;
        if (!task_ptr_valid(current)) {
            *link = NULL;
            break;
        }
        if (current->wake_deadline_ns > task->wake_deadline_ns) break;
        link = &current->sleep_next;
    }
    task->sleep_next = *link;
    task->in_sleep_queue = 1;
    *link = task;
    __atomic_store_n(&sleep_queue_deadline_ns,
                     sleep_queue_head->wake_deadline_ns, __ATOMIC_RELEASE);
    if (sleep_queue_head == task) smp_tick_deadline(task->wake_deadline_ns);
}

uint64_t task_next_wake_ns(void) {
    return __atomic_load_n(&sleep_queue_deadline_ns, __ATOMIC_ACQUIRE);
}

typedef struct descriptor_wait_filter {
    task_t *task;
    const uint64_t *keys;
    int count;
    struct descriptor_wait_filter *next;
} descriptor_wait_filter_t;

static descriptor_wait_filter_t *descriptor_wait_filters;

static void descriptor_filter_remove_locked(task_t *task) {
    descriptor_wait_filter_t **link;

    link = &descriptor_wait_filters;
    while (*link) {
        if ((*link)->task == task) {
            kfree((void *)(*link)->keys);
            *link = (*link)->next;
            return;
        }
        link = &(*link)->next;
    }
}

void task_remove_wait_locked(task_t *task) {
    if (!task) return;
    descriptor_filter_remove_locked(task);
    if (task->in_wait_queue == TASK_WAIT_QUEUE_NORMAL &&
        task->waiting_queue) {
        waitq_remove_locked(task->waiting_queue, task);
    } else if (task->in_wait_queue == TASK_WAIT_QUEUE_FUTEX) {
        task->in_wait_queue = 0;
        task->waiting_queue = NULL;
        task->wait_next = NULL;
    }
}

void waitq_init(wait_queue_t* q) {
    if (!q) return;
    q->head = NULL;
}

void waitq_add(wait_queue_t* q, task_t* t) {
    if (!q || !t) return;
    lock_scheduler();
    if (!t->in_wait_queue && !t->waiting_queue && t->state != TASK_DEAD) {
        t->wait_next = q->head;
        q->head = t;
        t->in_wait_queue = TASK_WAIT_QUEUE_NORMAL;
        t->waiting_queue = q;
        t->state = TASK_BLOCKED;
    }
    unlock_scheduler();
}

static task_t* waitq_pop_locked(wait_queue_t* q) {
    task_t *t;

    if (!q || !q->head) return NULL;
    t = q->head;
    while (t) {
        if (!task_ptr_valid(t)) {
            q->head = NULL;
            return NULL;
        }
        if (t->state != TASK_DEAD) break;
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        t = q->head;
    }
    if (!t || !task_ptr_valid(t)) return NULL;
    q->head = t->wait_next;
    t->wait_next = NULL;
    t->in_wait_queue = 0;
    t->waiting_queue = NULL;
    return t;
}

task_t* waitq_pop(wait_queue_t* q) {
    task_t *t;

    lock_scheduler();
    t = waitq_pop_locked(q);
    unlock_scheduler();
    return t;
}

static void waitq_wake_all_locked(wait_queue_t *q) {
    task_t *t;

    while ((t = waitq_pop_locked(q))) {
        task_mark_ready_locked(t);
    }
}

void waitq_wake_all(wait_queue_t* q) {
    if (!q) return;
    lock_scheduler();
    waitq_wake_all_locked(q);
    unlock_scheduler();
}

void waitq_wake_one(wait_queue_t *q) {
    task_t *t;

    if (!q) return;
    lock_scheduler();
    t = waitq_pop_locked(q);
    task_mark_ready_locked(t);
    unlock_scheduler();
}

void waitq_wait(wait_queue_t *q) {
    if (!q || !current_task) return;
    waitq_add(q, current_task);
    yield();
}

static void waitq_remove_locked(wait_queue_t* q, task_t* t) {
    task_t *prev;
    task_t *cur;

    if (!q || !t || !q->head) return;
    if (!task_ptr_valid(q->head)) {
        q->head = NULL;
        return;
    }
    if (q->head == t) {
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        return;
    }
    prev = q->head;
    cur = prev->wait_next;
    while (cur) {
        if (!task_ptr_valid(cur)) {
            prev->wait_next = NULL;
            return;
        }
        if (cur == t) {
            prev->wait_next = cur->wait_next;
            cur->wait_next = NULL;
            cur->in_wait_queue = 0;
            cur->waiting_queue = NULL;
            return;
        }
        prev = cur;
        cur = cur->wait_next;
    }
}

void waitq_remove(wait_queue_t* q, task_t* t) {
    if (!q || !t) return;
    lock_scheduler();
    waitq_remove_locked(q, t);
    unlock_scheduler();
}

static wait_queue_t descriptor_ready_waitq;
static volatile uint64_t descriptor_ready_sequence = 1;
static volatile int descriptor_ready_irq_pending;

uint64_t descriptor_ready_generation(void) {
    return __atomic_load_n(&descriptor_ready_sequence, __ATOMIC_ACQUIRE);
}

void descriptor_ready_notify(void) {
    lock_scheduler();
    __atomic_store_n(&descriptor_ready_irq_pending, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&descriptor_ready_sequence, 1, __ATOMIC_RELEASE);
    waitq_wake_all(&descriptor_ready_waitq);
    unlock_scheduler();
}

void descriptor_ready_notify_key(uint64_t key) {
    descriptor_wait_filter_t *filter;
    descriptor_wait_filter_t *next;
    task_t *task;
    int i;
    int matches;

    if (!key) {
        descriptor_ready_notify();
        return;
    }
    lock_scheduler();
    __atomic_add_fetch(&descriptor_ready_sequence, 1, __ATOMIC_RELEASE);
    filter = descriptor_wait_filters;
    while (filter) {
        next = filter->next;
        task = filter->task;
        if (task->waiting_queue == &descriptor_ready_waitq) {
            matches = filter->count == 0;
            for (i = 0; i < filter->count && !matches; i++)
                matches = !filter->keys[i] || filter->keys[i] == key;
            if (matches) {
                task_remove_wait_locked(task);
                sleepq_remove(task);
                task_mark_ready_locked(task);
            }
        }
        filter = next;
    }
    unlock_scheduler();
}

void descriptor_ready_notify_irq(void) {
    __atomic_add_fetch(&descriptor_ready_sequence, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&descriptor_ready_irq_pending, 1, __ATOMIC_RELEASE);
}

void descriptor_ready_drain_irq_locked(void) {
    if (!__atomic_exchange_n(&descriptor_ready_irq_pending, 0,
                             __ATOMIC_ACQ_REL)) return;
    waitq_wake_all_locked(&descriptor_ready_waitq);
}

void descriptor_ready_try_drain_irq(void) {
    if (!__atomic_load_n(&descriptor_ready_irq_pending, __ATOMIC_ACQUIRE))
        return;
    if (!spin_trylock(&sched_lock)) return;
    descriptor_ready_drain_irq_locked();
    spin_unlock(&sched_lock);
    task_kick_idle_cpus();
}

int descriptor_ready_wait_owned_keys(uint64_t generation, uint64_t timeout_ticks,
                                const uint64_t *keys, int count) {
    uint64_t wake_deadline_ns;
    int changed;
    descriptor_wait_filter_t filter;

    if (!current_task) {
        kfree((void *)keys);
        return 0;
    }
    lock_scheduler();
    if (descriptor_ready_generation() != generation) {
        unlock_scheduler();
        kfree((void *)keys);
        return 1;
    }
    if (timeout_ticks == 0 || current_task->in_wait_queue ||
        current_task->waiting_queue) {
        unlock_scheduler();
        kfree((void *)keys);
        return 0;
    }
    filter.task = current_task;
    filter.keys = keys;
    filter.count = count;
    filter.next = descriptor_wait_filters;
    descriptor_wait_filters = &filter;
    current_task->wait_next = descriptor_ready_waitq.head;
    descriptor_ready_waitq.head = current_task;
    current_task->in_wait_queue = TASK_WAIT_QUEUE_NORMAL;
    current_task->waiting_queue = &descriptor_ready_waitq;
    current_task->state = TASK_BLOCKED;
    if (timeout_ticks != UINT64_MAX) {
        wake_deadline_ns = sleepq_deadline_after_ticks(timeout_ticks);
        current_task->wake_deadline_ns = wake_deadline_ns;
        sleepq_insert(current_task);
    }
    if (descriptor_ready_generation() != generation) {
        task_remove_wait_locked(current_task);
        sleepq_remove(current_task);
        current_task->state = TASK_RUNNING;
        unlock_scheduler();
        return 1;
    }
    unlock_scheduler();
    schedule();
    lock_scheduler();
    changed = descriptor_ready_generation() != generation;
    task_remove_wait_locked(current_task);
    sleepq_remove(current_task);
    unlock_scheduler();
    return changed;
}

int waitq_wait_cond(wait_queue_t *q, int (*cond)(void *), void *arg,
                    uint64_t deadline_ns) {
    uint64_t flags;
    task_t *task;
    int done;

    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    task = current_task;
    if (!scheduler_initialized || !smp_scheduling_enabled() || !task ||
        (task->id == 0 && !task->is_user) || !(flags & (1ULL << 9)) ||
        smp_this_cpu()->scheduler_lock_depth)
        return -1;
    for (;;) {
        lock_scheduler();
        done = cond(arg);
        if (done || timekeeping_monotonic_ns() >= deadline_ns ||
            task->in_wait_queue || task->waiting_queue) {
            unlock_scheduler();
            return done;
        }
        task->wait_next = q->head;
        q->head = task;
        task->in_wait_queue = TASK_WAIT_QUEUE_NORMAL;
        task->waiting_queue = q;
        task->state = TASK_BLOCKED;
        task->wake_deadline_ns = deadline_ns;
        sleepq_insert(task);
        unlock_scheduler();
        schedule();
        lock_scheduler();
        task_remove_wait_locked(task);
        sleepq_remove(task);
        unlock_scheduler();
    }
}

int descriptor_ready_wait(uint64_t generation, uint64_t timeout_ticks) {
    return descriptor_ready_wait_owned_keys(generation, timeout_ticks, NULL, 0);
}

void sleep_ticks(uint64_t ticks) {
    uint64_t new_wake;

    if (!current_task || ticks == 0) return;
    lock_scheduler();
    new_wake = sleepq_deadline_after_ticks(ticks);
    sleepq_remove(current_task);
    current_task->wake_deadline_ns = new_wake;
    current_task->state = TASK_BLOCKED;
    sleepq_insert(current_task);
    unlock_scheduler();
    schedule();
}

int task_sleep_until(uint64_t deadline_ns) {
    if (!current_task) return -KERR_EINVAL;
    while (timekeeping_monotonic_ns() < deadline_ns) {
        lock_scheduler();
        if (task_has_pending_signals()) {
            unlock_scheduler();
            return -KERR_EINTR;
        }
        if (timekeeping_monotonic_ns() >= deadline_ns) {
            unlock_scheduler();
            continue;
        }
        sleepq_remove(current_task);
        current_task->wake_deadline_ns = deadline_ns;
        current_task->state = TASK_BLOCKED;
        sleepq_insert(current_task);
        unlock_scheduler();
        schedule();
        if (task_has_pending_signals()) return -KERR_EINTR;
    }
    return 0;
}

static void wake_sleeping_tasks_locked(void) {
    task_t *t;
    uint64_t now;

    now = timekeeping_monotonic_ns();
    while (sleep_queue_head) {
        t = sleep_queue_head;
        if (!task_ptr_valid(t)) {
            sleep_queue_head = NULL;
            break;
        }
        if (t->wake_deadline_ns > now) break;
        sleep_queue_head = t->sleep_next;
        t->sleep_next = NULL;
        t->in_sleep_queue = 0;
        task_mark_ready_locked(t);
    }
    __atomic_store_n(&sleep_queue_deadline_ns,
        sleep_queue_head ? sleep_queue_head->wake_deadline_ns : UINT64_MAX,
        __ATOMIC_RELEASE);
}

void wake_sleeping_tasks(void) {
    if (!sleep_queue_head) return;
    lock_scheduler();
    wake_sleeping_tasks_locked();
    unlock_scheduler();
}

int wake_sleeping_tasks_from_irq(void) {
    uint64_t deadline;

    deadline = __atomic_load_n(&sleep_queue_deadline_ns, __ATOMIC_ACQUIRE);
    if (deadline == UINT64_MAX || deadline > timekeeping_monotonic_ns()) return 1;
    if (!spin_trylock(&sched_lock)) return 0;
    wake_sleeping_tasks_locked();
    spin_unlock(&sched_lock);
    task_kick_idle_cpus();
    return 1;
}

void sleep_ms(uint64_t ms) {
    extern uint64_t pit_freq;
    uint64_t ticks;

    if (ms == 0) return;
    ticks = (ms * pit_freq + 999) / 1000; 
    if (ticks == 0) ticks = 1;
    sleep_ticks(ticks);
}

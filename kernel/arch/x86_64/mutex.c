#include <stddef.h>
#include <lebirun/mutex.h>
#include <lebirun/task.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->depth = 0;
    m->owner = 0;
    waitq_init(&m->waiters);
}

void mutex_lock(mutex_t* m) {
    extern int scheduler_initialized;
    cpu_info_t *cpu;
    task_t *task;
    task_t *holder;
    volatile int i;
    uint64_t owner;
    uint8_t stage;
    int can_block;
    int staged;
    int queued;

    if (!m) return;

    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : current_task;
    owner = (uint64_t)(uintptr_t)task;
    stage = task ? task->kernel_stage : 0;
    can_block = task && task->id != 0 && cpu &&
                cpu->scheduler_lock_depth == 0;
    staged = 0;
    if (!scheduler_initialized) {
        while (!__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            smp_tlb_flush_poll();
            __asm__ __volatile__ ("pause");
        }
        m->depth = 1;
        m->owner = 0;
        return;
    }

    while (1) {
        if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            m->owner = owner;
            m->depth = 1;
            if (staged) task->kernel_stage = stage;
            return;
        }

        if (task && m->owner == owner && owner != 0) {
            m->depth++;
            if (staged) task->kernel_stage = stage;
            return;
        }

        for (i = 0; i < 64 && m->locked; i++) {
            smp_tlb_flush_poll();
            __asm__ volatile ("pause" ::: "memory");
        }
        if (m->locked && can_block) {
            if (!staged && task->kernel_stage == TASK_KERNEL_STAGE_NONE) {
                task->kernel_stage = TASK_KERNEL_STAGE_MUTEX;
                staged = 1;
            }
            lock_scheduler();
            if (m->owner == owner && owner != 0) {
                m->depth++;
                unlock_scheduler();
                if (staged) task->kernel_stage = stage;
                return;
            }
            waitq_add(&m->waiters, task);
            queued = task->waiting_queue == &m->waiters;
            if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
                if (queued) {
                    waitq_remove(&m->waiters, task);
                    task->state = TASK_RUNNING;
                }
                m->owner = owner;
                m->depth = 1;
                unlock_scheduler();
                if (staged) task->kernel_stage = stage;
                return;
            }
            if (queued && m->owner) {
                holder = (task_t *)(uintptr_t)m->owner;
                if (holder && holder != task && holder->pi_boost < 40) holder->pi_boost = 40;
            }
            unlock_scheduler();
            if (queued) {
                schedule();
                lock_scheduler();
                cpu = smp_this_cpu();
                if (cpu && cpu->running_task == task &&
                    task->state == TASK_READY)
                    task->state = TASK_RUNNING;
                unlock_scheduler();
            }
        }
    }
}

int mutex_trylock(mutex_t* m) {
    cpu_info_t *cpu;
    task_t *task;
    uint64_t owner;

    if (!m) return 0;
    cpu = smp_this_cpu();
    task = cpu ? cpu->running_task : current_task;
    owner = (uint64_t)(uintptr_t)task;
    if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
        m->owner = owner;
        m->depth = 1;
        return 1;
    }
    if (owner && m->owner == owner) {
        m->depth++;
        return 1;
    }
    return 0;
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
            if (m->depth > 1) {
                m->depth--;
                return;
            }
            m->depth = 0;
            m->owner = 0;
            __sync_lock_release(&m->locked);
        }
        return;
    }

    if (!m->locked) {
        return;
    }
    if (m->owner != (uint64_t)(uintptr_t)task) {
        return;
    }
    if (m->depth > 1) {
        m->depth--;
        return;
    }

    m->depth = 0;
    m->owner = 0;
    if (task) task->pi_boost = 0;
    __atomic_exchange_n(&m->locked, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->waiters.head, __ATOMIC_ACQUIRE))
        waitq_wake_one(&m->waiters);
}

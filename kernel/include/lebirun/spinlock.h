#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdint.h>

typedef struct spinlock {
    volatile uint64_t locked;
} spinlock_t;

static inline void cpu_relax(void) {
    asm volatile ("pause" ::: "memory");
}

static inline void spinlock_init(spinlock_t* l) {
    if (!l) return;
    l->locked = 0;
}

static inline void spin_lock(spinlock_t* l) {
    if (!l) return;
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked) {
            cpu_relax();
        }
    }
}

static inline int spin_trylock(spinlock_t* l) {
    if (!l) return 0;
    if (__sync_lock_test_and_set(&l->locked, 1) == 0) {
        return 1;
    }
    return 0;
}

static inline void spin_unlock(spinlock_t* l) {
    if (!l) return;
    __sync_lock_release(&l->locked);
}

#endif

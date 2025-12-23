#ifndef _SCHED_H
#define _SCHED_H 1

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6

#define SCHED_RESET_ON_FORK 0x40000000

struct sched_param {
    int sched_priority;
};

#ifdef __USE_GNU

typedef struct {
    unsigned long __bits[128 / sizeof(unsigned long)];
} cpu_set_t;

#define CPU_SETSIZE 1024

#define __CPUELT(cpu)  ((cpu) / (8 * sizeof(unsigned long)))
#define __CPUMASK(cpu) ((unsigned long)1 << ((cpu) % (8 * sizeof(unsigned long))))

#define CPU_SET(cpu, cpusetp)   ((cpusetp)->__bits[__CPUELT(cpu)] |= __CPUMASK(cpu))
#define CPU_CLR(cpu, cpusetp)   ((cpusetp)->__bits[__CPUELT(cpu)] &= ~__CPUMASK(cpu))
#define CPU_ISSET(cpu, cpusetp) (((cpusetp)->__bits[__CPUELT(cpu)] & __CPUMASK(cpu)) != 0)
#define CPU_ZERO(cpusetp)       do { \
    unsigned int __i; \
    cpu_set_t *__arr = (cpusetp); \
    for (__i = 0; __i < sizeof(cpu_set_t) / sizeof(unsigned long); __i++) \
        __arr->__bits[__i] = 0; \
} while (0)

#define CPU_COUNT(cpusetp) __sched_cpucount(sizeof(cpu_set_t), cpusetp)

#define CPU_AND(destset, srcset1, srcset2) \
    __CPU_OP(destset, srcset1, srcset2, &)
#define CPU_OR(destset, srcset1, srcset2) \
    __CPU_OP(destset, srcset1, srcset2, |)
#define CPU_XOR(destset, srcset1, srcset2) \
    __CPU_OP(destset, srcset1, srcset2, ^)

#define CPU_EQUAL(cpusetp1, cpusetp2) \
    __builtin_memcmp(cpusetp1, cpusetp2, sizeof(cpu_set_t)) == 0

#define __CPU_OP(destset, srcset1, srcset2, op) \
    do { \
        cpu_set_t *__dest = (destset); \
        const cpu_set_t *__src1 = (srcset1); \
        const cpu_set_t *__src2 = (srcset2); \
        unsigned int __i; \
        for (__i = 0; __i < sizeof(cpu_set_t) / sizeof(unsigned long); __i++) \
            __dest->__bits[__i] = __src1->__bits[__i] op __src2->__bits[__i]; \
    } while (0)

int __sched_cpucount(size_t setsize, const cpu_set_t *setp);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);

#endif

int sched_yield(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_rr_get_interval(pid_t pid, struct timespec *tp);

#ifdef __cplusplus
}
#endif

#endif

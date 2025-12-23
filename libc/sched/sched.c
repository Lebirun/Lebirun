#include <sched.h>
#include <errno.h>

int sched_yield(void) {
    return 0;
}

int sched_get_priority_max(int policy) {
    (void)policy;
    return 99;
}

int sched_get_priority_min(int policy) {
    (void)policy;
    return 0;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    (void)pid;
    (void)policy;
    (void)param;
    return 0;
}

int sched_getscheduler(pid_t pid) {
    (void)pid;
    return SCHED_OTHER;
}

int sched_setparam(pid_t pid, const struct sched_param *param) {
    (void)pid;
    (void)param;
    return 0;
}

int sched_getparam(pid_t pid, struct sched_param *param) {
    (void)pid;
    if (param) {
        param->sched_priority = 0;
    }
    return 0;
}

int sched_rr_get_interval(pid_t pid, struct timespec *tp) {
    (void)pid;
    if (tp) {
        tp->tv_sec = 0;
        tp->tv_nsec = 100000000;
    }
    return 0;
}

#ifdef __USE_GNU
int __sched_cpucount(size_t setsize, const cpu_set_t *setp) {
    int count = 0;
    const unsigned long *p = setp->__bits;
    const unsigned long *end = p + setsize / sizeof(unsigned long);
    while (p < end) {
        unsigned long val = *p++;
        while (val) {
            count++;
            val &= val - 1;
        }
    }
    return count;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    (void)pid;
    (void)cpusetsize;
    (void)mask;
    return 0;
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
    (void)pid;
    if (mask && cpusetsize >= sizeof(cpu_set_t)) {
        CPU_ZERO(mask);
        CPU_SET(0, mask);
    }
    return 0;
}
#endif

#include "syscall_defs.h"
#include <lebirun/lke.h>
#include <lebirun/about.h>
#include <lebirun/rng.h>
#include <lebirun/pty.h>

extern task_t *current_task;
extern void **syscall_table;

struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[20 - 2 * sizeof(unsigned long) - sizeof(unsigned int)];
};

struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

struct rusage {
    struct kernel_timeval ru_utime;
    struct kernel_timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15
#define RLIM_NLIMITS      16

#define RLIM_INFINITY     (~0UL)

#define RUSAGE_SELF       0
#define RUSAGE_CHILDREN   (-1)
#define RUSAGE_THREAD     1

static struct rlimit default_rlimits[RLIM_NLIMITS] = {
    [RLIMIT_CPU]        = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_FSIZE]      = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_DATA]       = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_STACK]      = { 8 * 1024 * 1024, RLIM_INFINITY },
    [RLIMIT_CORE]       = { 0, RLIM_INFINITY },
    [RLIMIT_RSS]        = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_NPROC]      = { 1024, 1024 },
    [RLIMIT_NOFILE]     = { 1024, 4096 },
    [RLIMIT_MEMLOCK]    = { 65536, 65536 },
    [RLIMIT_AS]         = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_LOCKS]      = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_SIGPENDING] = { 1024, 1024 },
    [RLIMIT_MSGQUEUE]   = { 819200, 819200 },
    [RLIMIT_NICE]       = { 0, 0 },
    [RLIMIT_RTPRIO]     = { 0, 0 },
    [RLIMIT_RTTIME]     = { RLIM_INFINITY, RLIM_INFINITY },
};

typedef struct {
    int resource;
    struct rlimit value;
} task_rlimit_entry_t;

typedef struct {
    task_rlimit_entry_t *entries;
    int count;
    int capacity;
} task_rlimit_data_t;

static task_rlimit_entry_t *task_rlimit_find(task_t *task, int resource) {
    task_rlimit_data_t *data;
    int i;

    if (!task || !task->limits_data) return NULL;
    data = (task_rlimit_data_t *)task->limits_data;
    for (i = 0; i < data->count; i++) {
        if (data->entries[i].resource == resource) return &data->entries[i];
    }
    return NULL;
}

unsigned long task_rlimit_get(task_t *task, int resource, int maximum) {
    task_rlimit_entry_t *entry;

    if (resource < 0 || resource >= RLIM_NLIMITS) return RLIM_INFINITY;
    entry = task_rlimit_find(task, resource);
    if (!entry) {
        return maximum ? default_rlimits[resource].rlim_max :
                         default_rlimits[resource].rlim_cur;
    }
    return maximum ? entry->value.rlim_max : entry->value.rlim_cur;
}

int task_rlimit_set(task_t *task, int resource, unsigned long current,
                    unsigned long maximum) {
    task_rlimit_data_t *data;
    task_rlimit_entry_t *entry;
    task_rlimit_entry_t *new_entries;
    int new_capacity;

    if (!task || resource < 0 || resource >= RLIM_NLIMITS) return -1;
    if (current > maximum) return -1;
    entry = task_rlimit_find(task, resource);
    if (entry) {
        entry->value.rlim_cur = current;
        entry->value.rlim_max = maximum;
        return 0;
    }
    data = (task_rlimit_data_t *)task->limits_data;
    if (!data) {
        data = (task_rlimit_data_t *)kmalloc(sizeof(task_rlimit_data_t));
        if (!data) return -1;
        memset(data, 0, sizeof(task_rlimit_data_t));
        task->limits_data = data;
    }
    if (data->count == data->capacity) {
        new_capacity = data->capacity ? data->capacity * 2 : 1;
        new_entries = (task_rlimit_entry_t *)krealloc(
            data->entries, new_capacity * sizeof(task_rlimit_entry_t));
        if (!new_entries) return -1;
        data->entries = new_entries;
        data->capacity = new_capacity;
    }
    entry = &data->entries[data->count++];
    entry->resource = resource;
    entry->value.rlim_cur = current;
    entry->value.rlim_max = maximum;
    return 0;
}

int task_rlimit_copy(task_t *dest, task_t *src) {
    task_rlimit_data_t *src_data;
    task_rlimit_data_t *dest_data;
    size_t size;

    if (!dest || !src || !src->limits_data) return 0;
    src_data = (task_rlimit_data_t *)src->limits_data;
    dest_data = (task_rlimit_data_t *)kmalloc(sizeof(task_rlimit_data_t));
    if (!dest_data) return -1;
    memset(dest_data, 0, sizeof(task_rlimit_data_t));
    if (src_data->count != 0) {
        size = (size_t)src_data->count * sizeof(task_rlimit_entry_t);
        dest_data->entries = (task_rlimit_entry_t *)kmalloc(size);
        if (!dest_data->entries) {
            kfree(dest_data);
            return -1;
        }
        memcpy(dest_data->entries, src_data->entries, size);
        dest_data->count = src_data->count;
        dest_data->capacity = src_data->count;
    }
    dest->limits_data = dest_data;
    return 0;
}

void task_rlimit_free(task_t *task) {
    task_rlimit_data_t *data;

    if (!task || !task->limits_data) return;
    data = (task_rlimit_data_t *)task->limits_data;
    kfree(data->entries);
    kfree(data);
    task->limits_data = NULL;
}

uint64_t task_user_memory_bytes(task_t *task) {
    if (!task) return 0;
    if (task->user_pages_count > UINT64_MAX / PAGE_SIZE) return UINT64_MAX;
    return task->user_pages_count * PAGE_SIZE;
}

int task_memory_allows(task_t *task, uint64_t additional_bytes) {
    uint64_t used;
    unsigned long limit;
    unsigned long resident_limit;

    if (!task) return 0;
    limit = task_rlimit_get(task, RLIMIT_AS, 0);
    resident_limit = task_rlimit_get(task, RLIMIT_RSS, 0);
    used = task_user_memory_bytes(task);
    if (additional_bytes > UINT64_MAX - used) return 0;
    if (limit != RLIM_INFINITY &&
        used + additional_bytes > (uint64_t)limit) return 0;
    if (resident_limit != RLIM_INFINITY &&
        used + additional_bytes > (uint64_t)resident_limit) return 0;
    return 1;
}

int task_memory_total_allows(task_t *task, uint64_t total_bytes) {
    unsigned long limit;
    unsigned long resident_limit;

    if (!task) return 0;
    limit = task_rlimit_get(task, RLIMIT_AS, 0);
    resident_limit = task_rlimit_get(task, RLIMIT_RSS, 0);
    if (limit != RLIM_INFINITY && total_bytes > (uint64_t)limit) return 0;
    if (resident_limit != RLIM_INFINITY &&
        total_bytes > (uint64_t)resident_limit) return 0;
    return 1;
}

int task_stack_allows(task_t *task, uint64_t additional_bytes) {
    unsigned long limit;

    if (!task) return 0;
    limit = task_rlimit_get(task, RLIMIT_STACK, 0);
    if (limit == RLIM_INFINITY) return 1;
    if (additional_bytes > UINT64_MAX - task->stack_size) return 0;
    return task->stack_size + additional_bytes <= (uint64_t)limit;
}

int task_data_allows(task_t *task, uint64_t new_brk) {
    uint64_t bytes;
    unsigned long limit;

    if (!task || new_brk < task->user_brk_start) return 0;
    limit = task_rlimit_get(task, RLIMIT_DATA, 0);
    if (limit == RLIM_INFINITY) return 1;
    bytes = new_brk - task->user_brk_start;
    return bytes <= (uint64_t)limit;
}

extern volatile uint64_t tick_count;
extern uint64_t pit_freq;
extern uint64_t pfa_count_free(void);

static void copy_string(char *dest, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static int sys_uname(struct utsname *buf) {
    struct utsname value;
    char version_str[65];
    int len;
    int i;
    
    if (!buf) return -EFAULT;
    memset(&value, 0, sizeof(value));
    copy_string(value.sysname, OS_NAME, 65);
    copy_string(value.nodename, NODENAME, 65);
    copy_string(value.release, OS_VERSION, 65);
    
    len = 0;
    for (i = 0; KERNEL_BUILD_DATE[i] && len < 50; i++) {
        version_str[len++] = KERNEL_BUILD_DATE[i];
    }
    if (len < 64) version_str[len++] = ' ';
    for (i = 0; KERNEL_BUILD_TIME[i] && len < 63; i++) {
        version_str[len++] = KERNEL_BUILD_TIME[i];
    }
    if (len < 64) version_str[len++] = ' ';
    for (i = 0; KERNEL_BUILD_TIMEZONE[i] && len < 63; i++) {
        version_str[len++] = KERNEL_BUILD_TIMEZONE[i];
    }
    version_str[len] = '\0';
    copy_string(value.version, version_str, 65);
    copy_string(value.machine, MACHINE, 65);
    if (copy_to_user(buf, &value, sizeof(value)) < 0) return -EFAULT;
    return 0;
}

static int sys_sysinfo(struct sysinfo *info) {
    struct sysinfo value;
    uint64_t total_kb;
    uint64_t free_pages;
    int proc_count;
    
    if (!info) return -EFAULT;
    memset(&value, 0, sizeof(value));
    
    total_kb = pfa_get_total_ram_kb();
    free_pages = pfa_count_free();

    {
        extern void task_get_cached_stats(int *, int *, int *, pid_t *);
        task_get_cached_stats(&proc_count, NULL, NULL, NULL);
    }
    if (proc_count < 1) proc_count = 1;
    
    value.uptime = tick_count / pit_freq;
    value.totalram = (unsigned long)total_kb * 1024;
    value.freeram = (unsigned long)(free_pages * 4) * 1024;
    value.procs = (unsigned short)proc_count;
    value.mem_unit = 1;
    if (copy_to_user(info, &value, sizeof(value)) < 0) return -EFAULT;
    return 0;
}

static int sys_getrlimit(int resource, struct rlimit *rlim) {
    struct rlimit value;

    if (!rlim) return -EFAULT;
    if (resource < 0 || resource >= RLIM_NLIMITS) return -EINVAL;
    value.rlim_cur = task_rlimit_get(current_task, resource, 0);
    value.rlim_max = task_rlimit_get(current_task, resource, 1);
    if (copy_to_user(rlim, &value, sizeof(value)) < 0) return -EFAULT;
    return 0;
}

static int sys_setrlimit(int resource, const struct rlimit *rlim) {
    struct rlimit value;
    unsigned long old_maximum;

    if (!rlim) return -EFAULT;
    if (resource < 0 || resource >= RLIM_NLIMITS) return -EINVAL;
    if (copy_from_user(&value, rlim, sizeof(value)) < 0) return -EFAULT;
    if (value.rlim_cur > value.rlim_max) return -EINVAL;
    old_maximum = task_rlimit_get(current_task, resource, 1);
    if (value.rlim_max > old_maximum && current_task->euid != 0) return -EPERM;
    if (task_rlimit_set(current_task, resource, value.rlim_cur,
                        value.rlim_max) < 0) return -ENOMEM;
    return 0;
}

static int sys_getrusage(int who, struct rusage *usage) {
    struct rusage value;

    if (!usage) return -EFAULT;
    if (who != RUSAGE_SELF && who != RUSAGE_THREAD &&
        who != RUSAGE_CHILDREN) {
        return -EINVAL;
    }
    memset(&value, 0, sizeof(value));
    if (copy_to_user(usage, &value, sizeof(value)) < 0) return -EFAULT;
    return 0;
}

static int sys_prlimit64(int pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit) {
    struct rlimit old_value;
    struct rlimit new_value;
    unsigned long old_maximum;

    if (resource < 0 || resource >= RLIM_NLIMITS) return -EINVAL;
    
    if (pid != 0 && (!current_task || (pid_t)pid != current_task->pid)) {
        return -ESRCH;
    }
    
    if (old_limit) {
        old_value.rlim_cur = task_rlimit_get(current_task, resource, 0);
        old_value.rlim_max = task_rlimit_get(current_task, resource, 1);
        if (copy_to_user(old_limit, &old_value, sizeof(old_value)) < 0)
            return -EFAULT;
    }
    if (new_limit) {
        if (copy_from_user(&new_value, new_limit, sizeof(new_value)) < 0)
            return -EFAULT;
        if (new_value.rlim_cur > new_value.rlim_max) return -EINVAL;
        old_maximum = task_rlimit_get(current_task, resource, 1);
        if (new_value.rlim_max > old_maximum && current_task->euid != 0)
            return -EPERM;
        if (task_rlimit_set(current_task, resource, new_value.rlim_cur,
                            new_value.rlim_max) < 0) return -ENOMEM;
    }
    return 0;
}

static int sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    uint8_t tmp[256];
    size_t done;
    size_t chunk;

    (void)flags;
    if (!buf) return -EFAULT;
    if (buflen == 0) return 0;
    done = 0;
    while (done < buflen) {
        chunk = buflen - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        rng_fill(tmp, chunk);
        if (copy_to_user((uint8_t *)buf + done, tmp, chunk) < 0) {
            memset(tmp, 0, sizeof(tmp));
            return -EFAULT;
        }
        done += chunk;
    }
    memset(tmp, 0, sizeof(tmp));
    return buflen;
}

#define PR_SET_NAME    15
#define PR_GET_NAME    16
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3
#define PR_SET_SECCOMP  22
#define PR_GET_SECCOMP  21

static int sys_prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5) {
    char name[16];
    size_t length;

    (void)arg3; (void)arg4; (void)arg5;
    switch (option) {
        case PR_SET_NAME:
            if (current_task && arg2) {
                if (strnlen_user((const char *)arg2, 16, &length) < 0)
                    return -EFAULT;
                if (copy_from_user(name, (const void *)arg2, length + 1) < 0)
                    return -EFAULT;
                memcpy(current_task->name, name, length + 1);
            }
            return 0;
            
        case PR_GET_NAME:
            if (current_task && arg2) {
                memset(name, 0, sizeof(name));
                length = strlen(current_task->name);
                if (length > 15) length = 15;
                memcpy(name, current_task->name, length);
                if (copy_to_user((void *)arg2, name, sizeof(name)) < 0)
                    return -EFAULT;
            }
            return 0;
            
        case PR_SET_DUMPABLE:
            return 0;
            
        case PR_GET_DUMPABLE:
            return 1;
            
        case PR_SET_SECCOMP:
        case PR_GET_SECCOMP:
            return -EINVAL;
            
        default:
            return -EINVAL;
    }
}

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

static int sys_arch_prctl(int code, unsigned long addr) {
    switch (code) {
        case ARCH_SET_FS:
            __asm__ volatile (
                "wrmsr"
                :
                : "c"(0xC0000100u),
                  "a"((uint32_t)(addr & 0xFFFFFFFF)),
                  "d"((uint32_t)(addr >> 32))
                : "memory"
            );
            if (current_task) {
                current_task->tls_base = addr;
            }
            return 0;
            
        case ARCH_GET_FS:
            if (addr && current_task) {
                if (copy_to_user((void *)addr, &current_task->tls_base,
                                 sizeof(current_task->tls_base)) < 0)
                    return -EFAULT;
            }
            return 0;
            
        case ARCH_SET_GS:
        case ARCH_GET_GS:
            return 0;
            
        default:
            return -EINVAL;
    }
}

static int sys_set_tid_address(int *tidptr) {
    if (current_task) {
        current_task->clear_child_tid = tidptr;
        return current_task->pid;
    }
    return 1;
}

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval_k {
    struct kernel_timeval it_interval;
    struct kernel_timeval it_value;
};

typedef struct {
    struct itimerval_k values[3];
} task_timer_data_t;

static task_timer_data_t *get_task_timer_data(int create) {
    task_timer_data_t *timers;

    if (!current_task) return NULL;
    timers = (task_timer_data_t *)current_task->timer_data;
    if (!timers && create) {
        timers = (task_timer_data_t *)kmalloc(sizeof(task_timer_data_t));
        if (!timers) return NULL;
        memset(timers, 0, sizeof(task_timer_data_t));
        current_task->timer_data = timers;
    }
    return timers;
}

static int timer_value_is_zero(const struct itimerval_k *value) {
    if (!value) return 1;
    return value->it_interval.tv_sec == 0 &&
           value->it_interval.tv_usec == 0 &&
           value->it_value.tv_sec == 0 &&
           value->it_value.tv_usec == 0;
}

static int timer_data_is_empty(const task_timer_data_t *timers) {
    int i;

    if (!timers) return 1;
    for (i = 0; i < 3; i++) {
        if (!timer_value_is_zero(&timers->values[i])) return 0;
    }
    return 1;
}

static int sys_setitimer(int which, const struct itimerval_k *new_value, struct itimerval_k *old_value) {
    task_timer_data_t *timers;
    struct itimerval_k value;

    if (which < 0 || which > 2) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (old_value &&
        ((uint64_t)old_value < 0x1000 || (uint64_t)old_value >= KERNEL_VMA))
        return -EFAULT;
    if (new_value &&
        ((uint64_t)new_value < 0x1000 || (uint64_t)new_value >= KERNEL_VMA))
        return -EFAULT;

    timers = get_task_timer_data(0);
    if (old_value) {
        if (timers) {
            memcpy(old_value, &timers->values[which], sizeof(struct itimerval_k));
        } else {
            memset(old_value, 0, sizeof(struct itimerval_k));
        }
    }

    if (new_value) {
        memcpy(&value, new_value, sizeof(struct itimerval_k));
        if (!timers && !timer_value_is_zero(&value)) {
            timers = get_task_timer_data(1);
            if (!timers) return -ENOMEM;
        }
        if (timers) {
            memcpy(&timers->values[which], &value, sizeof(struct itimerval_k));
            if (timer_data_is_empty(timers)) {
                kfree(timers);
                current_task->timer_data = NULL;
            }
        }
    }

    return 0;
}

static int sys_getitimer(int which, struct itimerval_k *curr_value) {
    task_timer_data_t *timers;

    if (which < 0 || which > 2) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (!curr_value) return -EFAULT;
    if ((uint64_t)curr_value < 0x1000 || (uint64_t)curr_value >= KERNEL_VMA) return -EFAULT;

    timers = get_task_timer_data(0);
    if (timers) {
        memcpy(curr_value, &timers->values[which], sizeof(struct itimerval_k));
    } else {
        memset(curr_value, 0, sizeof(struct itimerval_k));
    }

    return 0;
}

static int sys_alarm(int seconds, const char *unused1, int unused2) {
    uint64_t old_alarm;
    
    (void)unused1; (void)unused2;
    if (!current_task) return 0;
    
    old_alarm = current_task->alarm_tick;
    
    if (seconds > 0) {
        current_task->alarm_tick = tick_count + (seconds * pit_freq);
    } else {
        current_task->alarm_tick = 0;
    }
    
    if (old_alarm > tick_count) {
        return (old_alarm - tick_count) / pit_freq;
    }
    return 0;
}

static int sys_nanosleep(int arg0, int arg1, int arg2, int arg3) {
    const struct kernel_timespec *req;
    struct kernel_timespec *rem;
    struct kernel_timespec ts64;
    long long *ts_ptr;
    uint64_t ms;
    uint64_t a0;

    a0 = (uint64_t)arg0;

    if (a0 < 32) {
        ts_ptr = (long long *)(uint64_t)arg2;
        rem = (struct kernel_timespec *)(uint64_t)arg3;
        if (!ts_ptr) return -EFAULT;
        if ((uint64_t)ts_ptr < 0x1000 || (uint64_t)ts_ptr >= KERNEL_VMA)
            return -EFAULT;
        ts64.tv_sec = (long)ts_ptr[0];
        ts64.tv_nsec = (long)ts_ptr[1];
        req = &ts64;
    } else {
        req = (const struct kernel_timespec *)a0;
        rem = (struct kernel_timespec *)(uint64_t)arg1;
        if (!req) return -EFAULT;
        if ((uint64_t)req < 0x1000 || (uint64_t)req >= KERNEL_VMA)
            return -EFAULT;
    }

    ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    if (ms > 0) {
        sleep_ms(ms);
    } else {
        schedule();
    }

    {
        extern int task_has_pending_signals(void);
        if (task_has_pending_signals()) {
            if (rem) {
                if ((uint64_t)rem >= 0x1000 && (uint64_t)rem < KERNEL_VMA) {
                    rem->tv_sec = 0;
                    rem->tv_nsec = 0;
                }
            }
            return -EINTR;
        }
    }

    if (rem) {
        if ((uint64_t)rem >= 0x1000 && (uint64_t)rem < KERNEL_VMA) {
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
    }

    return 0;
}

static int sys_chmod(const char *pathname, int mode) {
    uint64_t addr;
    vfs_node_t *node;
    int ret;

    if (!pathname) return -EFAULT;
    addr = (uint64_t)pathname;
    if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    if (!current_task) return -ESRCH;

    node = vfs_namei(pathname);
    if (!node) return -ENOENT;

    if (current_task->euid != 0 && current_task->euid != node->uid) {
        vfs_release(node);
        return -EPERM;
    }

    if (node->chmod) {
        ret = node->chmod(node, mode & 07777);
        vfs_release(node);
        return ret;
    }
    node->mask = mode & 07777;
    vfs_release(node);
    return 0;
}

static int sys_chown(const char *pathname, int owner, int group) {
    uint64_t addr;
    vfs_node_t *node;

    if (!pathname) return -EFAULT;
    addr = (uint64_t)pathname;
    if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    if (!current_task) return -ESRCH;

    if (current_task->euid != 0)
        return -EPERM;

    node = vfs_namei(pathname);
    if (!node) return -ENOENT;

    if (node->chown) {
        int r;
        r = node->chown(node, owner, group);
        vfs_release(node);
        return r;
    }
    if (owner != -1) node->uid = owner;
    if (group != -1) node->gid = group;
    vfs_release(node);
    return 0;
}

static int sys_lchown(const char *pathname, int owner, int group) {
    return sys_chown(pathname, owner, group);
}

#define ENV_NAME_SIZE 64
#define ENV_VALUE_SIZE 256

typedef struct {
    char *name;
    char *value;
} env_entry_t;

static env_entry_t *env_entries = NULL;
static int env_count = 0;
static char **environ_ptrs = NULL;
static int env_initialized = 0;
static int environ_dirty = 1;

static void env_invalidate_environ(void) {
    kfree(environ_ptrs);
    environ_ptrs = NULL;
    environ_dirty = 1;
}

static char *env_duplicate(const char *source, size_t limit) {
    size_t length;
    char *copy;

    length = 0;
    while (source[length] && length + 1 < limit) length++;
    copy = (char *)kmalloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static int env_add(const char *name, const char *value) {
    char *new_name;
    char *new_value;
    env_entry_t *new_entries;

    new_name = env_duplicate(name, ENV_NAME_SIZE);
    if (!new_name) return -1;
    new_value = env_duplicate(value, ENV_VALUE_SIZE);
    if (!new_value) {
        kfree(new_name);
        return -1;
    }
    new_entries = (env_entry_t *)krealloc(
        env_entries, (env_count + 1) * sizeof(env_entry_t));
    if (!new_entries) {
        kfree(new_name);
        kfree(new_value);
        return -1;
    }
    env_entries = new_entries;
    env_entries[env_count].name = new_name;
    env_entries[env_count].value = new_value;
    env_count++;
    env_invalidate_environ();
    return 0;
}

static void init_default_environ(void) {
    int i;

    if (env_initialized) return;
    if (env_add("PATH", "/bin:/usr/bin:/sbin:/usr/sbin") < 0) goto failed;
    if (env_add("HOME", "/root") < 0) goto failed;
    if (env_add("SHELL", "/bin/sh") < 0) goto failed;
    if (env_add("USER", "root") < 0) goto failed;
    if (env_add("TERM", "linux") < 0) goto failed;
    if (env_add("PWD", "/") < 0) goto failed;
    env_initialized = 1;
    return;

failed:
    for (i = 0; i < env_count; i++) {
        kfree(env_entries[i].name);
        kfree(env_entries[i].value);
    }
    kfree(env_entries);
    env_entries = NULL;
    env_count = 0;
    env_invalidate_environ();
}

static int find_env(const char *name) {
    int i;

    for (i = 0; i < env_count; i++) {
        if (strcmp(name, env_entries[i].name) == 0) return i;
    }
    return -1;
}

static int sys_getenv(const char *name, char *buf, int bufsize) {
    uint64_t name_addr;
    uint64_t buf_addr;
    int idx;
    int len;
    int i;

    if (!name || !buf) return -EFAULT;
    name_addr = (uint64_t)name;
    buf_addr = (uint64_t)buf;
    if (name_addr >= KERNEL_VMA || name_addr < 0x1000) return -EFAULT;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    
    init_default_environ();
    
    idx = find_env(name);
    if (idx < 0) return -ENOENT;

    len = 0;
    while (env_entries[idx].value[len]) len++;
    if (len + 1 > bufsize) return -ERANGE;

    for (i = 0; i <= len; i++) {
        buf[i] = env_entries[idx].value[i];
    }
    return len;
}

static int sys_setenv(const char *name, const char *value, int overwrite) {
    uint64_t name_addr;
    uint64_t value_addr;
    int idx;
    char *new_value;

    if (!name || !value) return -EFAULT;
    name_addr = (uint64_t)name;
    value_addr = (uint64_t)value;
    if (name_addr >= KERNEL_VMA || name_addr < 0x1000) return -EFAULT;
    if (value_addr >= KERNEL_VMA || value_addr < 0x1000) return -EFAULT;
    
    init_default_environ();
    
    idx = find_env(name);
    if (idx >= 0) {
        if (!overwrite) return 0;
        new_value = env_duplicate(value, ENV_VALUE_SIZE);
        if (!new_value) return -ENOMEM;
        kfree(env_entries[idx].value);
        env_entries[idx].value = new_value;
        env_invalidate_environ();
        return 0;
    }

    return env_add(name, value) < 0 ? -ENOMEM : 0;
}

static int sys_unsetenv(const char *name) {
    uint64_t addr;
    int idx;
    env_entry_t *new_entries;

    if (!name) return -EFAULT;
    addr = (uint64_t)name;
    if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;

    idx = find_env(name);
    if (idx < 0) return 0;

    kfree(env_entries[idx].name);
    kfree(env_entries[idx].value);
    if (idx + 1 < env_count) {
        memmove(&env_entries[idx], &env_entries[idx + 1],
                (env_count - idx - 1) * sizeof(env_entry_t));
    }
    env_count--;
    if (env_count == 0) {
        kfree(env_entries);
        env_entries = NULL;
    } else {
        new_entries = (env_entry_t *)krealloc(
            env_entries, env_count * sizeof(env_entry_t));
        if (new_entries) env_entries = new_entries;
    }
    env_invalidate_environ();
    return 0;
}

static int sys_clearenv(void) {
    int i;

    for (i = 0; i < env_count; i++) {
        kfree(env_entries[i].name);
        kfree(env_entries[i].value);
    }
    kfree(env_entries);
    env_entries = NULL;
    env_count = 0;
    env_initialized = 1;
    env_invalidate_environ();
    return 0;
}

char **get_environ(void) {
    int i;

    init_default_environ();
    if (!environ_dirty) return environ_ptrs;
    environ_ptrs = (char **)kmalloc((env_count + 1) * sizeof(char *));
    if (!environ_ptrs) return NULL;
    for (i = 0; i < env_count; i++) {
        environ_ptrs[i] = env_entries[i].value;
    }
    environ_ptrs[env_count] = NULL;
    environ_dirty = 0;
    return environ_ptrs;
}

static int sys_getpriority(int which, int who) {
    task_t *target;

    if (which < 0 || which > 2) return -EINVAL;
    if (which != 0) return -EINVAL;
    target = who == 0 ? current_task : task_find((pid_t)who);
    if (!target) return -ESRCH;
    return 20 - task_get_nice(target);
}

static int sys_setpriority(int which, int who, int prio) {
    task_t *target;
    int current_nice;

    if (which < 0 || which > 2) return -EINVAL;
    if (which != 0 || prio < -20 || prio > 19) return -EINVAL;
    target = who == 0 ? current_task : task_find((pid_t)who);
    if (!target) return -ESRCH;
    if (target != current_task && current_task->euid != 0 &&
        target->euid != current_task->euid) return -EPERM;
    current_nice = task_get_nice(target);
    if (prio < current_nice && current_task->euid != 0) return -EACCES;
    if (task_set_nice(target, prio) < 0) return -EINVAL;
    return 0;
}

typedef struct {
    int sched_priority;
} kernel_sched_param_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} kernel_sched_timespec_t;

static task_t *sched_target(int pid) {
    if (pid == 0) return current_task;
    return task_find((pid_t)pid);
}

static int sched_may_change(task_t *target, int policy, int priority) {
    if (!current_task || !target) return 0;
    if (target != current_task && current_task->euid != 0 &&
        target->euid != current_task->euid) return 0;
    if ((policy != 0 || priority != 0) && current_task->euid != 0) return 0;
    return 1;
}

static int sys_sched_setparam(int pid, const char *param_ptr, int unused) {
    kernel_sched_param_t param;
    task_t *target;
    int policy;

    (void)unused;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    if (copy_from_user(&param, param_ptr, sizeof(param)) < 0) return -EFAULT;
    policy = task_get_scheduler(target, NULL);
    if (!sched_may_change(target, policy, param.sched_priority)) return -EPERM;
    if (task_set_scheduler(target, policy, param.sched_priority) < 0)
        return -EINVAL;
    return 0;
}

static int sys_sched_getparam(int pid, const char *param_ptr, int unused) {
    kernel_sched_param_t param;
    task_t *target;

    (void)unused;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    task_get_scheduler(target, &param.sched_priority);
    if (copy_to_user((void *)param_ptr, &param, sizeof(param)) < 0)
        return -EFAULT;
    return 0;
}

static int sys_sched_setscheduler(int pid, const char *policy_ptr,
                                  int param_address) {
    kernel_sched_param_t param;
    task_t *target;
    int policy;

    policy = (int)(uintptr_t)policy_ptr;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    if (copy_from_user(&param, (const void *)(uintptr_t)param_address,
                       sizeof(param)) < 0) return -EFAULT;
    if (!sched_may_change(target, policy, param.sched_priority)) return -EPERM;
    if (task_set_scheduler(target, policy, param.sched_priority) < 0)
        return -EINVAL;
    return 0;
}

static int sys_sched_getscheduler(int pid, const char *unused1, int unused2) {
    task_t *target;

    (void)unused1;
    (void)unused2;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    return task_get_scheduler(target, NULL);
}

static int sys_sched_priority_max(int policy, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    if (policy == 0) return 0;
    if (policy == 1 || policy == 2) return 99;
    return -EINVAL;
}

static int sys_sched_priority_min(int policy, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    if (policy == 0) return 0;
    if (policy == 1 || policy == 2) return 1;
    return -EINVAL;
}

static int sys_sched_rr_interval(int pid, const char *interval_ptr, int unused) {
    kernel_sched_timespec_t interval;
    task_t *target;
    uint64_t frequency;

    (void)unused;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    frequency = pit_freq ? pit_freq : 1000;
    interval.tv_sec = 0;
    interval.tv_nsec = (int64_t)(3ULL * 1000000000ULL / frequency);
    if (copy_to_user((void *)interval_ptr, &interval, sizeof(interval)) < 0)
        return -EFAULT;
    return 0;
}

static int sys_posix_openpt(int flags) {
    (void)flags;
    return pty_open_master();
}

static int sys_grantpt(int fd) {
    return pty_grant(fd) == 0 ? 0 : -EBADF;
}

static int sys_unlockpt(int fd) {
    return pty_unlock(fd) == 0 ? 0 : -EBADF;
}

static int sys_ptsname(int fd, char *buf, int buflen) {
    char *name;
    int i;

    if (!buf || buflen <= 0) return -EINVAL;
    if ((uint64_t)buf < 0x1000 || (uint64_t)buf >= KERNEL_VMA) return -EFAULT;
    name = pty_name(fd);
    if (!name) return -ENOTTY;
    for (i = 0; name[i] && i < buflen - 1; i++) {
        buf[i] = name[i];
    }
    buf[i] = '\0';
    return 0;
}

static int sys_lke_load(const char *pathname) {
    if (!pathname) return -EFAULT;
    if ((uint64_t)pathname >= KERNEL_VMA || (uint64_t)pathname < 0x1000) return -EFAULT;
    if (!current_task || current_task->uid != 0) return -EPERM;
    return lke_load(pathname);
}

static int sys_lke_unload(const char *name) {
    if (!name) return -EFAULT;
    if ((uint64_t)name >= KERNEL_VMA || (uint64_t)name < 0x1000) return -EFAULT;
    if (!current_task || current_task->uid != 0) return -EPERM;
    return lke_unload(name);
}

static int sys_lke_list(char *buf, int size) {
    if (size < 0) return -EINVAL;
    if (!buf) return size == 0 ? lke_list(NULL, 0) : -EFAULT;
    if ((uint64_t)buf >= KERNEL_VMA || (uint64_t)buf < 0x1000) return -EFAULT;
    return lke_list(buf, size);
}

void syscalls_misc_init(void) {
    init_default_environ();
    
    syscall_table[SYSCALL_UNAME] = sys_uname;
    syscall_table[SYSCALL_SYSINFO] = sys_sysinfo;
    syscall_table[SYSCALL_GETRLIMIT] = sys_getrlimit;
    syscall_table[SYSCALL_SETRLIMIT] = sys_setrlimit;
    syscall_table[SYSCALL_GETRUSAGE] = sys_getrusage;
    syscall_table[SYSCALL_PRLIMIT64] = sys_prlimit64;
    syscall_table[SYSCALL_GETRANDOM] = sys_getrandom;
    syscall_table[SYSCALL_PRCTL] = sys_prctl;
    syscall_table[SYSCALL_ARCH_PRCTL] = sys_arch_prctl;
    syscall_table[SYSCALL_SET_TID_ADDRESS] = sys_set_tid_address;
    syscall_table[SYSCALL_SETITIMER] = sys_setitimer;
    syscall_table[SYSCALL_GETITIMER] = sys_getitimer;
    syscall_table[SYSCALL_ALARM] = sys_alarm;
    syscall_table[SYSCALL_NANOSLEEP] = sys_nanosleep;
    syscall_table[SYSCALL_CHMOD] = sys_chmod;
    syscall_table[SYSCALL_CHOWN] = sys_chown;
    syscall_table[SYSCALL_LCHOWN] = sys_lchown;
    syscall_table[SYSCALL_SETENV] = sys_setenv;
    syscall_table[SYSCALL_GETENV] = sys_getenv;
    syscall_table[SYSCALL_UNSETENV] = sys_unsetenv;
    syscall_table[SYSCALL_CLEARENV] = sys_clearenv;
    syscall_table[SYSCALL_POSIX_OPENPT] = sys_posix_openpt;
    syscall_table[SYSCALL_GRANTPT] = sys_grantpt;
    syscall_table[SYSCALL_UNLOCKPT] = sys_unlockpt;
    syscall_table[SYSCALL_PTSNAME] = sys_ptsname;
    syscall_table[SYSCALL_GETPRIORITY] = sys_getpriority;
    syscall_table[SYSCALL_SETPRIORITY] = sys_setpriority;
    syscall_table[SYSCALL_SCHED_SETPARAM] = sys_sched_setparam;
    syscall_table[SYSCALL_SCHED_GETPARAM] = sys_sched_getparam;
    syscall_table[SYSCALL_SCHED_SETSCHEDULER] = sys_sched_setscheduler;
    syscall_table[SYSCALL_SCHED_GETSCHEDULER] = sys_sched_getscheduler;
    syscall_table[SYSCALL_SCHED_GET_PRIORITY_MAX] = sys_sched_priority_max;
    syscall_table[SYSCALL_SCHED_GET_PRIORITY_MIN] = sys_sched_priority_min;
    syscall_table[SYSCALL_SCHED_RR_GET_INTERVAL] = sys_sched_rr_interval;
    syscall_table[SYSCALL_LKE_LOAD] = sys_lke_load;
    syscall_table[SYSCALL_LKE_UNLOAD] = sys_lke_unload;
    syscall_table[SYSCALL_LKE_LIST] = sys_lke_list;
}

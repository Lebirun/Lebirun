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

static int user_range_mapped_misc(uint64_t addr, uint64_t size) {
    uint64_t end;
    uint64_t p;
    uint64_t pend;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size - 1;
    if (end < addr) return 0;
    if (addr < 0x1000 || end >= KERNEL_VMA) return 0;
    p = addr & ~0xFFFu;
    pend = end & ~0xFFFu;
    for (;;) {
        if (vmm_get_phys_in_pml4(current_task->cr3, p) == 0) return 0;
        if (p == pend) break;
        if (p > 0xFFFFFFFFFFFFF000ULL) return 0;
        p += 0x1000u;
    }
    return 1;
}

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
    char version_str[65];
    int len;
    int i;
    
    if (!buf) return -EFAULT;
    if (!user_range_mapped_misc((uint64_t)(uintptr_t)buf, sizeof(struct utsname))) return -EFAULT;
    
    copy_string(buf->sysname, OS_NAME, 65);
    copy_string(buf->nodename, NODENAME, 65);
    copy_string(buf->release, OS_VERSION, 65);
    
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
    copy_string(buf->version, version_str, 65);
    
    copy_string(buf->machine, MACHINE, 65);
    
    return 0;
}

static int sys_sysinfo(struct sysinfo *info) {
    uint64_t total_kb;
    uint64_t free_pages;
    int proc_count;
    
    if (!info) return -EFAULT;
    if (!user_range_mapped_misc((uint64_t)(uintptr_t)info, sizeof(struct sysinfo))) return -EFAULT;
    
    memset(info, 0, sizeof(struct sysinfo));
    
    total_kb = pfa_get_total_ram_kb();
    free_pages = pfa_count_free();

    {
        extern void task_get_cached_stats(int *, int *, int *, pid_t *);
        task_get_cached_stats(&proc_count, NULL, NULL, NULL);
    }
    if (proc_count < 1) proc_count = 1;
    
    info->uptime = tick_count / pit_freq;
    info->loads[0] = 0;
    info->loads[1] = 0;
    info->loads[2] = 0;
    info->totalram = (unsigned long)total_kb * 1024;
    info->freeram = (unsigned long)(free_pages * 4) * 1024;
    info->sharedram = 0;
    info->bufferram = 0;
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = (unsigned short)proc_count;
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = 1;
    
    return 0;
}

static int sys_getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim) return -EFAULT;
    if (resource < 0 || resource >= RLIM_NLIMITS) return -EINVAL;
    
    rlim->rlim_cur = default_rlimits[resource].rlim_cur;
    rlim->rlim_max = default_rlimits[resource].rlim_max;
    
    return 0;
}

static int sys_setrlimit(int resource, const struct rlimit *rlim) {
    if (!rlim) return -EFAULT;
    if (resource < 0 || resource >= RLIM_NLIMITS) return -EINVAL;
    
    if (rlim->rlim_cur > default_rlimits[resource].rlim_max) return -EPERM;
    if (rlim->rlim_max > default_rlimits[resource].rlim_max) return -EPERM;
    
    default_rlimits[resource].rlim_cur = rlim->rlim_cur;
    default_rlimits[resource].rlim_max = rlim->rlim_max;
    
    return 0;
}

static int sys_getrusage(int who, struct rusage *usage) {
    if (!usage) return -EFAULT;
    
    memset(usage, 0, sizeof(struct rusage));
    
    if (who == RUSAGE_SELF || who == RUSAGE_THREAD) {
        usage->ru_utime.tv_sec = 0;
        usage->ru_utime.tv_usec = 0;
        usage->ru_stime.tv_sec = 0;
        usage->ru_stime.tv_usec = 0;
    } else if (who == RUSAGE_CHILDREN) {
        usage->ru_utime.tv_sec = 0;
        usage->ru_utime.tv_usec = 0;
        usage->ru_stime.tv_sec = 0;
        usage->ru_stime.tv_usec = 0;
    } else {
        return -EINVAL;
    }
    
    return 0;
}

static int sys_prlimit64(int pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit) {
    if (resource < 0 || resource >= RLIM_NLIMITS) return -EINVAL;
    
    if (pid != 0 && (!current_task || (pid_t)pid != current_task->pid)) {
        return -ESRCH;
    }
    
    if (old_limit) {
        old_limit->rlim_cur = default_rlimits[resource].rlim_cur;
        old_limit->rlim_max = default_rlimits[resource].rlim_max;
    }
    
    if (new_limit) {
        if (new_limit->rlim_cur > default_rlimits[resource].rlim_max) return -EPERM;
        if (new_limit->rlim_max > default_rlimits[resource].rlim_max) return -EPERM;
        
        default_rlimits[resource].rlim_cur = new_limit->rlim_cur;
        default_rlimits[resource].rlim_max = new_limit->rlim_max;
    }
    
    return 0;
}

static int sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    uint8_t tmp[256];
    uint8_t *dst;
    size_t done;
    size_t chunk;

    (void)flags;
    if (!buf) return -EFAULT;
    if (buflen == 0) return 0;
    if (!user_range_mapped_misc((uint64_t)(uintptr_t)buf, (uint64_t)buflen)) return -EFAULT;

    dst = (uint8_t *)buf;
    done = 0;
    while (done < buflen) {
        chunk = buflen - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        rng_fill(tmp, chunk);
        memcpy(dst + done, tmp, chunk);
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
    (void)arg3; (void)arg4; (void)arg5;
    switch (option) {
        case PR_SET_NAME:
            if (current_task && arg2) {
                const char *name = (const char *)arg2;
                int i;
                for (i = 0; i < 15 && name[i]; i++) {
                    current_task->name[i] = name[i];
                }
                current_task->name[i] = '\0';
            }
            return 0;
            
        case PR_GET_NAME:
            if (current_task && arg2) {
                char *name = (char *)arg2;
                int i;
                for (i = 0; i < 16 && current_task->name[i]; i++) {
                    name[i] = current_task->name[i];
                }
                name[i] = '\0';
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
                *(unsigned long *)addr = current_task->tls_base;
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

static int sys_inotify_init(int flags) {
    (void)flags;
    return -ENOSYS;
}

static int sys_inotify_add_watch(int fd, const char *pathname, uint64_t mask) {
    (void)fd; (void)pathname; (void)mask;
    return -ENOSYS;
}

static int sys_inotify_rm_watch(int fd, int wd) {
    (void)fd; (void)wd;
    return -ENOSYS;
}

static int sys_getpriority(int which, int who) {
    (void)who;
    if (which < 0 || which > 2) return -EINVAL;
    return 20;
}

static int sys_setpriority(int which, int who, int prio) {
    (void)who;
    (void)prio;
    if (which < 0 || which > 2) return -EINVAL;
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
    syscall_table[SYSCALL_INOTIFY_INIT] = sys_inotify_init;
    syscall_table[SYSCALL_INOTIFY_INIT1] = sys_inotify_init;
    syscall_table[SYSCALL_INOTIFY_ADD_WATCH] = sys_inotify_add_watch;
    syscall_table[SYSCALL_INOTIFY_RM_WATCH] = sys_inotify_rm_watch;
    syscall_table[SYSCALL_POSIX_OPENPT] = sys_posix_openpt;
    syscall_table[SYSCALL_GRANTPT] = sys_grantpt;
    syscall_table[SYSCALL_UNLOCKPT] = sys_unlockpt;
    syscall_table[SYSCALL_PTSNAME] = sys_ptsname;
    syscall_table[SYSCALL_GETPRIORITY] = sys_getpriority;
    syscall_table[SYSCALL_SETPRIORITY] = sys_setpriority;
    syscall_table[SYSCALL_LKE_LOAD] = sys_lke_load;
    syscall_table[SYSCALL_LKE_UNLOAD] = sys_lke_unload;
    syscall_table[SYSCALL_LKE_LIST] = sys_lke_list;
}

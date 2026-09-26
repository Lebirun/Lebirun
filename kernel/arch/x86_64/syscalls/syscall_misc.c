#include "syscall_defs.h"
#include <lebirun/lke.h>
#include <lebirun/about.h>
#include <lebirun/rng.h>
#include <lebirun/seccomp.h>
#include <lebirun/pty.h>
#include <lebirun/creds.h>
#include <lebirun/timekeeping.h>
#include <lebirun/smp.h>
#include <lebirun/vring.h>
#include <lebirun/mem_map.h>
#include <lebirun/pit.h>
#include <lebirun/vfs.h>
#include <string.h>

extern task_t *current_task;

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
    [RLIMIT_NPROC]      = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_NOFILE]     = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_MEMLOCK]    = { 65536, RLIM_INFINITY },
    [RLIMIT_AS]         = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_LOCKS]      = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_SIGPENDING] = { 1024, RLIM_INFINITY },
    [RLIMIT_MSGQUEUE]   = { 819200, RLIM_INFINITY },
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

static char kern_hostname[65] = NODENAME;
static char kern_domainname[65] = "localdomain";

static int sys_uname(struct utsname *buf) {
    struct utsname value;
    char version_str[65];
    int len;
    int i;
    
    if (!buf) return -EFAULT;
    memset(&value, 0, sizeof(value));
    copy_string(value.sysname, OS_NAME, 65);
    copy_string(value.nodename, kern_hostname, 65);
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
    uint64_t frequency;
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
    
    frequency = pit_freq ? pit_freq : 1;
    value.uptime = tick_count / frequency;
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
    uint64_t frequency;
    uint64_t user_ticks;
    uint64_t system_ticks;

    if (!usage) return -EFAULT;
    if (who != RUSAGE_SELF && who != RUSAGE_THREAD &&
        who != RUSAGE_CHILDREN) {
        return -EINVAL;
    }
    memset(&value, 0, sizeof(value));
    if (current_task) {
        frequency = pit_freq ? pit_freq : 1000;
        if (who == RUSAGE_CHILDREN) {
            user_ticks = current_task->child_utime;
            system_ticks = current_task->child_stime;
            value.ru_maxrss = (long)current_task->child_maxrss_kb;
        } else {
            user_ticks = current_task->utime;
            system_ticks = current_task->stime;
            value.ru_maxrss = (long)current_task->maxrss_kb;
            value.ru_minflt = (long)current_task->minor_faults;
            value.ru_majflt = (long)current_task->major_faults;
            value.ru_nvcsw =
                (long)current_task->voluntary_context_switches;
            value.ru_nivcsw =
                (long)current_task->involuntary_context_switches;
        }
        value.ru_utime.tv_sec = (long)(user_ticks / frequency);
        value.ru_utime.tv_usec = (long)((user_ticks % frequency) * 1000000ULL / frequency);
        value.ru_stime.tv_sec = (long)(system_ticks / frequency);
        value.ru_stime.tv_usec = (long)((system_ticks % frequency) * 1000000ULL / frequency);
    }
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

    if (flags & ~(0x0001u | 0x0002u)) return -EINVAL;
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
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_SET_SYSCALL_MASK 0x4C420001
#define PR_SET_SYSCALL_MASK2 0x4C420002
#define CLONE_NEWNS 0x00020000

static int sys_prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5) {
    char name[16];
    size_t length;
    uint64_t syscall_mask[(NR_SYSCALLS + 63) / 64];
    uint64_t *extended_mask;
    size_t syscall_words;
    int result;

    (void)arg4; (void)arg5;
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
            return creds_set_dumpable(current_task, (int)arg2);
            
        case PR_GET_DUMPABLE:
            return creds_get_dumpable(current_task);
            
        case PR_SET_NO_NEW_PRIVS:
            if (arg2 != 1) return -EINVAL;
            return creds_set_no_new_privs(current_task);

        case PR_GET_NO_NEW_PRIVS:
            return creds_get_no_new_privs(current_task);

        case PR_SET_SECCOMP:
            if (arg2 != 1 && arg2 != 2) return -EINVAL;
            if (!creds_get_no_new_privs(current_task) &&
                !creds_has_capability(current_task, 21)) return -EPERM;
            if (arg2 == 2)
                return seccomp_install(current_task,
                                           (const void *)arg3);
            return creds_set_strict_syscalls(current_task);

        case PR_GET_SECCOMP:
            return creds_get_syscall_filter_mode(current_task);

        case PR_SET_SYSCALL_MASK:
            syscall_words = (NR_SYSCALLS + 63) / 64;
            if (!arg2 || arg3 != syscall_words) return -EINVAL;
            if (!creds_get_no_new_privs(current_task) &&
                !creds_has_capability(current_task, 21)) return -EPERM;
            if (copy_from_user(syscall_mask, (const void *)arg2,
                               sizeof(syscall_mask)) != 0) return -EFAULT;
            return creds_set_syscall_mask(current_task, syscall_mask,
                                           syscall_words);

        case PR_SET_SYSCALL_MASK2:
            syscall_words = (size_t)arg3;
            if (!arg2 || syscall_words == 0 ||
                syscall_words > SIZE_MAX / sizeof(uint64_t) || arg4 > 1)
                return -EINVAL;
            if (!creds_get_no_new_privs(current_task) &&
                !creds_has_capability(current_task, 21)) return -EPERM;
            extended_mask = (uint64_t *)kmalloc(
                syscall_words * sizeof(uint64_t));
            if (!extended_mask) return -ENOMEM;
            if (copy_from_user(extended_mask, (const void *)arg2,
                               syscall_words * sizeof(uint64_t)) != 0) {
                kfree(extended_mask);
                return -EFAULT;
            }
            result = creds_set_syscall_mask_ex(current_task, extended_mask,
                                                syscall_words, (int)arg4);
            kfree(extended_mask);
            return result;
            
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
            return -EINVAL;

        case ARCH_GET_GS:
            if (addr && current_task) {
                if (copy_to_user((void *)addr, &current_task->tls_limit,
                                 sizeof(current_task->tls_limit)) < 0)
                    return -EFAULT;
            }
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
    uint64_t expiry[3];
    uint64_t interval[3];
} task_timer_data_t;

static volatile uint64_t timer_earliest_tick;

static int timer_which_signal(int which) {
    if (which == 1) return 26;
    if (which == 2) return 27;
    return 14;
}

static uint64_t timer_us_to_ticks(int64_t sec, int64_t usec) {
    uint64_t total_us;
    uint64_t ticks;

    if (sec < 0 || usec < 0 || usec >= 1000000L) return UINT64_MAX;
    if ((uint64_t)sec > (UINT64_MAX - (uint64_t)usec) / 1000000ULL)
        return UINT64_MAX;
    total_us = (uint64_t)sec * 1000000ULL + (uint64_t)usec;
    if (total_us == 0) return 0;
    if (pit_freq == 0) return UINT64_MAX;
    if (total_us > (UINT64_MAX - 999999ULL) / pit_freq)
        return UINT64_MAX - 1;
    ticks = total_us * pit_freq / 1000000ULL;
    if (ticks == 0) ticks = 1;
    return ticks;
}

static void timer_ticks_to_value(uint64_t ticks, long *sec_out,
                                 long *usec_out) {
    uint64_t freq;

    freq = pit_freq ? pit_freq : 1;
    *sec_out = (long)(ticks / freq);
    *usec_out = (long)((ticks % freq) * 1000000ULL / freq);
}

static void timer_earliest_update(uint64_t expiry) {
    uint64_t old;

    if (!expiry) return;
    for (;;) {
        old = timer_earliest_tick;
        if (old && old <= expiry) return;
        if (__sync_bool_compare_and_swap(&timer_earliest_tick, old,
                                         expiry)) return;
    }
}

static void timer_earliest_recompute(uint64_t next) {
    uint64_t old;

    for (;;) {
        old = timer_earliest_tick;
        if (old && next && old < next) return;
        if (old == next) return;
        if (__sync_bool_compare_and_swap(&timer_earliest_tick, old,
                                         next)) return;
    }
}

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
    struct itimerval_k current;
    uint64_t first;
    uint64_t repeat;

    if (which < 0 || which > 2) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (old_value &&
        ((uint64_t)old_value < 0x1000 || (uint64_t)old_value >= KERNEL_VMA))
        return -EFAULT;
    if (new_value &&
        ((uint64_t)new_value < 0x1000 || (uint64_t)new_value >= KERNEL_VMA))
        return -EFAULT;

    if (new_value) {
        if (copy_from_user(&value, new_value, sizeof(struct itimerval_k)) != 0)
            return -EFAULT;
        first = timer_us_to_ticks(value.it_value.tv_sec,
                                  value.it_value.tv_usec);
        repeat = timer_us_to_ticks(value.it_interval.tv_sec,
                                   value.it_interval.tv_usec);
        if (first == UINT64_MAX || repeat == UINT64_MAX) return -EINVAL;
        if (first == 0) {
            repeat = 0;
            memset(&value.it_interval, 0, sizeof(value.it_interval));
        }
    }

    lock_scheduler();
    timers = get_task_timer_data(0);
    if (old_value) {
        memset(&current, 0, sizeof(current));
        if (timers && timers->expiry[which] > tick_count) {
            timer_ticks_to_value(timers->expiry[which] - tick_count,
                                 &current.it_value.tv_sec,
                                 &current.it_value.tv_usec);
            timer_ticks_to_value(timers->interval[which],
                                 &current.it_interval.tv_sec,
                                 &current.it_interval.tv_usec);
        } else if (timers && timers->expiry[which]) {
            timer_ticks_to_value(timers->interval[which],
                                 &current.it_interval.tv_sec,
                                 &current.it_interval.tv_usec);
        }
    }

    if (new_value) {
        if (!timers && (first || repeat)) {
            timers = get_task_timer_data(1);
            if (!timers) {
                unlock_scheduler();
                return -ENOMEM;
            }
        }
        if (timers) {
            memcpy(&timers->values[which], &value,
                   sizeof(struct itimerval_k));
            timers->interval[which] = repeat;
            if (first) {
                timers->expiry[which] = tick_count + first;
                if (timers->expiry[which] < tick_count)
                    timers->expiry[which] = UINT64_MAX;
                timer_earliest_update(timers->expiry[which]);
            } else {
                timers->expiry[which] = 0;
            }
            if (timer_data_is_empty(timers) &&
                !timers->expiry[0] && !timers->expiry[1] &&
                !timers->expiry[2]) {
                kfree(timers);
                current_task->timer_data = NULL;
            }
        }
    }
    unlock_scheduler();

    if (old_value) {
        if (copy_to_user(old_value, &current, sizeof(current)) != 0)
            return -EFAULT;
    }

    return 0;
}

static int sys_getitimer(int which, struct itimerval_k *curr_value) {
    task_timer_data_t *timers;
    struct itimerval_k current;

    if (which < 0 || which > 2) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (!curr_value) return -EFAULT;
    if ((uint64_t)curr_value < 0x1000 || (uint64_t)curr_value >= KERNEL_VMA) return -EFAULT;

    memset(&current, 0, sizeof(current));
    lock_scheduler();
    timers = get_task_timer_data(0);
    if (timers) {
        if (timers->expiry[which] > tick_count) {
            timer_ticks_to_value(timers->expiry[which] - tick_count,
                                 &current.it_value.tv_sec,
                                 &current.it_value.tv_usec);
            timer_ticks_to_value(timers->interval[which],
                                 &current.it_interval.tv_sec,
                                 &current.it_interval.tv_usec);
        } else if (timers->expiry[which]) {
            timer_ticks_to_value(timers->interval[which],
                                 &current.it_interval.tv_sec,
                                 &current.it_interval.tv_usec);
        }
    }
    unlock_scheduler();

    if (copy_to_user(curr_value, &current, sizeof(current)) != 0)
        return -EFAULT;

    return 0;
}

void task_timer_check(void) {
    task_t *t;
    task_timer_data_t *timers;
    uint64_t now;
    uint64_t next;
    uint64_t address;
    int i;
    extern void posix_timers_check_tick(uint64_t now);

    if (!timer_earliest_tick) {
        posix_timers_check_tick(tick_count);
        return;
    }
    now = tick_count;
    if (now < timer_earliest_tick) return;
    next = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        address = (uint64_t)t;
        if (address < KERNEL_VMA) break;
        if ((address & 0xFFFF0000u) == 0xFEFE0000u) break;
        if (t->alarm_tick && now >= t->alarm_tick) {
            t->alarm_tick = 0;
            deliver_signal_to_task(t, 14);
        } else if (t->alarm_tick && (!next || t->alarm_tick < next)) {
            next = t->alarm_tick;
        }
        timers = (task_timer_data_t *)t->timer_data;
        if (timers) {
            for (i = 0; i < 3; i++) {
                if (!timers->expiry[i]) continue;
                if (now >= timers->expiry[i]) {
                    if (timers->interval[i] &&
                        timers->interval[i] < UINT64_MAX / 2) {
                        timers->expiry[i] += timers->interval[i];
                        if (timers->expiry[i] <= now ||
                            timers->expiry[i] < timers->interval[i])
                            timers->expiry[i] = now +
                                timers->interval[i];
                        if (!timers->expiry[i]) timers->expiry[i] = 1;
                    } else {
                        timers->expiry[i] = 0;
                    }
                    deliver_signal_to_task(t, timer_which_signal(i));
                }
                if (timers->expiry[i] &&
                    (!next || timers->expiry[i] < next))
                    next = timers->expiry[i];
            }
            if (timer_data_is_empty(timers) && !timers->expiry[0] &&
                !timers->expiry[1] && !timers->expiry[2]) {
                kfree(timers);
                t->timer_data = NULL;
            }
        }
        t = t->all_next;
    }
    timer_earliest_recompute(next);
    unlock_scheduler();
    posix_timers_check_tick(now);
}

static int sys_alarm(int seconds, const char *unused1, int unused2) {
    uint64_t old_alarm;
    
    (void)unused1; (void)unused2;
    if (!current_task) return 0;
    
    old_alarm = current_task->alarm_tick;
    
    if (seconds > 0) {
        current_task->alarm_tick = tick_count + (seconds * pit_freq);
        timer_earliest_update(current_task->alarm_tick);
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
    struct kernel_timespec remaining;
    long long *ts_ptr;
    uint64_t a0;
    uint64_t requested_ns;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint64_t remaining_ns;
    uint64_t sleep_ticks_count;
    extern int task_has_pending_signals(void);

    a0 = (uint64_t)arg0;

    if (a0 < 32) {
        ts_ptr = (long long *)(uint64_t)arg2;
        rem = (struct kernel_timespec *)(uint64_t)arg3;
        if (!ts_ptr) return -EFAULT;
        if ((uint64_t)ts_ptr < 0x1000 || (uint64_t)ts_ptr >= KERNEL_VMA)
            return -EFAULT;
        if (copy_from_user(&ts64, ts_ptr, sizeof(ts64)) != 0)
            return -EFAULT;
        req = &ts64;
    } else {
        req = (const struct kernel_timespec *)a0;
        rem = (struct kernel_timespec *)(uint64_t)arg1;
        if (!req) return -EFAULT;
        if ((uint64_t)req < 0x1000 || (uint64_t)req >= KERNEL_VMA)
            return -EFAULT;
        if (copy_from_user(&ts64, req, sizeof(ts64)) != 0)
            return -EFAULT;
        req = &ts64;
    }

    if (req->tv_sec < 0 || req->tv_nsec < 0 ||
        req->tv_nsec >= 1000000000L) return -EINVAL;
    if ((uint64_t)req->tv_sec >
        (UINT64_MAX - (uint64_t)req->tv_nsec) / 1000000000ULL)
        return -EINVAL;
    requested_ns = (uint64_t)req->tv_sec * 1000000000ULL +
                   (uint64_t)req->tv_nsec;
    start_ns = timekeeping_monotonic_ns();
    if (requested_ns != 0) {
        if (pit_freq == 0) return -EINVAL;
        if (requested_ns >
            (UINT64_MAX - 999999999ULL) / pit_freq)
            sleep_ticks_count = UINT64_MAX;
        else
            sleep_ticks_count =
                (requested_ns * pit_freq + 999999999ULL) / 1000000000ULL;
        if (sleep_ticks_count == 0) sleep_ticks_count = 1;
        sleep_ticks(sleep_ticks_count);
    }

    if (task_has_pending_signals()) {
        if (rem) {
            elapsed_ns = timekeeping_monotonic_ns() - start_ns;
            remaining_ns = elapsed_ns < requested_ns ?
                           requested_ns - elapsed_ns : 0;
            remaining.tv_sec = (long)(remaining_ns / 1000000000ULL);
            remaining.tv_nsec = (long)(remaining_ns % 1000000000ULL);
            if (copy_to_user(rem, &remaining, sizeof(remaining)) != 0)
                return -EFAULT;
        }
        return -EINTR;
    }

    if (rem) {
        memset(&remaining, 0, sizeof(remaining));
        if (copy_to_user(rem, &remaining, sizeof(remaining)) != 0)
            return -EFAULT;
    }

    return 0;
}

static int sys_chmod(const char *pathname, int mode) {
    char *kpath;
    vfs_node_t *node;
    int ret;

    if (!pathname) return -EFAULT;
    if (!current_task) return -ESRCH;
    kpath = copy_string_from_user_alloc(pathname);
    if (!kpath) return -EFAULT;

    node = vfs_namei(kpath);
    kfree(kpath);
    if (!node) return -ENOENT;

    if (current_task->euid != 0 && current_task->euid != node->uid) {
        vfs_release(node);
        return -EPERM;
    }

    if (node->ops && node->ops->chmod) {
        ret = node->ops->chmod(node, mode & 07777);
        vfs_release(node);
        return ret;
    }
    node->mask = mode & 07777;
    vfs_release(node);
    return 0;
}

static int sys_chown(const char *pathname, int owner, int group) {
    char *kpath;
    vfs_node_t *node;

    if (!pathname) return -EFAULT;
    if (!current_task) return -ESRCH;

    if (current_task->euid != 0)
        return -EPERM;

    kpath = copy_string_from_user_alloc(pathname);
    if (!kpath) return -EFAULT;
    node = vfs_namei(kpath);
    kfree(kpath);
    if (!node) return -ENOENT;

    if (node->ops && node->ops->chown) {
        int r;
        r = node->ops->chown(node, owner, group);
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

static char *env_duplicate(const char *source) {
    size_t length;
    char *copy;

    if (!source) return NULL;
    length = strlen(source);
    if (length == SIZE_MAX) return NULL;
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

    new_name = env_duplicate(name);
    if (!new_name) return -1;
    new_value = env_duplicate(value);
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
    uint64_t buf_addr;
    char *kname;
    int idx;
    int len;
    int i;

    if (!name) return -EFAULT;
    kname = copy_string_from_user_alloc(name);
    if (!kname) return -EFAULT;
    init_default_environ();
    idx = find_env(kname);
    kfree(kname);
    if (idx < 0) return -ENOENT;

    len = 0;
    while (env_entries[idx].value[len]) len++;
    if (!buf && bufsize == 0) return len + 1;
    if (!buf || bufsize <= 0) return -EFAULT;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (len + 1 > bufsize) return -ERANGE;

    for (i = 0; i <= len; i++) {
        if (copy_to_user(&buf[i], &env_entries[idx].value[i], 1) != 0) return -EFAULT;
    }
    return len;
}

static int sys_setenv(const char *name, const char *value, int overwrite) {
    int idx;
    char *kname;
    char *kvalue;
    char *new_value;
    int ret;

    if (!name || !value) return -EFAULT;
    kname = copy_string_from_user_alloc(name);
    if (!kname) return -EFAULT;
    kvalue = copy_string_from_user_alloc(value);
    if (!kvalue) { kfree(kname); return -EFAULT; }
    init_default_environ();
    idx = find_env(kname);
    if (idx >= 0) {
        if (!overwrite) { kfree(kname); kfree(kvalue); return 0; }
        new_value = env_duplicate(kvalue);
        kfree(kname);
        kfree(kvalue);
        if (!new_value) return -ENOMEM;
        kfree(env_entries[idx].value);
        env_entries[idx].value = new_value;
        env_invalidate_environ();
        return 0;
    }

    ret = env_add(kname, kvalue);
    kfree(kname);
    kfree(kvalue);
    return ret < 0 ? -ENOMEM : 0;
}

static int sys_unsetenv(const char *name) {
    int idx;
    char *kname;
    env_entry_t *new_entries;

    if (!name) return -EFAULT;
    kname = copy_string_from_user_alloc(name);
    if (!kname) return -EFAULT;

    idx = find_env(kname);
    kfree(kname);
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
    return pty_open_path("/dev/ptmx", flags);
}

static int sys_grantpt(int fd) {
    int endpoint;

    endpoint = pty_task_endpoint(fd);
    return endpoint >= 0 && pty_grant(endpoint) == 0 ? 0 : -EBADF;
}

static int sys_unlockpt(int fd) {
    int endpoint;

    endpoint = pty_task_endpoint(fd);
    return endpoint >= 0 && pty_unlock(endpoint) == 0 ? 0 : -EBADF;
}

static int sys_ptsname(int fd, char *buf, int buflen) {
    char name[32];
    size_t len;
    int endpoint;

    if (!buf || buflen <= 0) return -EINVAL;
    if ((uint64_t)buf < 0x1000 || (uint64_t)buf >= KERNEL_VMA) return -EFAULT;
    endpoint = pty_task_endpoint(fd);
    if (endpoint < 0 || pty_name(endpoint, name, sizeof(name)) < 0)
        return -ENOTTY;
    len = strlen(name);
    if (len + 1 > (size_t)buflen) len = (size_t)buflen - 1;
    if (copy_to_user(buf, name, len + 1) != 0) return -EFAULT;
    return 0;
}

static int sys_lke_load(const char *pathname) {
#if CONFIG_KERNEL_LKE
    if (!pathname) return -EFAULT;
    if ((uint64_t)pathname >= KERNEL_VMA || (uint64_t)pathname < 0x1000) return -EFAULT;
    if (!current_task || current_task->uid != 0) return -EPERM;
    return lke_load(pathname);
#else
    (void)pathname;
    return -ENOSYS;
#endif
}

static int sys_lke_unload(const char *name) {
#if CONFIG_KERNEL_LKE
    if (!name) return -EFAULT;
    if ((uint64_t)name >= KERNEL_VMA || (uint64_t)name < 0x1000) return -EFAULT;
    if (!current_task || current_task->uid != 0) return -EPERM;
    return lke_unload(name);
#else
    (void)name;
    return -ENOSYS;
#endif
}

static int sys_lke_list(char *buf, int size) {
#if CONFIG_KERNEL_LKE
    if (size < 0) return -EINVAL;
    if (!buf) return size == 0 ? lke_list(NULL, 0) : -EFAULT;
    if ((uint64_t)buf >= KERNEL_VMA || (uint64_t)buf < 0x1000) return -EFAULT;
    return lke_list(buf, size);
#else
    (void)buf;
    (void)size;
    return -ENOSYS;
#endif
}

static int sys_sched_setaffinity(int pid, const char *mask_ptr, int len) {
    uint64_t mask64;
    uint32_t mask;
    task_t *target;

    if (len != 4 && len != 8) return -EINVAL;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    if (target != current_task && current_task->euid != 0 &&
        target->euid != current_task->euid) return -EPERM;
    if (len == 8) {
        if (copy_from_user(&mask64, mask_ptr, sizeof(mask64)) < 0) return -EFAULT;
        if (task_set_cpu_affinity64(target, mask64) < 0) return -EINVAL;
        return 0;
    }
    if (copy_from_user(&mask, mask_ptr, sizeof(mask)) < 0) return -EFAULT;
    if (task_set_cpu_affinity(target, mask) < 0) return -EINVAL;
    return 0;
}

static int sys_sched_getaffinity(int pid, const char *mask_ptr, int len) {
    uint64_t mask64;
    uint32_t mask;
    task_t *target;

    if (len < 4) return -EINVAL;
    target = sched_target(pid);
    if (!target) return -ESRCH;
    if (len >= 8) {
        mask64 = task_get_cpu_affinity64(target);
        if (copy_to_user((void *)mask_ptr, &mask64, sizeof(mask64)) < 0)
            return -EFAULT;
        return 8;
    }
    mask = task_get_cpu_affinity(target);
    if (copy_to_user((void *)mask_ptr, &mask, sizeof(mask)) < 0)
        return -EFAULT;
    return 4;
}

static int hostname_set(char *dst, const char *src, size_t len) {
    if (!src || len == 0 || len > 64) return -EINVAL;
    if (current_task && current_task->uid != 0 && current_task->euid != 0)
        return -EPERM;
    if (copy_from_user(dst, src, len) < 0) return -EFAULT;
    dst[len] = '\0';
    return 0;
}

static int hostname_get(char *dst, const char *src, size_t len) {
    size_t have;
    size_t copy;

    if (!dst || len == 0) return -EINVAL;
    have = strlen(src) + 1;
    copy = have < len ? have : len;
    if (copy_to_user(dst, src, copy) < 0) return -EFAULT;
    return 0;
}

static int sys_sethostname(const char *name, size_t len) {
    return hostname_set(kern_hostname, name, len);
}

static int sys_gethostname(char *name, size_t len) {
    return hostname_get(name, kern_hostname, len);
}

static int sys_setdomainname(const char *name, size_t len) {
    return hostname_set(kern_domainname, name, len);
}

static int sys_getdomainname(char *name, size_t len) {
    return hostname_get(name, kern_domainname, len);
}

static int sys_syslog(int type, char *buf, int len) {
    char chunk[1024];
    int total;
    int done;
    int want;
    int piece;

    if (type == 9 || type == 10) {
        total = klog_snapshot(NULL, 0);
        return total < 0 ? 0 : total;
    }
    if (type == 0 || type == 1 || type == 5 || type == 6 || type == 7)
        return 0;
    if (type == 8) {
        if (len < 1 || len > 8) return -EINVAL;
        return 0;
    }
    if (type != 2 && type != 3 && type != 4) return -EINVAL;
    total = klog_snapshot(NULL, 0);
    if (total <= 0) return 0;
    if (!buf || len <= 0) return -EINVAL;
    want = total < len ? total : len;
    done = 0;
    while (done < want) {
        piece = want - done;
        if (piece > (int)sizeof(chunk)) piece = sizeof(chunk);
        klog_snapshot_range(chunk, done, piece);
        if (copy_to_user(buf + done, chunk, (size_t)piece) < 0)
            return -EFAULT;
        done += piece;
    }
    return done;
}

struct tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

static int sys_times(struct tms *buf) {
    struct tms value;
    uint64_t frequency;

    memset(&value, 0, sizeof(value));
    frequency = pit_freq ? pit_freq : 1000;
    if (current_task) {
        value.tms_utime = (long)(current_task->utime * 100 / frequency);
        value.tms_stime = (long)(current_task->stime * 100 / frequency);
        value.tms_cutime =
            (long)(current_task->child_utime * 100 / frequency);
        value.tms_cstime =
            (long)(current_task->child_stime * 100 / frequency);
    }
    if (buf && copy_to_user(buf, &value, sizeof(value)) < 0)
        return -EFAULT;
    return (int)(tick_count * 100 / frequency);
}

#define MEMBARRIER_CMD_QUERY 0
#define MEMBARRIER_CMD_GLOBAL 1
#define MEMBARRIER_CMD_PRIVATE 8
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED 16
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED 32

static int sys_membarrier(int cmd, unsigned int flags) {
    if (flags != 0) return -EINVAL;
    if (cmd == MEMBARRIER_CMD_QUERY)
        return MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_PRIVATE |
            MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED |
            MEMBARRIER_CMD_PRIVATE_EXPEDITED;
    if (cmd == MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED) return 0;
    if (cmd == MEMBARRIER_CMD_GLOBAL || cmd == MEMBARRIER_CMD_PRIVATE ||
        cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED) {
        __sync_synchronize();
        return 0;
    }
    return -EINVAL;
}

typedef struct posix_timer_node {
    int id;
    uint64_t expiry;
    uint64_t interval;
    uint64_t overrun;
    int signo;
    struct posix_timer_node *next;
} posix_timer_node_t;

static int posix_timer_seq = 1;

void posix_timers_release_task(task_t *task) {
    task_ext_t *e;
    posix_timer_node_t *t;
    if (!task) return;
    e = task_ext_get(task, 0);
    if (!e) return;
    t = (posix_timer_node_t *)e->posix_timers;
    while (t) {
        posix_timer_node_t *n = t->next;
        kfree(t);
        t = n;
    }
    e->posix_timers = NULL;
}

static int sys_timer_create(int unused, const char *unused2, int unused3) {
    posix_timer_node_t *t;
    task_ext_t *e;
    (void)unused; (void)unused2; (void)unused3;
    if (!current_task) return -ESRCH;
    e = task_ext_get(current_task, 1);
    if (!e) return -ENOMEM;
    t = (posix_timer_node_t *)kmalloc(sizeof(posix_timer_node_t));
    if (!t) return -ENOMEM;
    memset(t, 0, sizeof(*t));
    t->id = posix_timer_seq++;
    if (posix_timer_seq <= 0) posix_timer_seq = 1;
    t->next = (posix_timer_node_t *)e->posix_timers;
    e->posix_timers = t;
    return t->id;
}

static int sys_timer_settime(int id, const char *ms_ptr, int interval_ms) {
    posix_timer_node_t *t;
    task_ext_t *e;
    uint64_t ms;
    if (!current_task) return -ESRCH;
    ms = (uint64_t)(uintptr_t)ms_ptr;
    e = task_ext_get(current_task, 0);
    if (!e) return -EINVAL;
    t = (posix_timer_node_t *)e->posix_timers;
    while (t) {
        if (t->id == id) break;
        t = t->next;
    }
    if (!t) return -EINVAL;
    if (ms == 0 && interval_ms <= 0) {
        t->expiry = 0;
        t->interval = 0;
        t->overrun = 0;
        return 0;
    }
    t->expiry = pit_get_ticks() + pit_ms_to_ticks(ms);
    if (!t->expiry) t->expiry = 1;
    t->interval = pit_ms_to_ticks((uint64_t)(interval_ms < 0 ? 0 : interval_ms));
    t->overrun = 0;
    if (t->signo == 0) t->signo = 14;
    return 0;
}

static int sys_timer_gettime(int id, const char *ms_ptr, int unused) {
    posix_timer_node_t *t;
    task_ext_t *e;
    uint64_t *out;
    uint64_t now;
    uint64_t left[2];
    (void)unused;
    if (!current_task) return -ESRCH;
    if (!ms_ptr) return -EFAULT;
    out = (uint64_t *)(uintptr_t)ms_ptr;
    e = task_ext_get(current_task, 0);
    if (!e) return -EINVAL;
    t = (posix_timer_node_t *)e->posix_timers;
    while (t) {
        if (t->id == id) break;
        t = t->next;
    }
    if (!t) return -EINVAL;
    now = pit_get_ticks();
    if (!t->expiry || t->expiry <= now) {
        left[0] = 0;
    } else {
        left[0] = pit_ticks_to_ms(t->expiry - now);
    }
    left[1] = pit_ticks_to_ms(t->interval);
    if (copy_to_user(out, left, sizeof(left)) != 0) return -EFAULT;
    return 0;
}

static int sys_timer_getoverrun(int id, const char *unused2, int unused3) {
    posix_timer_node_t *t;
    task_ext_t *e;
    uint64_t v;
    (void)unused2; (void)unused3;
    if (!current_task) return -ESRCH;
    e = task_ext_get(current_task, 0);
    if (!e) return -EINVAL;
    t = (posix_timer_node_t *)e->posix_timers;
    while (t) {
        if (t->id == id) break;
        t = t->next;
    }
    if (!t) return -EINVAL;
    v = t->overrun;
    t->overrun = 0;
    if (v > 99) v = 99;
    return (int)v;
}

void posix_timers_check_tick(uint64_t now) {
    task_t *t;
    task_ext_t *e;
    posix_timer_node_t *n;
    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        if ((uint64_t)t < KERNEL_VMA) break;
        e = task_ext_get(t, 0);
        if (e && e->posix_timers) {
            n = (posix_timer_node_t *)e->posix_timers;
            while (n) {
                if (n->expiry && now >= n->expiry) {
                    if (n->interval) {
                        uint64_t missed = (now - n->expiry) / n->interval;
                        n->overrun += missed + 1;
                        if (n->overrun > 99) n->overrun = 99;
                        n->expiry += (missed + 1) * n->interval;
                        if (n->expiry <= now || !n->expiry)
                            n->expiry = now + n->interval;
                    } else {
                        n->expiry = 0;
                        n->overrun++;
                    }
                    deliver_signal_to_task(t, n->signo ? n->signo : 14);
                }
                n = n->next;
            }
        }
        t = t->all_next;
    }
    unlock_scheduler();
}

static int sys_timer_delete(int id, const char *unused2, int unused3) {
    posix_timer_node_t **pp;
    task_ext_t *e;
    (void)unused2; (void)unused3;
    if (!current_task) return -ESRCH;
    e = task_ext_get(current_task, 0);
    if (!e) return -EINVAL;
    pp = (posix_timer_node_t **)&e->posix_timers;
    while (*pp) {
        if ((*pp)->id == id) {
            posix_timer_node_t *dead = *pp;
            *pp = dead->next;
            kfree(dead);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -EINVAL;
}

typedef struct mq_msg {
    uint8_t *data;
    uint64_t len;
    unsigned prio;
    struct mq_msg *next;
} mq_msg_t;

typedef struct mq_entry {
    char *name;
    mq_msg_t *head;
    mq_msg_t *tail;
    struct mq_entry *next;
} mq_entry_t;

static mq_entry_t *mq_list = NULL;

static mq_entry_t *mq_find(const char *name) {
    mq_entry_t *e = mq_list;
    while (e) {
        if (name && e->name && strcmp(e->name, name) == 0) return e;
        e = e->next;
    }
    return NULL;
}

static int sys_mq_open(const char *name_ptr, const char *unused2, int unused3) {
    char tmp[65];
    mq_entry_t *e;
    size_t n;
    size_t length;
    (void)unused2; (void)unused3;
    if (!name_ptr) return -EFAULT;
    if (strnlen_user(name_ptr, sizeof(tmp), &length) != 0) return -EFAULT;
    if (length >= sizeof(tmp)) return -ENAMETOOLONG;
    if (copy_from_user(tmp, name_ptr, length + 1) != 0) return -EFAULT;
    e = mq_find(tmp);
    if (e) return 0;
    e = (mq_entry_t *)kmalloc(sizeof(mq_entry_t));
    if (!e) return -ENOMEM;
    n = strlen(tmp) + 1;
    e->name = (char *)kmalloc(n);
    if (!e->name) { kfree(e); return -ENOMEM; }
    memcpy(e->name, tmp, n);
    e->head = NULL;
    e->tail = NULL;
    e->next = mq_list;
    mq_list = e;
    return 0;
}

static int sys_mq_send(const char *name_ptr, const char *buf_ptr, int len) {
    mq_entry_t *e;
    mq_msg_t *m;
    char tmp[65];
    uint8_t *d;
    size_t length;
    if (!name_ptr || !buf_ptr || len <= 0) return -EINVAL;
    if (strnlen_user(name_ptr, sizeof(tmp), &length) != 0) return -EFAULT;
    if (length >= sizeof(tmp)) return -ENAMETOOLONG;
    if (copy_from_user(tmp, name_ptr, length + 1) != 0) return -EFAULT;
    e = mq_find(tmp);
    if (!e) return -ENOENT;
    d = (uint8_t *)kmalloc((size_t)len);
    if (!d) return -ENOMEM;
    if (copy_from_user(d, buf_ptr, (size_t)len) != 0) { kfree(d); return -EFAULT; }
    m = (mq_msg_t *)kmalloc(sizeof(mq_msg_t));
    if (!m) { kfree(d); return -ENOMEM; }
    m->data = d;
    m->len = (uint64_t)len;
    m->prio = 0;
    m->next = NULL;
    if (e->tail) e->tail->next = m;
    else e->head = m;
    e->tail = m;
    return len;
}

static int sys_mq_receive(const char *name_ptr, const char *buf_ptr, int buflen) {
    mq_entry_t *e;
    mq_msg_t *m;
    char tmp[65];
    uint64_t n;
    size_t length;
    if (!name_ptr || !buf_ptr || buflen <= 0) return -EINVAL;
    if (strnlen_user(name_ptr, sizeof(tmp), &length) != 0) return -EFAULT;
    if (length >= sizeof(tmp)) return -ENAMETOOLONG;
    if (copy_from_user(tmp, name_ptr, length + 1) != 0) return -EFAULT;
    e = mq_find(tmp);
    if (!e || !e->head) return -EAGAIN;
    m = e->head;
    e->head = m->next;
    if (!e->head) e->tail = NULL;
    n = m->len < (uint64_t)buflen ? m->len : (uint64_t)buflen;
    if (copy_to_user((void *)buf_ptr, m->data, (size_t)n) != 0) {
        kfree(m->data);
        kfree(m);
        return -EFAULT;
    }
    kfree(m->data);
    kfree(m);
    return (int)n;
}

typedef struct sem_seg {
    int key;
    int *vals;
    int n;
    struct sem_seg *next;
} sem_seg_t;

static sem_seg_t *sem_list = NULL;

static int sys_semget(int key, const char *nsem_ptr, int flag) {
    int nsem = (int)(uintptr_t)nsem_ptr;
    sem_seg_t *s;
    (void)flag;
    if (nsem <= 0) return -EINVAL;
    for (s = sem_list; s; s = s->next) {
        if (s->key == key) return key;
    }
    s = (sem_seg_t *)kmalloc(sizeof(sem_seg_t));
    if (!s) return -ENOMEM;
    s->vals = (int *)kmalloc(sizeof(int) * (size_t)nsem);
    if (!s->vals) { kfree(s); return -ENOMEM; }
    memset(s->vals, 0, sizeof(int) * (size_t)nsem);
    s->key = key;
    s->n = nsem;
    s->next = sem_list;
    sem_list = s;
    return key;
}

typedef struct msg_node {
    uint8_t *data;
    uint64_t len;
    long type;
    struct msg_node *next;
} msg_node_t;

typedef struct msg_seg {
    int key;
    msg_node_t *head;
    msg_node_t *tail;
    struct msg_seg *next;
} msg_seg_t;

static msg_seg_t *msg_list = NULL;

static int sys_msgget(int key, const char *unused2, int unused3) {
    msg_seg_t *s;
    (void)unused2; (void)unused3;
    for (s = msg_list; s; s = s->next) {
        if (s->key == key) return key;
    }
    s = (msg_seg_t *)kmalloc(sizeof(msg_seg_t));
    if (!s) return -ENOMEM;
    memset(s, 0, sizeof(*s));
    s->key = key;
    s->next = msg_list;
    msg_list = s;
    return key;
}

static uint64_t ns_seq = 1;

static int sys_unshare(int flags, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    if (!current_task) return -ESRCH;
    if (flags & CLONE_NEWNS) {
        if (vfs_unshare_ns(current_task) != 0) return -ENOMEM;
    }
    current_task->ns_id = ns_seq++;
    return 0;
}

static int sys_seccomp(int op, int flags, const void *prog) {
    if (!current_task) return -ESRCH;
    if (op == 0) return 0;
    if (op != 1) return -EINVAL;
    if (flags != 0) return -EINVAL;
    if (!creds_get_no_new_privs(current_task) &&
        !creds_has_capability(current_task, 21)) return -EPERM;
    return seccomp_install(current_task, prog);
}

static int sys_splice(int fd_in, const char *fd_out_ptr, int len) {
    task_fd_t *a;
    task_fd_t *b;
    int fd_out = (int)(uintptr_t)fd_out_ptr;
    vfs_node_t *na;
    vfs_node_t *nb;
    uint8_t buf[4096];
    uint64_t in_pos;
    uint64_t out_pos;
    uint64_t total;
    uint64_t want;
    uint64_t got;
    uint64_t put;
    if (!current_task || len <= 0) return -EINVAL;
    if (fd_in < 0 || fd_in >= current_task->fds_capacity) return -EBADF;
    if (fd_out < 0 || fd_out >= current_task->fds_capacity) return -EBADF;
    a = &current_task->fds[fd_in];
    b = &current_task->fds[fd_out];
    if (!a->in_use || !b->in_use) return -EBADF;
    if (!a->node || !b->node) return -EBADF;
    na = (vfs_node_t *)a->node;
    nb = (vfs_node_t *)b->node;
    in_pos = a->offset;
    out_pos = b->offset;
    total = 0;
    while (total < (uint64_t)len) {
        want = (uint64_t)len - total;
        if (want > sizeof(buf)) want = sizeof(buf);
        got = vfs_read(na, in_pos + total, want, buf);
        if (got > want) got = want;
        if (got == 0) break;
        put = vfs_write(nb, out_pos + total, got, buf);
        if (put > got) put = got;
        if (put == 0) {
            if (total > 0) break;
            return -EIO;
        }
        total += put;
        if (put < got) break;
    }
    a->offset += total;
    b->offset += total;
    return (int)total;
}

static int sys_tee(int fd_in, const char *fd_out_ptr, int len) {
    task_fd_t *a;
    task_fd_t *b;
    int fd_out = (int)(uintptr_t)fd_out_ptr;
    vfs_node_t *na;
    vfs_node_t *nb;
    uint8_t buf[4096];
    uint64_t in_pos;
    uint64_t out_pos;
    uint64_t total;
    uint64_t want;
    uint64_t got;
    uint64_t put;
    if (!current_task || len <= 0) return -EINVAL;
    if (fd_in < 0 || fd_in >= current_task->fds_capacity) return -EBADF;
    if (fd_out < 0 || fd_out >= current_task->fds_capacity) return -EBADF;
    a = &current_task->fds[fd_in];
    b = &current_task->fds[fd_out];
    if (!a->in_use || !b->in_use) return -EBADF;
    if (!a->node || !b->node) return -EBADF;
    na = (vfs_node_t *)a->node;
    nb = (vfs_node_t *)b->node;
    in_pos = a->offset;
    out_pos = b->offset;
    total = 0;
    while (total < (uint64_t)len) {
        want = (uint64_t)len - total;
        if (want > sizeof(buf)) want = sizeof(buf);
        got = vfs_read(na, in_pos + total, want, buf);
        if (got > want) got = want;
        if (got == 0) break;
        put = vfs_write(nb, out_pos + total, got, buf);
        if (put > got) put = got;
        if (put == 0) {
            if (total > 0) break;
            return -EIO;
        }
        total += put;
        if (put < got) break;
    }
    b->offset += total;
    return (int)total;
}

static int sys_vring_path_rule(int minor, const char *range_ptr, int perms) {
    uint64_t *range = (uint64_t *)(uintptr_t)range_ptr;
    uint64_t r[2];
    if (minor <= 0 || minor > 255) return -EINVAL;
    if (!range) return -EFAULT;
    if (copy_from_user(r, range, sizeof(r)) != 0) return -EFAULT;
    if (r[0] >= r[1] || perms == 0) return -EINVAL;
    return vring_add_region((uint8_t)minor, r[0], r[1], (uint8_t)perms);
}

static int sys_ktls_tx(int sockfd, const char *unused2, int unused3) {
    (void)sockfd; (void)unused2; (void)unused3;
    return 0;
}

void syscalls_misc_init(void) {
    init_default_environ();
    
    syscall_table_set(SYSCALL_UNAME, (void *)(sys_uname));
    syscall_table_set(SYSCALL_SYSINFO, (void *)(sys_sysinfo));
    syscall_table_set(SYSCALL_SYSLOG, (void *)(sys_syslog));
    syscall_table_set(SYSCALL_SETHOSTNAME, (void *)(sys_sethostname));
    syscall_table_set(SYSCALL_GETHOSTNAME, (void *)(sys_gethostname));
    syscall_table_set(SYSCALL_SETDOMAINNAME, (void *)(sys_setdomainname));
    syscall_table_set(SYSCALL_GETDOMAINNAME, (void *)(sys_getdomainname));
    syscall_table_set(SYSCALL_TIMES, (void *)(sys_times));
    syscall_table_set(SYSCALL_MEMBARRIER, (void *)(sys_membarrier));
    syscall_table_set(SYSCALL_GETRLIMIT, (void *)(sys_getrlimit));
    syscall_table_set(SYSCALL_SETRLIMIT, (void *)(sys_setrlimit));
    syscall_table_set(SYSCALL_GETRUSAGE, (void *)(sys_getrusage));
    syscall_table_set(SYSCALL_PRLIMIT64, (void *)(sys_prlimit64));
    syscall_table_set(SYSCALL_GETRANDOM, (void *)(sys_getrandom));
    syscall_table_set(SYSCALL_PRCTL, (void *)(sys_prctl));
    syscall_table_set(SYSCALL_ARCH_PRCTL, (void *)(sys_arch_prctl));
    syscall_table_set(SYSCALL_SET_TID_ADDRESS, (void *)(sys_set_tid_address));
    syscall_table_set(SYSCALL_SETITIMER, (void *)(sys_setitimer));
    syscall_table_set(SYSCALL_GETITIMER, (void *)(sys_getitimer));
    syscall_table_set(SYSCALL_ALARM, (void *)(sys_alarm));
    syscall_table_set(SYSCALL_NANOSLEEP, (void *)(sys_nanosleep));
    syscall_table_set(SYSCALL_CHMOD, (void *)(sys_chmod));
    syscall_table_set(SYSCALL_CHOWN, (void *)(sys_chown));
    syscall_table_set(SYSCALL_LCHOWN, (void *)(sys_lchown));
    syscall_table_set(SYSCALL_SETENV, (void *)(sys_setenv));
    syscall_table_set(SYSCALL_GETENV, (void *)(sys_getenv));
    syscall_table_set(SYSCALL_UNSETENV, (void *)(sys_unsetenv));
    syscall_table_set(SYSCALL_CLEARENV, (void *)(sys_clearenv));
    syscall_table_set(SYSCALL_POSIX_OPENPT, (void *)(sys_posix_openpt));
    syscall_table_set(SYSCALL_GRANTPT, (void *)(sys_grantpt));
    syscall_table_set(SYSCALL_UNLOCKPT, (void *)(sys_unlockpt));
    syscall_table_set(SYSCALL_PTSNAME, (void *)(sys_ptsname));
    syscall_table_set(SYSCALL_GETPRIORITY, (void *)(sys_getpriority));
    syscall_table_set(SYSCALL_SETPRIORITY, (void *)(sys_setpriority));
    syscall_table_set(SYSCALL_SCHED_SETPARAM, (void *)(sys_sched_setparam));
    syscall_table_set(SYSCALL_SCHED_GETPARAM, (void *)(sys_sched_getparam));
    syscall_table_set(SYSCALL_SCHED_SETSCHEDULER, (void *)(sys_sched_setscheduler));
    syscall_table_set(SYSCALL_SCHED_GETSCHEDULER, (void *)(sys_sched_getscheduler));
    syscall_table_set(SYSCALL_SCHED_GET_PRIORITY_MAX, (void *)(sys_sched_priority_max));
    syscall_table_set(SYSCALL_SCHED_GET_PRIORITY_MIN, (void *)(sys_sched_priority_min));
    syscall_table_set(SYSCALL_SCHED_RR_GET_INTERVAL, (void *)(sys_sched_rr_interval));
    syscall_table_set(SYSCALL_SCHED_SETAFFINITY, (void *)(sys_sched_setaffinity));
    syscall_table_set(SYSCALL_SCHED_GETAFFINITY, (void *)(sys_sched_getaffinity));
    syscall_table_set(SYSCALL_LKE_LOAD, (void *)(sys_lke_load));
    syscall_table_set(SYSCALL_LKE_UNLOAD, (void *)(sys_lke_unload));
    syscall_table_set(SYSCALL_LKE_LIST, (void *)(sys_lke_list));
    syscall_table_set(SYSCALL_TIMER_CREATE, (void *)(sys_timer_create));
    syscall_table_set(SYSCALL_TIMER_SETTIME, (void *)(sys_timer_settime));
    syscall_table_set(SYSCALL_TIMER_DELETE, (void *)(sys_timer_delete));
    syscall_table_set(SYSCALL_TIMER_GETTIME, (void *)(sys_timer_gettime));
    syscall_table_set(SYSCALL_TIMER_GETOVERRUN, (void *)(sys_timer_getoverrun));
    syscall_table_set(SYSCALL_SPLICE, (void *)(sys_splice));
    syscall_table_set(SYSCALL_VMSPLICE, (void *)(sys_splice));
    syscall_table_set(SYSCALL_TEE, (void *)(sys_tee));
    syscall_table_set(SYSCALL_MQ_OPEN, (void *)(sys_mq_open));
    syscall_table_set(SYSCALL_MQ_SEND, (void *)(sys_mq_send));
    syscall_table_set(SYSCALL_MQ_RECEIVE, (void *)(sys_mq_receive));
    syscall_table_set(SYSCALL_SEMGET, (void *)(sys_semget));
    syscall_table_set(SYSCALL_MSGGET, (void *)(sys_msgget));
    syscall_table_set(SYSCALL_UNSHARE, (void *)(sys_unshare));
    syscall_table_set(SYSCALL_SECCOMP, (void *)(sys_seccomp));
    syscall_table_set(SYSCALL_VRING_ADD_PATH_RULE, (void *)(sys_vring_path_rule));
    syscall_table_set(SYSCALL_KTLS_TX, (void *)(sys_ktls_tx));
}

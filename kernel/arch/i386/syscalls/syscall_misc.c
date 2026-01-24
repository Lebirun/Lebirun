#include "syscall_defs.h"
#include <kernel/about.h>

extern task_t *current_task;
extern void *syscall_table[];

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

extern volatile uint32_t tick_count;
extern uint32_t pit_freq;
extern uint32_t pfa_count_free(void);

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
    uint32_t total_kb;
    uint32_t free_pages;
    
    if (!info) return -EFAULT;
    
    memset(info, 0, sizeof(struct sysinfo));
    
    total_kb = pfa_get_total_ram_kb();
    free_pages = pfa_count_free();
    
    info->uptime = tick_count / pit_freq;
    info->loads[0] = 0;
    info->loads[1] = 0;
    info->loads[2] = 0;
    info->totalram = (unsigned long)total_kb * 1024;
    info->freeram = free_pages * 4096;
    info->sharedram = 0;
    info->bufferram = 0;
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = 1;
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

static uint32_t random_state = 0x12345678;

static uint32_t simple_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static int sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if (!buf) return -EFAULT;
    if (buflen == 0) return 0;
    
    random_state ^= tick_count;
    
    uint8_t *p = (uint8_t *)buf;
    size_t i;
    
    for (i = 0; i < buflen; i++) {
        p[i] = simple_random() & 0xFF;
    }
    
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

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_WAIT_PRIVATE   (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE   (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

static int sys_futex(int *uaddr, int op, int val, const struct kernel_timespec *timeout, int *uaddr2, int val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    int cmd = op & ~FUTEX_PRIVATE_FLAG;
    
    if (!uaddr) return -EFAULT;
    
    switch (cmd) {
        case FUTEX_WAIT:
            if (*uaddr != val) return -EAGAIN;
            return 0;
            
        case FUTEX_WAKE:
            return 0;
            
        default:
            return -ENOSYS;
    }
}

static int sys_set_robust_list(void *head, size_t len) {
    (void)head; (void)len;
    return 0;
}

static int sys_get_robust_list(int pid, void **head_ptr, size_t *len_ptr) {
    (void)pid;
    if (head_ptr) *head_ptr = NULL;
    if (len_ptr) *len_ptr = 0;
    return 0;
}

static int sys_epoll_create(int size) {
    (void)size;
    return -ENOSYS;
}

static int sys_epoll_create1(int flags) {
    (void)flags;
    return -ENOSYS;
}

static int sys_epoll_ctl(int epfd, int op, int fd, void *event) {
    (void)epfd; (void)op; (void)fd; (void)event;
    return -ENOSYS;
}

static int sys_epoll_wait(int epfd, void *events, int maxevents, int timeout) {
    (void)epfd; (void)events; (void)maxevents; (void)timeout;
    return -ENOSYS;
}

static int sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout, const void *sigmask) {
    (void)epfd; (void)events; (void)maxevents; (void)timeout; (void)sigmask;
    return -ENOSYS;
}

static int sys_eventfd(unsigned int initval, int flags) {
    (void)initval; (void)flags;
    return -ENOSYS;
}

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval_k {
    struct kernel_timeval it_interval;
    struct kernel_timeval it_value;
};

static struct itimerval_k task_itimers[256][3];
static uint32_t alarm_ticks[256];

static int sys_setitimer(int which, const struct itimerval_k *new_value, struct itimerval_k *old_value) {
    if (which < 0 || which > 2) return -EINVAL;
    if (!current_task) return -ESRCH;
    
    uint32_t idx = ((uint32_t)current_task->pid) & 255u;
    
    if (old_value) {
        memcpy(old_value, &task_itimers[idx][which], sizeof(struct itimerval_k));
    }
    
    if (new_value) {
        memcpy(&task_itimers[idx][which], new_value, sizeof(struct itimerval_k));
    }
    
    return 0;
}

static int sys_getitimer(int which, struct itimerval_k *curr_value) {
    if (which < 0 || which > 2) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (!curr_value) return -EFAULT;
    
    uint32_t idx = ((uint32_t)current_task->pid) & 255u;
    memcpy(curr_value, &task_itimers[idx][which], sizeof(struct itimerval_k));
    
    return 0;
}

static int sys_alarm(int seconds, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (!current_task) return 0;
    
    uint32_t idx = ((uint32_t)current_task->pid) & 255u;
    uint32_t old_alarm = alarm_ticks[idx];
    
    if (seconds > 0) {
        alarm_ticks[idx] = tick_count + (seconds * pit_freq);
    } else {
        alarm_ticks[idx] = 0;
    }
    
    if (old_alarm > tick_count) {
        return (old_alarm - tick_count) / pit_freq;
    }
    return 0;
}

static int sys_nanosleep(const struct kernel_timespec *req, struct kernel_timespec *rem) {
    if (!req) return -EFAULT;
    
    uint32_t ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    if (ms > 0) {
        sleep_ms(ms);
    }
    
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    
    return 0;
}

static int sys_chmod(const char *pathname, int mode) {
    if (!pathname) return -EFAULT;
    uint32_t addr = (uint32_t)pathname;
    if (addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(pathname);
    if (!node) return -ENOENT;
    
    if (node->chmod) {
        return node->chmod(node, mode & 07777);
    }
    node->mask = mode & 07777;
    return 0;
}

static int sys_chown(const char *pathname, int owner, int group) {
    if (!pathname) return -EFAULT;
    uint32_t addr = (uint32_t)pathname;
    if (addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(pathname);
    if (!node) return -ENOENT;
    
    if (node->chown) {
        return node->chown(node, owner, group);
    }
    if (owner != -1) node->uid = owner;
    if (group != -1) node->gid = group;
    return 0;
}

static int sys_lchown(const char *pathname, int owner, int group) {
    return sys_chown(pathname, owner, group);
}

#define MAX_ENV_VARS 128
#define MAX_ENV_SIZE 256

static char env_names[MAX_ENV_VARS][64];
static char env_values[MAX_ENV_VARS][MAX_ENV_SIZE];
static int env_count = 0;
static char *environ_ptrs[MAX_ENV_VARS + 1];

static void init_default_environ(void) {
    if (env_count > 0) return;
    
    copy_string(env_names[0], "PATH", 64);
    copy_string(env_values[0], "/bin:/usr/bin:/sbin:/usr/sbin", MAX_ENV_SIZE);
    copy_string(env_names[1], "HOME", 64);
    copy_string(env_values[1], "/root", MAX_ENV_SIZE);
    copy_string(env_names[2], "SHELL", 64);
    copy_string(env_values[2], "/bin/sh", MAX_ENV_SIZE);
    copy_string(env_names[3], "USER", 64);
    copy_string(env_values[3], "root", MAX_ENV_SIZE);
    copy_string(env_names[4], "TERM", 64);
    copy_string(env_values[4], "linux", MAX_ENV_SIZE);
    copy_string(env_names[5], "PWD", 64);
    copy_string(env_values[5], "/", MAX_ENV_SIZE);
    env_count = 6;
}

static int find_env(const char *name) {
    for (int i = 0; i < env_count; i++) {
        int j = 0;
        while (name[j] && env_names[i][j] && name[j] == env_names[i][j]) j++;
        if (name[j] == '\0' && env_names[i][j] == '\0') return i;
    }
    return -1;
}

static int sys_getenv(const char *name, char *buf, int bufsize) {
    if (!name || !buf) return -EFAULT;
    uint32_t name_addr = (uint32_t)name;
    uint32_t buf_addr = (uint32_t)buf;
    if (name_addr >= 0xC0000000 || name_addr < 0x1000) return -EFAULT;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    
    init_default_environ();
    
    int idx = find_env(name);
    if (idx < 0) return -ENOENT;
    
    int len = 0;
    while (env_values[idx][len]) len++;
    if (len + 1 > bufsize) return -ERANGE;
    
    for (int i = 0; i <= len; i++) {
        buf[i] = env_values[idx][i];
    }
    return len;
}

static int sys_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value) return -EFAULT;
    uint32_t name_addr = (uint32_t)name;
    uint32_t value_addr = (uint32_t)value;
    if (name_addr >= 0xC0000000 || name_addr < 0x1000) return -EFAULT;
    if (value_addr >= 0xC0000000 || value_addr < 0x1000) return -EFAULT;
    
    init_default_environ();
    
    int idx = find_env(name);
    if (idx >= 0) {
        if (!overwrite) return 0;
        int i = 0;
        while (value[i] && i < MAX_ENV_SIZE - 1) {
            env_values[idx][i] = value[i];
            i++;
        }
        env_values[idx][i] = '\0';
        return 0;
    }
    
    if (env_count >= MAX_ENV_VARS) return -ENOMEM;
    
    int i = 0;
    while (name[i] && i < 63) {
        env_names[env_count][i] = name[i];
        i++;
    }
    env_names[env_count][i] = '\0';
    
    i = 0;
    while (value[i] && i < MAX_ENV_SIZE - 1) {
        env_values[env_count][i] = value[i];
        i++;
    }
    env_values[env_count][i] = '\0';
    
    env_count++;
    return 0;
}

static int sys_unsetenv(const char *name) {
    if (!name) return -EFAULT;
    uint32_t addr = (uint32_t)name;
    if (addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int idx = find_env(name);
    if (idx < 0) return 0;
    
    for (int i = idx; i < env_count - 1; i++) {
        for (int j = 0; j < 64; j++) env_names[i][j] = env_names[i+1][j];
        for (int j = 0; j < MAX_ENV_SIZE; j++) env_values[i][j] = env_values[i+1][j];
    }
    env_count--;
    return 0;
}

static int sys_clearenv(void) {
    env_count = 0;
    return 0;
}

char **get_environ(void) {
    init_default_environ();
    for (int i = 0; i < env_count; i++) {
        environ_ptrs[i] = env_values[i];
    }
    environ_ptrs[env_count] = NULL;
    return environ_ptrs;
}

void syscalls_misc_init(void) {
    init_default_environ();
    memset(task_itimers, 0, sizeof(task_itimers));
    memset(alarm_ticks, 0, sizeof(alarm_ticks));
    
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
    syscall_table[SYSCALL_FUTEX] = sys_futex;
    syscall_table[SYSCALL_SET_ROBUST_LIST] = sys_set_robust_list;
    syscall_table[SYSCALL_GET_ROBUST_LIST] = sys_get_robust_list;
    syscall_table[SYSCALL_EPOLL_CREATE] = sys_epoll_create;
    syscall_table[SYSCALL_EPOLL_CREATE1] = sys_epoll_create1;
    syscall_table[SYSCALL_EPOLL_CTL] = sys_epoll_ctl;
    syscall_table[SYSCALL_EPOLL_WAIT] = sys_epoll_wait;
    syscall_table[SYSCALL_EPOLL_PWAIT] = sys_epoll_pwait;
    syscall_table[SYSCALL_EVENTFD] = sys_eventfd;
    syscall_table[SYSCALL_EVENTFD2] = sys_eventfd;
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
}

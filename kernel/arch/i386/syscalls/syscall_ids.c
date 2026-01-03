#include "syscall_defs.h"
#include <kernel/creds.h>

typedef struct {
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t fsuid;
    uint32_t fsgid;
    uint32_t groups[32];
    int ngroups;
    uint32_t umask_val;
    pid_t pgid;
    pid_t sid;
} task_creds_t;

static task_creds_t task_creds[256];

static inline uint32_t task_creds_index(pid_t pid) {
    return ((uint32_t)pid) & 255u;
}

static task_creds_t *get_task_creds(void) {
    if (!current_task) return NULL;
    uint32_t idx = task_creds_index(current_task->pid);
    return &task_creds[idx];
}

static int sys_getuid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    task_creds_t *creds = get_task_creds();
    return creds ? (int)creds->uid : 0;
}

static int sys_getgid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    task_creds_t *creds = get_task_creds();
    return creds ? (int)creds->gid : 0;
}

static int sys_geteuid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    task_creds_t *creds = get_task_creds();
    return creds ? (int)creds->euid : 0;
}

static int sys_getegid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    task_creds_t *creds = get_task_creds();
    return creds ? (int)creds->egid : 0;
}

static int sys_setuid(int uid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        creds->uid = (uint32_t)uid;
        creds->euid = (uint32_t)uid;
        creds->suid = (uint32_t)uid;
    } else if ((uint32_t)uid == creds->uid || (uint32_t)uid == creds->suid) {
        creds->euid = (uint32_t)uid;
    } else {
        return -EPERM;
    }
    return 0;
}

static int sys_setgid(int gid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        creds->gid = (uint32_t)gid;
        creds->egid = (uint32_t)gid;
        creds->sgid = (uint32_t)gid;
    } else if ((uint32_t)gid == creds->gid || (uint32_t)gid == creds->sgid) {
        creds->egid = (uint32_t)gid;
    } else {
        return -EPERM;
    }
    return 0;
}

static int sys_seteuid(int euid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0 || (uint32_t)euid == creds->uid || (uint32_t)euid == creds->suid) {
        creds->euid = (uint32_t)euid;
        return 0;
    }
    return -EPERM;
}

static int sys_setegid(int egid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0 || (uint32_t)egid == creds->gid || (uint32_t)egid == creds->sgid) {
        creds->egid = (uint32_t)egid;
        return 0;
    }
    return -EPERM;
}

static int sys_setreuid(int ruid, const char *euid_ptr, int unused) {
    (void)unused;
    int euid = (int)(uintptr_t)euid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        if (ruid != -1) creds->uid = (uint32_t)ruid;
        if (euid != -1) creds->euid = (uint32_t)euid;
        return 0;
    }
    
    if (ruid != -1 && (uint32_t)ruid != creds->uid && (uint32_t)ruid != creds->euid) {
        return -EPERM;
    }
    if (euid != -1 && (uint32_t)euid != creds->uid && (uint32_t)euid != creds->euid && (uint32_t)euid != creds->suid) {
        return -EPERM;
    }
    
    if (ruid != -1) creds->uid = (uint32_t)ruid;
    if (euid != -1) creds->euid = (uint32_t)euid;
    
    return 0;
}

static int sys_setregid(int rgid, const char *egid_ptr, int unused) {
    (void)unused;
    int egid = (int)(uintptr_t)egid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        if (rgid != -1) creds->gid = (uint32_t)rgid;
        if (egid != -1) creds->egid = (uint32_t)egid;
        return 0;
    }
    
    if (rgid != -1) creds->gid = (uint32_t)rgid;
    if (egid != -1) creds->egid = (uint32_t)egid;
    
    return 0;
}

static int sys_setresuid(int ruid, const char *euid_ptr, int suid) {
    int euid = (int)(uintptr_t)euid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) {
        if (ruid != -1 && (uint32_t)ruid != creds->uid && (uint32_t)ruid != creds->euid && (uint32_t)ruid != creds->suid) return -EPERM;
        if (euid != -1 && (uint32_t)euid != creds->uid && (uint32_t)euid != creds->euid && (uint32_t)euid != creds->suid) return -EPERM;
        if (suid != -1 && (uint32_t)suid != creds->uid && (uint32_t)suid != creds->euid && (uint32_t)suid != creds->suid) return -EPERM;
    }
    
    if (ruid != -1) creds->uid = (uint32_t)ruid;
    if (euid != -1) creds->euid = (uint32_t)euid;
    if (suid != -1) creds->suid = (uint32_t)suid;
    
    return 0;
}

static int sys_setresgid(int rgid, const char *egid_ptr, int sgid) {
    int egid = (int)(uintptr_t)egid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) {
        if (rgid != -1 && (uint32_t)rgid != creds->gid && (uint32_t)rgid != creds->egid && (uint32_t)rgid != creds->sgid) return -EPERM;
        if (egid != -1 && (uint32_t)egid != creds->gid && (uint32_t)egid != creds->egid && (uint32_t)egid != creds->sgid) return -EPERM;
        if (sgid != -1 && (uint32_t)sgid != creds->gid && (uint32_t)sgid != creds->egid && (uint32_t)sgid != creds->sgid) return -EPERM;
    }
    
    if (rgid != -1) creds->gid = (uint32_t)rgid;
    if (egid != -1) creds->egid = (uint32_t)egid;
    if (sgid != -1) creds->sgid = (uint32_t)sgid;
    
    return 0;
}

static int sys_getresuid(int ruid_ptr, const char *euid_ptr, int suid_ptr) {
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint32_t r_addr = (uint32_t)ruid_ptr;
    uint32_t e_addr = (uint32_t)(uintptr_t)euid_ptr;
    uint32_t s_addr = (uint32_t)suid_ptr;
    
    if (r_addr && r_addr < 0xC0000000 && r_addr >= 0x1000) *(uint32_t *)r_addr = creds->uid;
    if (e_addr && e_addr < 0xC0000000 && e_addr >= 0x1000) *(uint32_t *)e_addr = creds->euid;
    if (s_addr && s_addr < 0xC0000000 && s_addr >= 0x1000) *(uint32_t *)s_addr = creds->suid;
    
    return 0;
}

static int sys_getresgid(int rgid_ptr, const char *egid_ptr, int sgid_ptr) {
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint32_t r_addr = (uint32_t)rgid_ptr;
    uint32_t e_addr = (uint32_t)(uintptr_t)egid_ptr;
    uint32_t s_addr = (uint32_t)sgid_ptr;
    
    if (r_addr && r_addr < 0xC0000000 && r_addr >= 0x1000) *(uint32_t *)r_addr = creds->gid;
    if (e_addr && e_addr < 0xC0000000 && e_addr >= 0x1000) *(uint32_t *)e_addr = creds->egid;
    if (s_addr && s_addr < 0xC0000000 && s_addr >= 0x1000) *(uint32_t *)s_addr = creds->sgid;
    
    return 0;
}

static int sys_setfsuid(int fsuid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint32_t old = creds->fsuid;
    
    if (creds->euid == 0 || (uint32_t)fsuid == creds->uid || (uint32_t)fsuid == creds->euid || (uint32_t)fsuid == creds->suid || (uint32_t)fsuid == creds->fsuid) {
        creds->fsuid = (uint32_t)fsuid;
    }
    
    return (int)old;
}

static int sys_setfsgid(int fsgid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint32_t old = creds->fsgid;
    
    if (creds->euid == 0 || (uint32_t)fsgid == creds->gid || (uint32_t)fsgid == creds->egid || (uint32_t)fsgid == creds->sgid || (uint32_t)fsgid == creds->fsgid) {
        creds->fsgid = (uint32_t)fsgid;
    }
    
    return (int)old;
}

static int sys_getgroups(int size, const char *list_ptr, int unused) {
    (void)unused;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (size == 0) return creds->ngroups;
    
    uint32_t addr = (uint32_t)(uintptr_t)list_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int count = (size < creds->ngroups) ? size : creds->ngroups;
    memcpy((void *)addr, creds->groups, count * sizeof(uint32_t));
    
    return count;
}

static int sys_setgroups(int size, const char *list_ptr, int unused) {
    (void)unused;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) return -EPERM;
    
    if (size < 0 || size > 32) return -EINVAL;
    
    if (size > 0) {
        uint32_t addr = (uint32_t)(uintptr_t)list_ptr;
        if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
        memcpy(creds->groups, (void *)addr, size * sizeof(uint32_t));
    }
    
    creds->ngroups = size;
    return 0;
}

static int sys_getpgid(int pid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    
    task_t *t = task_find((pid_t)pid);
    if (!t) return -ESRCH;
    
    uint32_t idx = task_creds_index(t->pid);
    return (int)task_creds[idx].pgid;
}

static int sys_setpgid(int pid, const char *pgid_ptr, int unused) {
    (void)unused;
    int pgid = (int)(uintptr_t)pgid_ptr;
    
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    if (pgid == 0) pgid = pid;
    
    task_t *t = task_find((pid_t)pid);
    if (!t) return -ESRCH;
    
    uint32_t idx = task_creds_index(t->pid);
    task_creds[idx].pgid = (pid_t)pgid;

    t->pgid = (pid_t)pgid;
    
    return 0;
}

static int sys_getpgrp(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return sys_getpgid(0, NULL, 0);
}

static int sys_setsid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    pid_t pid = current_task ? current_task->pid : 0;
    
    creds->sid = pid;
    creds->pgid = pid;

    if (current_task) {
        current_task->sid = pid;
        current_task->pgid = pid;
    }
    
    return (int)pid;
}

static int sys_getsid(int pid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    
    task_t *t = task_find((pid_t)pid);
    if (!t) return -ESRCH;
    
    uint32_t idx = task_creds_index(t->pid);
    return (int)task_creds[idx].sid;
}

static int sys_getppid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return 1;
}

static int sys_getpid_impl(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->pid : 0;
}

static int sys_gettid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->pid : 0;
}

void creds_init_task(pid_t pid) {
    uint32_t idx = task_creds_index(pid);
    memset(&task_creds[idx], 0, sizeof(task_creds_t));
    task_creds[idx].umask_val = 022;
    task_creds[idx].pgid = pid;
    task_creds[idx].sid = pid;

    task_t *t = task_find(pid);
    if (t) {
        t->pgid = pid;
        t->sid = pid;
    }
}

void creds_copy_task(pid_t parent_pid, pid_t child_pid) {
    uint32_t parent_idx = task_creds_index(parent_pid);
    uint32_t child_idx = task_creds_index(child_pid);
    memcpy(&task_creds[child_idx], &task_creds[parent_idx], sizeof(task_creds_t));
    
    task_t *child = task_find(child_pid);
    if (child) {
        child->pgid = task_creds[child_idx].pgid;
        child->sid = task_creds[child_idx].sid;
        child->ppid = parent_pid;
    }
}

pid_t creds_get_pgid(pid_t pid) {
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    if (pid == 0) return 0;
    task_t *t = task_find(pid);
    if (!t) return 0;
    return task_creds[task_creds_index(t->pid)].pgid;
}

pid_t creds_get_sid(pid_t pid) {
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    if (pid == 0) return 0;
    task_t *t = task_find(pid);
    if (!t) return 0;
    return task_creds[task_creds_index(t->pid)].sid;
}

void syscalls_ids_init(void) {
    memset(task_creds, 0, sizeof(task_creds));
    
    syscall_table[SYSCALL_GETUID] = sys_getuid;
    syscall_table[SYSCALL_GETGID] = sys_getgid;
    syscall_table[SYSCALL_GETEUID] = sys_geteuid;
    syscall_table[SYSCALL_GETEGID] = sys_getegid;
    syscall_table[SYSCALL_SETUID] = sys_setuid;
    syscall_table[SYSCALL_SETGID] = sys_setgid;
    syscall_table[SYSCALL_SETEUID] = sys_seteuid;
    syscall_table[SYSCALL_SETEGID] = sys_setegid;
    syscall_table[SYSCALL_SETREUID] = sys_setreuid;
    syscall_table[SYSCALL_SETREGID] = sys_setregid;
    syscall_table[SYSCALL_SETRESUID] = sys_setresuid;
    syscall_table[SYSCALL_SETRESGID] = sys_setresgid;
    syscall_table[SYSCALL_GETRESUID] = sys_getresuid;
    syscall_table[SYSCALL_GETRESGID] = sys_getresgid;
    syscall_table[SYSCALL_SETFSUID] = sys_setfsuid;
    syscall_table[SYSCALL_SETFSGID] = sys_setfsgid;
    syscall_table[SYSCALL_GETGROUPS] = sys_getgroups;
    syscall_table[SYSCALL_SETGROUPS] = sys_setgroups;
    syscall_table[SYSCALL_GETPGID] = sys_getpgid;
    syscall_table[SYSCALL_SETPGID] = sys_setpgid;
    syscall_table[SYSCALL_GETPGRP] = sys_getpgrp;
    syscall_table[SYSCALL_SETSID] = sys_setsid;
    syscall_table[SYSCALL_GETSID] = sys_getsid;
    syscall_table[SYSCALL_GETPPID] = sys_getppid;
    syscall_table[SYSCALL_GETPID2] = sys_getpid_impl;
    syscall_table[SYSCALL_GETTID] = sys_gettid;
}

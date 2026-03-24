#include "syscall_defs.h"
#include <kernel/creds.h>

typedef struct {
    uint64_t uid;
    uint64_t gid;
    uint64_t euid;
    uint64_t egid;
    uint64_t suid;
    uint64_t sgid;
    uint64_t fsuid;
    uint64_t fsgid;
    uint64_t groups[16];
    int ngroups;
    uint64_t umask_val;
    pid_t pgid;
    pid_t sid;
} task_creds_t;

static task_creds_t *get_task_creds(void) {
    task_creds_t *creds;

    if (!current_task) return NULL;
    creds = (task_creds_t *)current_task->creds_data;
    if (!creds) {
        creds = (task_creds_t *)kmalloc(sizeof(task_creds_t));
        if (!creds) return NULL;
        memset(creds, 0, sizeof(task_creds_t));
        creds->uid    = current_task->uid;
        creds->euid   = current_task->euid;
        creds->suid   = current_task->suid;
        creds->gid    = current_task->gid;
        creds->egid   = current_task->egid;
        creds->sgid   = current_task->sgid;
        creds->fsuid  = current_task->fsuid;
        creds->fsgid  = current_task->fsgid;
        creds->umask_val = 022;
        creds->pgid = current_task->pid;
        creds->sid = current_task->pid;
        current_task->creds_data = creds;
    }
    return creds;
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

static void sync_creds_to_task(task_creds_t *creds) {
    if (!current_task || !creds) return;
    current_task->uid   = creds->uid;
    current_task->euid  = creds->euid;
    current_task->suid  = creds->suid;
    current_task->gid   = creds->gid;
    current_task->egid  = creds->egid;
    current_task->sgid  = creds->sgid;
    current_task->fsuid = creds->fsuid;
    current_task->fsgid = creds->fsgid;
}

static int sys_setuid(int uid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        creds->uid = (uint64_t)uid;
        creds->euid = (uint64_t)uid;
        creds->suid = (uint64_t)uid;
    } else if ((uint64_t)uid == creds->uid || (uint64_t)uid == creds->suid) {
        creds->euid = (uint64_t)uid;
    } else {
        return -EPERM;
    }
    sync_creds_to_task(creds);
    return 0;
}

static int sys_setgid(int gid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        creds->gid = (uint64_t)gid;
        creds->egid = (uint64_t)gid;
        creds->sgid = (uint64_t)gid;
    } else if ((uint64_t)gid == creds->gid || (uint64_t)gid == creds->sgid) {
        creds->egid = (uint64_t)gid;
    } else {
        return -EPERM;
    }
    sync_creds_to_task(creds);
    return 0;
}

static int sys_seteuid(int euid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0 || (uint64_t)euid == creds->uid || (uint64_t)euid == creds->suid) {
        creds->euid = (uint64_t)euid;
        sync_creds_to_task(creds);
        return 0;
    }
    return -EPERM;
}

static int sys_setegid(int egid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0 || (uint64_t)egid == creds->gid || (uint64_t)egid == creds->sgid) {
        creds->egid = (uint64_t)egid;
        sync_creds_to_task(creds);
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
        if (ruid != -1) creds->uid = (uint64_t)ruid;
        if (euid != -1) creds->euid = (uint64_t)euid;
        sync_creds_to_task(creds);
        return 0;
    }
    
    if (ruid != -1 && (uint64_t)ruid != creds->uid && (uint64_t)ruid != creds->euid) {
        return -EPERM;
    }
    if (euid != -1 && (uint64_t)euid != creds->uid && (uint64_t)euid != creds->euid && (uint64_t)euid != creds->suid) {
        return -EPERM;
    }
    
    if (ruid != -1) creds->uid = (uint64_t)ruid;
    if (euid != -1) creds->euid = (uint64_t)euid;
    sync_creds_to_task(creds);
    return 0;
}

static int sys_setregid(int rgid, const char *egid_ptr, int unused) {
    (void)unused;
    int egid = (int)(uintptr_t)egid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid == 0) {
        if (rgid != -1) creds->gid = (uint64_t)rgid;
        if (egid != -1) creds->egid = (uint64_t)egid;
        sync_creds_to_task(creds);
        return 0;
    }
    
    if (rgid != -1) creds->gid = (uint64_t)rgid;
    if (egid != -1) creds->egid = (uint64_t)egid;
    sync_creds_to_task(creds);
    return 0;
}

static int sys_setresuid(int ruid, const char *euid_ptr, int suid) {
    int euid = (int)(uintptr_t)euid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) {
        if (ruid != -1 && (uint64_t)ruid != creds->uid && (uint64_t)ruid != creds->euid && (uint64_t)ruid != creds->suid) return -EPERM;
        if (euid != -1 && (uint64_t)euid != creds->uid && (uint64_t)euid != creds->euid && (uint64_t)euid != creds->suid) return -EPERM;
        if (suid != -1 && (uint64_t)suid != creds->uid && (uint64_t)suid != creds->euid && (uint64_t)suid != creds->suid) return -EPERM;
    }
    
    if (ruid != -1) creds->uid = (uint64_t)ruid;
    if (euid != -1) creds->euid = (uint64_t)euid;
    if (suid != -1) creds->suid = (uint64_t)suid;
    sync_creds_to_task(creds);
    return 0;
}

static int sys_setresgid(int rgid, const char *egid_ptr, int sgid) {
    int egid = (int)(uintptr_t)egid_ptr;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) {
        if (rgid != -1 && (uint64_t)rgid != creds->gid && (uint64_t)rgid != creds->egid && (uint64_t)rgid != creds->sgid) return -EPERM;
        if (egid != -1 && (uint64_t)egid != creds->gid && (uint64_t)egid != creds->egid && (uint64_t)egid != creds->sgid) return -EPERM;
        if (sgid != -1 && (uint64_t)sgid != creds->gid && (uint64_t)sgid != creds->egid && (uint64_t)sgid != creds->sgid) return -EPERM;
    }
    
    if (rgid != -1) creds->gid = (uint64_t)rgid;
    if (egid != -1) creds->egid = (uint64_t)egid;
    if (sgid != -1) creds->sgid = (uint64_t)sgid;
    sync_creds_to_task(creds);
    return 0;
}

static int sys_getresuid(int ruid_ptr, const char *euid_ptr, int suid_ptr) {
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint64_t r_addr = (uint64_t)ruid_ptr;
    uint64_t e_addr = (uint64_t)(uintptr_t)euid_ptr;
    uint64_t s_addr = (uint64_t)suid_ptr;
    
    if (r_addr && r_addr < KERNEL_VMA && r_addr >= 0x1000) *(uint64_t *)r_addr = creds->uid;
    if (e_addr && e_addr < KERNEL_VMA && e_addr >= 0x1000) *(uint64_t *)e_addr = creds->euid;
    if (s_addr && s_addr < KERNEL_VMA && s_addr >= 0x1000) *(uint64_t *)s_addr = creds->suid;
    
    return 0;
}

static int sys_getresgid(int rgid_ptr, const char *egid_ptr, int sgid_ptr) {
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint64_t r_addr = (uint64_t)rgid_ptr;
    uint64_t e_addr = (uint64_t)(uintptr_t)egid_ptr;
    uint64_t s_addr = (uint64_t)sgid_ptr;
    
    if (r_addr && r_addr < KERNEL_VMA && r_addr >= 0x1000) *(uint64_t *)r_addr = creds->gid;
    if (e_addr && e_addr < KERNEL_VMA && e_addr >= 0x1000) *(uint64_t *)e_addr = creds->egid;
    if (s_addr && s_addr < KERNEL_VMA && s_addr >= 0x1000) *(uint64_t *)s_addr = creds->sgid;
    
    return 0;
}

static int sys_setfsuid(int fsuid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint64_t old = creds->fsuid;
    
    if (creds->euid == 0 || (uint64_t)fsuid == creds->uid || (uint64_t)fsuid == creds->euid || (uint64_t)fsuid == creds->suid || (uint64_t)fsuid == creds->fsuid) {
        creds->fsuid = (uint64_t)fsuid;
        sync_creds_to_task(creds);
    }
    
    return (int)old;
}

static int sys_setfsgid(int fsgid, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    uint64_t old = creds->fsgid;
    
    if (creds->euid == 0 || (uint64_t)fsgid == creds->gid || (uint64_t)fsgid == creds->egid || (uint64_t)fsgid == creds->sgid || (uint64_t)fsgid == creds->fsgid) {
        creds->fsgid = (uint64_t)fsgid;
        sync_creds_to_task(creds);
    }
    
    return (int)old;
}

static int sys_getgroups(int size, const char *list_ptr, int unused) {
    (void)unused;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (size == 0) return creds->ngroups;
    
    uint64_t addr = (uint64_t)(uintptr_t)list_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int count = (size < creds->ngroups) ? size : creds->ngroups;
    memcpy((void *)addr, creds->groups, count * sizeof(uint64_t));
    
    return count;
}

static int sys_setgroups(int size, const char *list_ptr, int unused) {
    (void)unused;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) return -EPERM;
    
    if (size < 0 || size > 16) return -EINVAL;
    
    if (size > 0) {
        uint64_t addr = (uint64_t)(uintptr_t)list_ptr;
        if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
        memcpy(creds->groups, (void *)addr, size * sizeof(uint64_t));
    }
    
    creds->ngroups = size;
    return 0;
}

static int sys_getpgid(int pid, const char *unused1, int unused2) {
    task_t *t;
    task_creds_t *creds;

    (void)unused1; (void)unused2;
    
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    
    t = task_find((pid_t)pid);
    if (!t) return -ESRCH;
    
    creds = (task_creds_t *)t->creds_data;
    if (!creds) return (int)t->pgid;
    return (int)creds->pgid;
}

static int sys_setpgid(int pid, const char *pgid_ptr, int unused) {
    int pgid;
    task_t *t;
    task_creds_t *creds;

    (void)unused;
    pgid = (int)(uintptr_t)pgid_ptr;
    
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    if (pgid == 0) pgid = pid;
    
    t = task_find((pid_t)pid);
    if (!t) return -ESRCH;
    
    creds = (task_creds_t *)t->creds_data;
    if (creds) creds->pgid = (pid_t)pgid;
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
    task_t *t;
    task_creds_t *creds;

    (void)unused1; (void)unused2;
    
    if (pid == 0) pid = current_task ? current_task->pid : 0;
    
    t = task_find((pid_t)pid);
    if (!t) return -ESRCH;
    
    creds = (task_creds_t *)t->creds_data;
    if (!creds) return (int)t->sid;
    return (int)creds->sid;
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

void creds_init_task(task_t *task) {
    task_creds_t *creds;

    if (!task) return;
    creds = (task_creds_t *)task->creds_data;
    if (!creds) {
        creds = (task_creds_t *)kmalloc(sizeof(task_creds_t));
        if (!creds) return;
        task->creds_data = creds;
    }
    memset(creds, 0, sizeof(task_creds_t));
    creds->umask_val = 022;
    creds->pgid = task->pid;
    creds->sid = task->pid;
    task->pgid = task->pid;
    task->sid = task->pid;
}

void creds_copy_task(task_t *parent, task_t *child) {
    task_creds_t *pcreds;
    task_creds_t *ccreds;

    if (!parent || !child) return;
    pcreds = (task_creds_t *)parent->creds_data;
    ccreds = (task_creds_t *)child->creds_data;
    if (!ccreds) {
        ccreds = (task_creds_t *)kmalloc(sizeof(task_creds_t));
        if (!ccreds) return;
        child->creds_data = ccreds;
    }
    if (pcreds) {
        memcpy(ccreds, pcreds, sizeof(task_creds_t));
    } else {
        memset(ccreds, 0, sizeof(task_creds_t));
        ccreds->uid   = parent->uid;
        ccreds->euid  = parent->euid;
        ccreds->suid  = parent->suid;
        ccreds->gid   = parent->gid;
        ccreds->egid  = parent->egid;
        ccreds->sgid  = parent->sgid;
        ccreds->fsuid = parent->fsuid;
        ccreds->fsgid = parent->fsgid;
        ccreds->umask_val = 022;
        ccreds->pgid = child->pid;
        ccreds->sid = child->pid;
    }
    child->uid   = ccreds->uid;
    child->euid  = ccreds->euid;
    child->suid  = ccreds->suid;
    child->gid   = ccreds->gid;
    child->egid  = ccreds->egid;
    child->sgid  = ccreds->sgid;
    child->fsuid = ccreds->fsuid;
    child->fsgid = ccreds->fsgid;
    child->pgid = ccreds->pgid;
    child->sid = ccreds->sid;
    child->ppid = parent->pid;
}

pid_t creds_get_pgid(pid_t pid) {
    task_t *t;
    task_creds_t *creds;

    if (pid == 0) pid = current_task ? current_task->pid : 0;
    if (pid == 0) return 0;
    t = task_find(pid);
    if (!t) return 0;
    creds = (task_creds_t *)t->creds_data;
    if (!creds) return t->pgid;
    return creds->pgid;
}

pid_t creds_get_sid(pid_t pid) {
    task_t *t;
    task_creds_t *creds;

    if (pid == 0) pid = current_task ? current_task->pid : 0;
    if (pid == 0) return 0;
    t = task_find(pid);
    if (!t) return 0;
    creds = (task_creds_t *)t->creds_data;
    if (!creds) return t->sid;
    return creds->sid;
}

void syscalls_ids_init(void) {
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

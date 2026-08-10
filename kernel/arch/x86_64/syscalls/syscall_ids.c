#include "syscall_defs.h"
#include <lebirun/creds.h>

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
    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_inheritable;
    uint64_t *syscall_mask;
    int no_new_privs;
    int syscall_filter_mode;
    int dumpable;
} task_creds_t;

#define CAP_FULL_SET 0x0000003FFFFFFFFFULL
#define CAP_VERSION_3 0x20080522u

typedef struct {
    uint32_t version;
    int pid;
} cap_header_t;

typedef struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} cap_data_t;

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
        if (current_task->groups && current_task->ngroups > 0)
            memcpy(creds->groups, current_task->groups, current_task->ngroups * sizeof(uint64_t));
        creds->ngroups = current_task->ngroups;
        creds->umask_val = 022;
        creds->pgid = current_task->pgid ? current_task->pgid : current_task->pid;
        creds->sid = current_task->sid ? current_task->sid : current_task->pid;
        if (current_task->euid == 0) {
            creds->cap_effective = CAP_FULL_SET;
            creds->cap_permitted = CAP_FULL_SET;
        }
        creds->dumpable = 1;
        current_task->creds_data = creds;
    }
    return creds;
}

static int sys_getuid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->uid : 0;
}

static int sys_getgid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->gid : 0;
}

static int sys_geteuid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->euid : 0;
}

static int sys_getegid(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->egid : 0;
}

static void sync_creds_to_task(task_creds_t *creds) {
    uint64_t old_euid;

    if (!current_task || !creds) return;
    old_euid = current_task->euid;
    if (old_euid == 0 && creds->euid != 0)
        creds->cap_effective = 0;
    else if (old_euid != 0 && creds->euid == 0)
        creds->cap_effective = creds->cap_permitted;
    if (creds->uid != 0 && creds->euid != 0 && creds->suid != 0) {
        creds->cap_effective = 0;
        creds->cap_permitted = 0;
    }
    current_task->uid   = creds->uid;
    current_task->euid  = creds->euid;
    current_task->suid  = creds->suid;
    current_task->gid   = creds->gid;
    current_task->egid  = creds->egid;
    current_task->sgid  = creds->sgid;
    current_task->fsuid = creds->fsuid;
    current_task->fsgid = creds->fsgid;
    current_task->groups = creds->groups;
    current_task->ngroups = creds->ngroups;
    current_task->pgid = creds->pgid;
    current_task->sid = creds->sid;
}

void creds_apply_exec_ids(struct task *task, uint64_t euid, uint64_t egid) {
    task_creds_t *creds;

    if (!task) return;
    creds = (task_creds_t *)task->creds_data;
    if (creds && creds->no_new_privs) {
        euid = task->euid;
        egid = task->egid;
    }
    task->euid = euid;
    task->suid = euid;
    task->fsuid = euid;
    task->egid = egid;
    task->sgid = egid;
    task->fsgid = egid;
    if (!creds) return;
    creds->euid = euid;
    creds->suid = euid;
    creds->fsuid = euid;
    creds->egid = egid;
    creds->sgid = egid;
    creds->fsgid = egid;
    if (euid == 0) {
        creds->cap_effective = CAP_FULL_SET;
        creds->cap_permitted = CAP_FULL_SET;
    } else {
        creds->cap_effective = 0;
        if (task->uid != 0 && task->suid != 0)
            creds->cap_permitted = 0;
    }
}

static int sys_setuid(int uid, const char *unused1, int unused2) {
    task_creds_t *creds;

    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    creds = (task_creds_t *)current_task->creds_data;
    if (!creds) {
        if (current_task->euid == 0) {
            current_task->uid = (uint64_t)uid;
            current_task->euid = (uint64_t)uid;
            current_task->suid = (uint64_t)uid;
            return 0;
        }
        if ((uint64_t)uid == current_task->uid || (uint64_t)uid == current_task->suid) {
            current_task->euid = (uint64_t)uid;
            return 0;
        }
        return -EPERM;
    }
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
    task_creds_t *creds;

    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    creds = (task_creds_t *)current_task->creds_data;
    if (!creds) {
        if (current_task->euid == 0) {
            current_task->gid = (uint64_t)gid;
            current_task->egid = (uint64_t)gid;
            current_task->sgid = (uint64_t)gid;
            return 0;
        }
        if ((uint64_t)gid == current_task->gid || (uint64_t)gid == current_task->sgid) {
            current_task->egid = (uint64_t)gid;
            return 0;
        }
        return -EPERM;
    }
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
    task_creds_t *creds;

    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    creds = (task_creds_t *)current_task->creds_data;
    if (!creds) {
        if (current_task->euid == 0 || (uint64_t)euid == current_task->uid || (uint64_t)euid == current_task->suid) {
            current_task->euid = (uint64_t)euid;
            return 0;
        }
        return -EPERM;
    }
    if (creds->euid == 0 || (uint64_t)euid == creds->uid || (uint64_t)euid == creds->suid) {
        creds->euid = (uint64_t)euid;
        sync_creds_to_task(creds);
        return 0;
    }
    return -EPERM;
}

static int sys_setegid(int egid, const char *unused1, int unused2) {
    task_creds_t *creds;

    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    creds = (task_creds_t *)current_task->creds_data;
    if (!creds) {
        if (current_task->euid == 0 || (uint64_t)egid == current_task->gid || (uint64_t)egid == current_task->sgid) {
            current_task->egid = (uint64_t)egid;
            return 0;
        }
        return -EPERM;
    }
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
    uint64_t r_addr = (uint64_t)ruid_ptr;
    uint64_t e_addr = (uint64_t)(uintptr_t)euid_ptr;
    uint64_t s_addr = (uint64_t)suid_ptr;

    if (!current_task) return -ESRCH;
    if (r_addr && r_addr < KERNEL_VMA && r_addr >= 0x1000) *(uint64_t *)r_addr = current_task->uid;
    if (e_addr && e_addr < KERNEL_VMA && e_addr >= 0x1000) *(uint64_t *)e_addr = current_task->euid;
    if (s_addr && s_addr < KERNEL_VMA && s_addr >= 0x1000) *(uint64_t *)s_addr = current_task->suid;
    
    return 0;
}

static int sys_getresgid(int rgid_ptr, const char *egid_ptr, int sgid_ptr) {
    uint64_t r_addr = (uint64_t)rgid_ptr;
    uint64_t e_addr = (uint64_t)(uintptr_t)egid_ptr;
    uint64_t s_addr = (uint64_t)sgid_ptr;

    if (!current_task) return -ESRCH;
    if (r_addr && r_addr < KERNEL_VMA && r_addr >= 0x1000) *(uint64_t *)r_addr = current_task->gid;
    if (e_addr && e_addr < KERNEL_VMA && e_addr >= 0x1000) *(uint64_t *)e_addr = current_task->egid;
    if (s_addr && s_addr < KERNEL_VMA && s_addr >= 0x1000) *(uint64_t *)s_addr = current_task->sgid;
    
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
    int count;
    uint64_t addr = (uint64_t)(uintptr_t)list_ptr;

    (void)unused;
    if (!current_task) return -ESRCH;
    if (size == 0) return current_task->ngroups;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    count = (size < current_task->ngroups) ? size : current_task->ngroups;
    memcpy((void *)addr, current_task->groups, count * sizeof(uint64_t));
    
    return count;
}

static int sys_setgroups(int size, const char *list_ptr, int unused) {
    uint64_t addr;
    (void)unused;
    task_creds_t *creds = get_task_creds();
    if (!creds) return -ESRCH;
    
    if (creds->euid != 0) return -EPERM;
    
    if (size < 0 || size > 16) return -EINVAL;
    
    if (size > 0) {
        addr = (uint64_t)(uintptr_t)list_ptr;
        if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
        memcpy(creds->groups, (void *)addr, size * sizeof(uint64_t));
    }
    
    creds->ngroups = size;
    sync_creds_to_task(creds);
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
    task_creds_t *creds;
    pid_t pid;

    (void)unused1; (void)unused2; (void)unused3;
    if (!current_task) return -ESRCH;
    pid = current_task->pid;
    creds = (task_creds_t *)current_task->creds_data;
    if (creds) {
        creds->sid = pid;
        creds->pgid = pid;
    }
    current_task->sid = pid;
    current_task->pgid = pid;
    
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
    if (!task) return;
    task->uid = 0;
    task->gid = 0;
    task->euid = 0;
    task->egid = 0;
    task->suid = 0;
    task->sgid = 0;
    task->fsuid = 0;
    task->fsgid = 0;
    task->groups = NULL;
    task->ngroups = 0;
    task->pgid = task->pid;
    task->sid = task->pid;
    task->creds_data = NULL;
}

void creds_release_task(task_t *task) {
    task_creds_t *creds;

    if (!task || !task->creds_data) return;
    creds = (task_creds_t *)task->creds_data;
    if (creds->syscall_mask) kfree(creds->syscall_mask);
    kfree(creds);
    task->creds_data = NULL;
}

int creds_syscall_allowed(task_t *task, int syscall_number) {
    task_creds_t *creds;
    int word;
    int bit;

    if (!task || syscall_number < 0 || syscall_number >= NR_SYSCALLS)
        return 0;
    creds = (task_creds_t *)task->creds_data;
    if (!creds || !creds->syscall_mask) return 1;
    word = syscall_number / 64;
    bit = syscall_number % 64;
    return (creds->syscall_mask[word] & (1ULL << bit)) != 0;
}

int creds_set_syscall_mask(task_t *task, const uint64_t *mask,
                           size_t word_count) {
    task_creds_t *creds;
    uint64_t *new_mask;
    size_t required_words;
    size_t i;

    if (!task || !mask) return -EINVAL;
    required_words = (NR_SYSCALLS + 63) / 64;
    if (word_count != required_words) return -EINVAL;
    creds = get_task_creds();
    if (!creds) return -ENOMEM;
    new_mask = (uint64_t *)kmalloc(required_words * sizeof(uint64_t));
    if (!new_mask) return -ENOMEM;
    memcpy(new_mask, mask, required_words * sizeof(uint64_t));
    if (creds->syscall_mask) {
        for (i = 0; i < required_words; i++)
            new_mask[i] &= creds->syscall_mask[i];
        kfree(creds->syscall_mask);
    }
    creds->syscall_mask = new_mask;
    creds->syscall_filter_mode = 2;
    return 0;
}

int creds_set_strict_syscalls(task_t *task) {
    uint64_t mask[(NR_SYSCALLS + 63) / 64];
    int result;

    if (!task) return -EINVAL;
    memset(mask, 0, sizeof(mask));
    mask[SYSCALL_EXIT / 64] |= 1ULL << (SYSCALL_EXIT % 64);
    mask[SYSCALL_READ / 64] |= 1ULL << (SYSCALL_READ % 64);
    mask[SYSCALL_WRITE / 64] |= 1ULL << (SYSCALL_WRITE % 64);
    mask[SYSCALL_SIGRETURN / 64] |= 1ULL << (SYSCALL_SIGRETURN % 64);
    mask[SYSCALL_RT_SIGRETURN / 64] |= 1ULL << (SYSCALL_RT_SIGRETURN % 64);
    result = creds_set_syscall_mask(task, mask, (NR_SYSCALLS + 63) / 64);
    if (result == 0)
        ((task_creds_t *)task->creds_data)->syscall_filter_mode = 1;
    return result;
}

int creds_set_no_new_privs(task_t *task) {
    task_creds_t *creds;

    if (!task) return -EINVAL;
    creds = get_task_creds();
    if (!creds) return -ENOMEM;
    creds->no_new_privs = 1;
    return 0;
}

int creds_get_no_new_privs(task_t *task) {
    task_creds_t *creds;

    if (!task) return 0;
    creds = (task_creds_t *)task->creds_data;
    return creds ? creds->no_new_privs : 0;
}

int creds_get_syscall_filter_mode(task_t *task) {
    task_creds_t *creds;

    if (!task) return 0;
    creds = (task_creds_t *)task->creds_data;
    return creds ? creds->syscall_filter_mode : 0;
}

int creds_set_dumpable(task_t *task, int dumpable) {
    task_creds_t *creds;

    if (!task || (dumpable != 0 && dumpable != 1)) return -EINVAL;
    creds = get_task_creds();
    if (!creds) return -ENOMEM;
    creds->dumpable = dumpable;
    return 0;
}

int creds_get_dumpable(task_t *task) {
    task_creds_t *creds;

    if (!task) return 0;
    creds = (task_creds_t *)task->creds_data;
    return creds ? creds->dumpable : 1;
}

int creds_has_capability(task_t *task, int capability) {
    task_creds_t *creds;

    if (!task || capability < 0 || capability >= 38) return 0;
    creds = (task_creds_t *)task->creds_data;
    if (!creds) return task->euid == 0;
    return (creds->cap_effective & (1ULL << capability)) != 0;
}

int creds_copy_task(task_t *parent, task_t *child) {
    task_creds_t *pcreds;
    task_creds_t *ccreds;
    uint64_t *copied_mask;
    size_t mask_bytes;

    if (!parent || !child) return -1;
    pcreds = (task_creds_t *)parent->creds_data;
    ccreds = (task_creds_t *)child->creds_data;
    child->uid   = parent->uid;
    child->euid  = parent->euid;
    child->suid  = parent->suid;
    child->gid   = parent->gid;
    child->egid  = parent->egid;
    child->sgid  = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->groups = NULL;
    child->ngroups = parent->ngroups;
    child->pgid = parent->pgid ? parent->pgid : child->pid;
    child->sid = parent->sid ? parent->sid : child->pid;
    child->ppid = parent->pid;
    if (!pcreds) {
        child->creds_data = NULL;
        return 0;
    }
    if (!ccreds) {
        ccreds = (task_creds_t *)kmalloc(sizeof(task_creds_t));
        if (!ccreds) return -1;
        memset(ccreds, 0, sizeof(task_creds_t));
        child->creds_data = ccreds;
    }
    copied_mask = NULL;
    mask_bytes = ((NR_SYSCALLS + 63) / 64) * sizeof(uint64_t);
    if (pcreds->syscall_mask) {
        copied_mask = (uint64_t *)kmalloc(mask_bytes);
        if (!copied_mask) return -1;
        memcpy(copied_mask, pcreds->syscall_mask, mask_bytes);
    }
    memcpy(ccreds, pcreds, sizeof(task_creds_t));
    ccreds->syscall_mask = copied_mask;
    child->groups = ccreds->groups;
    child->uid   = ccreds->uid;
    child->euid  = ccreds->euid;
    child->suid  = ccreds->suid;
    child->gid   = ccreds->gid;
    child->egid  = ccreds->egid;
    child->sgid  = ccreds->sgid;
    child->fsuid = ccreds->fsuid;
    child->fsgid = ccreds->fsgid;
    child->ngroups = ccreds->ngroups;
    child->pgid = ccreds->pgid;
    child->sid = ccreds->sid;
    child->ppid = parent->pid;
    return 0;
}

static int sys_capget(cap_header_t *header, cap_data_t *data) {
    cap_header_t local_header;
    cap_data_t local_data[2];
    task_creds_t *creds;
    uint64_t effective;
    uint64_t permitted;
    uint64_t inheritable;

    if (!header || !data || !current_task) return -EFAULT;
    if (copy_from_user(&local_header, header, sizeof(local_header)) != 0)
        return -EFAULT;
    if (local_header.version != CAP_VERSION_3) return -EINVAL;
    if (local_header.pid != 0 && local_header.pid != current_task->pid)
        return -EPERM;
    creds = (task_creds_t *)current_task->creds_data;
    effective = creds ? creds->cap_effective :
                (current_task->euid == 0 ? CAP_FULL_SET : 0);
    permitted = creds ? creds->cap_permitted :
                (current_task->euid == 0 ? CAP_FULL_SET : 0);
    inheritable = creds ? creds->cap_inheritable : 0;
    memset(local_data, 0, sizeof(local_data));
    local_data[0].effective = (uint32_t)effective;
    local_data[0].permitted = (uint32_t)permitted;
    local_data[0].inheritable = (uint32_t)inheritable;
    local_data[1].effective = (uint32_t)(effective >> 32);
    local_data[1].permitted = (uint32_t)(permitted >> 32);
    local_data[1].inheritable = (uint32_t)(inheritable >> 32);
    if (copy_to_user(data, local_data, sizeof(local_data)) != 0)
        return -EFAULT;
    return 0;
}

static int sys_capset(cap_header_t *header, const cap_data_t *data) {
    cap_header_t local_header;
    cap_data_t local_data[2];
    task_creds_t *creds;
    uint64_t effective;
    uint64_t permitted;
    uint64_t inheritable;
    uint64_t old_permitted;

    if (!header || !data || !current_task) return -EFAULT;
    if (copy_from_user(&local_header, header, sizeof(local_header)) != 0 ||
        copy_from_user(local_data, data, sizeof(local_data)) != 0)
        return -EFAULT;
    if (local_header.version != CAP_VERSION_3) return -EINVAL;
    if (local_header.pid != 0 && local_header.pid != current_task->pid)
        return -EPERM;
    effective = local_data[0].effective |
                ((uint64_t)local_data[1].effective << 32);
    permitted = local_data[0].permitted |
                ((uint64_t)local_data[1].permitted << 32);
    inheritable = local_data[0].inheritable |
                  ((uint64_t)local_data[1].inheritable << 32);
    if ((effective & ~permitted) != 0 ||
        ((effective | permitted | inheritable) & ~CAP_FULL_SET) != 0)
        return -EPERM;
    creds = get_task_creds();
    if (!creds) return -ENOMEM;
    old_permitted = creds->cap_permitted;
    if ((permitted & ~old_permitted) != 0) return -EPERM;
    creds->cap_effective = effective;
    creds->cap_permitted = permitted;
    creds->cap_inheritable = inheritable;
    return 0;
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
    syscall_table_set(SYSCALL_GETUID, (void *)(sys_getuid));
    syscall_table_set(SYSCALL_GETGID, (void *)(sys_getgid));
    syscall_table_set(SYSCALL_GETEUID, (void *)(sys_geteuid));
    syscall_table_set(SYSCALL_GETEGID, (void *)(sys_getegid));
    syscall_table_set(SYSCALL_SETUID, (void *)(sys_setuid));
    syscall_table_set(SYSCALL_SETGID, (void *)(sys_setgid));
    syscall_table_set(SYSCALL_SETEUID, (void *)(sys_seteuid));
    syscall_table_set(SYSCALL_SETEGID, (void *)(sys_setegid));
    syscall_table_set(SYSCALL_SETREUID, (void *)(sys_setreuid));
    syscall_table_set(SYSCALL_SETREGID, (void *)(sys_setregid));
    syscall_table_set(SYSCALL_SETRESUID, (void *)(sys_setresuid));
    syscall_table_set(SYSCALL_SETRESGID, (void *)(sys_setresgid));
    syscall_table_set(SYSCALL_GETRESUID, (void *)(sys_getresuid));
    syscall_table_set(SYSCALL_GETRESGID, (void *)(sys_getresgid));
    syscall_table_set(SYSCALL_SETFSUID, (void *)(sys_setfsuid));
    syscall_table_set(SYSCALL_SETFSGID, (void *)(sys_setfsgid));
    syscall_table_set(SYSCALL_GETGROUPS, (void *)(sys_getgroups));
    syscall_table_set(SYSCALL_SETGROUPS, (void *)(sys_setgroups));
    syscall_table_set(SYSCALL_GETPGID, (void *)(sys_getpgid));
    syscall_table_set(SYSCALL_SETPGID, (void *)(sys_setpgid));
    syscall_table_set(SYSCALL_GETPGRP, (void *)(sys_getpgrp));
    syscall_table_set(SYSCALL_SETSID, (void *)(sys_setsid));
    syscall_table_set(SYSCALL_GETSID, (void *)(sys_getsid));
    syscall_table_set(SYSCALL_GETPPID, (void *)(sys_getppid));
    syscall_table_set(SYSCALL_GETPID2, (void *)(sys_getpid_impl));
    syscall_table_set(SYSCALL_GETTID, (void *)(sys_gettid));
    syscall_table_set(SYSCALL_CAPGET, (void *)(sys_capget));
    syscall_table_set(SYSCALL_CAPSET, (void *)(sys_capset));
}

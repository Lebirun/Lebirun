#include "syscall_defs.h"
#include <lebirun/common.h>
#include <stdarg.h>

#define WNOHANG   1
#define WUNTRACED 2
#define WCONTINUED 8

#define P_ALL  0
#define P_PID  1
#define P_PGID 2

static void syscall_error(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (current_task && current_task->console_id >= 0 && console_is_initialized()) {
        console_write_to(current_task->console_id, buf, (size_t)n);
    }
}

static int sys_getpid(int unused, const char *unused2, int unused3) {
    (void)unused;
    (void)unused2;
    (void)unused3;
    return (int)getpid();
}

static int sys_yield(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    schedule();
    return 0;
}

static int sys_sleep(int ms, const char *unused, int unused2) {
    extern int task_has_pending_signals(void);
    (void)unused; (void)unused2;
    if (ms <= 0) return -EINVAL;
    sleep_ms((uint64_t)ms);
    if (task_has_pending_signals()) return -EINTR;
    return 0;
}

static int wait_status_from_exit_code(uint64_t exit_code) {
    if (exit_code >= 128 && exit_code < 192) {
        return (int)(exit_code - 128);
    }
    return (int)((exit_code & 0xFF) << 8);
}

static int sys_waitpid(int pid, const char *status_ptr, int options) {
    extern int task_has_pending_signals(void);
    pid_t pgid_filter;
    task_t* t;
    task_t *dead;
    pid_t dead_pid;
    uint64_t exit_code;
    uint64_t addr;
    int status;
    int r;
    int wait_state;

    if (!current_task) return -ESRCH;

    pgid_filter = 0;
    if (pid == 0) {
        pgid_filter = current_task->pgid ? current_task->pgid : current_task->pid;
        pid = -(int)pgid_filter;
    }

    if (pid > 0) {
        if (options & WNOHANG) {
            t = task_find((pid_t)pid);
            if (!t || t->ppid != current_task->pid) return -ECHILD;
            if (t->state != TASK_DEAD) return 0;
        }

        exit_code = 0;
        r = task_wait_child_pid((pid_t)pid, current_task->pid, &exit_code);
        if (r == -EINTR) return -EINTR;
        if (r != 0) return -ECHILD;

        reap_dead_tasks();

        if (status_ptr) {
            addr = (uint64_t)status_ptr;
            if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
            status = wait_status_from_exit_code(exit_code);
            memcpy((void*)addr, &status, sizeof(int));
        }
        return (int)pid;
    }

    if (pid < -1) {
        pgid_filter = (pid_t)(-pid);
    }

    if (pid != -1 && pid >= 0) {
        return -EINVAL;
    }

    if (!task_has_child_of(current_task->pid, pgid_filter)) {
        return -ECHILD;
    }

    for (;;) {
        dead = task_find_dead_child_of(current_task->pid, pgid_filter);
        if (dead) {
            dead_pid = dead->pid;
            exit_code = 0;
            r = task_join(dead, &exit_code);
            if (r != 0) return -ECHILD;

            dead->waited = 1;
            reap_dead_tasks();

            if (status_ptr) {
                addr = (uint64_t)status_ptr;
                if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
                status = wait_status_from_exit_code(exit_code);
                memcpy((void*)addr, &status, sizeof(int));
            }
            return (int)dead_pid;
        }

        if (options & WNOHANG) {
            return 0;
        }

        wait_state = task_prepare_wait_any_child(current_task->pid, pgid_filter);
        if (wait_state < 0) {
            return -ECHILD;
        }
        if (wait_state > 0) {
            continue;
        }
        schedule();
        task_finish_wait_any_child();
        if (task_has_pending_signals()) {
            dead = task_find_dead_child_of(current_task->pid, pgid_filter);
            if (dead) {
                continue;
            }
            return -EINTR;
        }
    }
}

static int sys_wait4(int pid, const char *status_ptr, int options) {
    return sys_waitpid(pid, status_ptr, options);
}

struct siginfo_k {
    int si_signo;
    int si_errno;
    int si_code;
    int _pad[29];
};

static int sys_waitid(int idtype, const char *id_ptr, int infop) {
    int id;
    uint64_t info_addr;
    pid_t target_pid;
    uint64_t exit_code;
    int r;
    struct siginfo_k *info;

    id = (int)(uintptr_t)id_ptr;
    info_addr = (uint64_t)infop;
    target_pid = -1;
    
    if (idtype == P_PID) {
        target_pid = (pid_t)id;
    } else if (idtype == P_ALL) {
        target_pid = -1;
    } else if (idtype == P_PGID) {
        target_pid = -1;
    } else {
        return -EINVAL;
    }
    
    if (target_pid > 0) {
        exit_code = 0;
        r = task_wait_child_pid(target_pid, current_task->pid, &exit_code);
        if (r == -EINTR) return -EINTR;
        if (r != 0) return -ECHILD;
        reap_dead_tasks();

        if (info_addr && info_addr < KERNEL_VMA && info_addr >= 0x1000) {
            info = (struct siginfo_k *)info_addr;
            memset(info, 0, sizeof(struct siginfo_k));
            info->si_signo = 17;
            info->si_code = 1;
        }
        return 0;
    }
    
    return -ECHILD;
}

static int sys_kill(int pid, const char *sig_arg, int unused) {
    (void)unused;
    return sys_kill_impl(pid, sig_arg, 0);
}

static int sys_fork(int unused, const char *unused2, int unused3) {
    registers_t *regs;
    int result;

    (void)unused; (void)unused2; (void)unused3;
    regs = current_task->syscall_frame;
    if (!regs) {
        printf("sys_fork: no registers pointer\n");
        return -EAGAIN;
    }
    result = (int)task_fork(regs, 0, 0, 0, NULL, NULL);
    return result;
}

static int sys_exec(uint64_t bin_ptr, const char *size_ptr, int unused) {
    registers_t *regs;
    uint64_t bin_addr;
    uint64_t bin_size;
    int result;

    (void)unused;
    bin_addr = (uint64_t)bin_ptr;
    bin_size = (uint64_t)(uintptr_t)size_ptr;

    if (bin_addr >= KERNEL_VMA || bin_addr < 0x1000) {
        syscall_error("sys_exec: invalid binary pointer 0x%016lX\n", bin_addr);
        return -EFAULT;
    }
    if (bin_size == 0) {
        syscall_error("sys_exec: invalid size %u\n", bin_size);
        return -EINVAL;
    }
    if (bin_addr + bin_size < bin_addr || bin_addr + bin_size >= KERNEL_VMA) {
        syscall_error("sys_exec: binary range overflows into kernel space\n");
        return -EFAULT;
    }

    regs = current_task->syscall_frame;
    if (!regs) {
        syscall_error("sys_exec: no registers pointer\n");
        return -EAGAIN;
    }

    result = task_exec((const uint8_t *)bin_addr, bin_size, regs);
    
    if (result == 0) {
        syscall_set_exec_completed();
        if (current_task) {
            current_task->exec_completed = 1;
        }
    }
    
    return result;
}

static int sys_vfork(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return sys_fork(0, NULL, 0);
}

static int sys_clone(int flags, const char *child_stack, int ptid,
                     uint64_t tls_base, int *child_tid, int unused) {
    registers_t *regs;
    int share_address_space;

    (void)unused;
    regs = current_task ? current_task->syscall_frame : NULL;
    if (!regs) return -EAGAIN;
    share_address_space = (flags & 0x00000100) != 0;
    if (share_address_space &&
        (!child_stack || (uint64_t)(uintptr_t)child_stack < 0x1000 ||
         (uint64_t)(uintptr_t)child_stack >= KERNEL_VMA)) return -EFAULT;
    return (int)task_fork(regs, share_address_space,
                          (uint64_t)(uintptr_t)child_stack, tls_base,
                          (int *)(uintptr_t)ptid, child_tid);
}

struct clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

#define CLONE3_ALLOW_FLAGS (0x00000100u | 0x00000200u | 0x00000400u | \
    0x00000800u | 0x00010000u | 0x00040000u | 0x00080000u | \
    0x00100000u | 0x00200000u | 0x01000000u)

static int sys_clone3(uint64_t uargs_addr, uint64_t usize, uint64_t unused2,
                      uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    struct clone_args args;
    registers_t *regs;
    uint64_t stack_top;
    int share_address_space;

    (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (usize != sizeof(args)) return -EINVAL;
    if (uargs_addr < 0x1000 || uargs_addr >= KERNEL_VMA) return -EFAULT;
    if (copy_from_user(&args, (const void *)(uintptr_t)uargs_addr,
                       sizeof(args)) < 0) return -EFAULT;
    if (args.flags & ~((uint64_t)CLONE3_ALLOW_FLAGS)) return -EINVAL;
    if (args.exit_signal > 64) return -EINVAL;
    if (args.pidfd || args.set_tid || args.set_tid_size || args.cgroup)
        return -EINVAL;
    regs = current_task ? current_task->syscall_frame : NULL;
    if (!regs) return -EAGAIN;
    share_address_space = (args.flags & 0x00000100u) != 0;
    stack_top = 0;
    if (share_address_space) {
        if (!args.stack || !args.stack_size) return -EINVAL;
        if (args.stack < 0x1000 || args.stack >= KERNEL_VMA) return -EFAULT;
        if (args.stack_size >= KERNEL_VMA ||
            args.stack + args.stack_size < args.stack ||
            args.stack + args.stack_size >= KERNEL_VMA) return -EFAULT;
        stack_top = args.stack + args.stack_size;
    }
    if ((args.flags & 0x00100000u) &&
        (args.parent_tid < 0x1000 || args.parent_tid >= KERNEL_VMA))
        return -EFAULT;
    if ((args.flags & 0x01000000u) &&
        (args.child_tid < 0x1000 || args.child_tid >= KERNEL_VMA))
        return -EFAULT;
    return (int)task_fork(regs, share_address_space, stack_top,
                          (args.flags & 0x00080000u) ? args.tls : 0,
                          (args.flags & 0x00100000u) ?
                              (int *)(uintptr_t)args.parent_tid : NULL,
                          (args.flags & 0x01000000u) ?
                              (int *)(uintptr_t)args.child_tid : NULL);
}

#define PIDFD_BASE_FD 0x68000000
#define PIDFD_CLOEXEC 0x80000
#define PIDFD_NONBLOCK 0x800

typedef struct {
    int in_use;
    pid_t pid;
    pid_t owner_pid;
    int flags;
} pidfd_entry_t;

static pidfd_entry_t *pidfds;
static int pidfd_capacity;
static mutex_t pidfd_lock;

static int pidfd_grow(void) {
    pidfd_entry_t *grown;
    int cap;
    int i;

    if (pidfd_capacity > INT32_MAX / 2) return -1;
    cap = pidfd_capacity ? pidfd_capacity * 2 : 1;
    grown = (pidfd_entry_t *)krealloc(pidfds,
                                      (size_t)cap * sizeof(pidfd_entry_t));
    if (!grown) return -1;
    for (i = pidfd_capacity; i < cap; i++)
        memset(&grown[i], 0, sizeof(pidfd_entry_t));
    pidfds = grown;
    pidfd_capacity = cap;
    return 0;
}

static int sys_pidfd_open(int pid, unsigned int flags) {
    task_t *target;
    int i;
    int idx;

    if (flags & ~(unsigned int)(PIDFD_CLOEXEC | PIDFD_NONBLOCK))
        return -EINVAL;
    if (!current_task) return -ESRCH;
    if (pid <= 0) return -EINVAL;
    mutex_lock(&pidfd_lock);
    target = task_find((pid_t)pid);
    if (!target || target->state == TASK_DEAD) {
        mutex_unlock(&pidfd_lock);
        return -ESRCH;
    }
    idx = -1;
    for (i = 0; i < pidfd_capacity; i++) {
        if (!pidfds[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (pidfd_grow() < 0) {
            mutex_unlock(&pidfd_lock);
            return -EMFILE;
        }
        idx = pidfd_capacity / 2;
    }
    memset(&pidfds[idx], 0, sizeof(pidfd_entry_t));
    pidfds[idx].in_use = 1;
    pidfds[idx].pid = (pid_t)pid;
    pidfds[idx].owner_pid = current_task->pid;
    pidfds[idx].flags = (int)flags;
    mutex_unlock(&pidfd_lock);
    return PIDFD_BASE_FD + idx;
}

pid_t pidfd_lookup(int pidfd) {
    int idx;
    pid_t pid;

    mutex_lock(&pidfd_lock);
    idx = pidfd - PIDFD_BASE_FD;
    if (idx < 0 || idx >= pidfd_capacity || !pidfds ||
        !pidfds[idx].in_use || !current_task ||
        pidfds[idx].owner_pid != current_task->pid) {
        mutex_unlock(&pidfd_lock);
        return -1;
    }
    pid = pidfds[idx].pid;
    mutex_unlock(&pidfd_lock);
    return pid;
}

int pidfd_is_fd(int fd) {
    int idx;
    int found;

    found = 0;
    mutex_lock(&pidfd_lock);
    idx = fd - PIDFD_BASE_FD;
    if (idx >= 0 && idx < pidfd_capacity && pidfds &&
        pidfds[idx].in_use && current_task &&
        pidfds[idx].owner_pid == current_task->pid) found = 1;
    mutex_unlock(&pidfd_lock);
    return found;
}

int pidfd_close_fd(int fd) {
    int idx;
    int i;
    int any;

    mutex_lock(&pidfd_lock);
    idx = fd - PIDFD_BASE_FD;
    if (idx < 0 || idx >= pidfd_capacity || !pidfds ||
        !pidfds[idx].in_use || !current_task ||
        pidfds[idx].owner_pid != current_task->pid) {
        mutex_unlock(&pidfd_lock);
        return -EBADF;
    }
    memset(&pidfds[idx], 0, sizeof(pidfd_entry_t));
    any = 0;
    for (i = 0; i < pidfd_capacity; i++) {
        if (pidfds[i].in_use) {
            any = 1;
            break;
        }
    }
    if (!any) {
        kfree(pidfds);
        pidfds = NULL;
        pidfd_capacity = 0;
    }
    mutex_unlock(&pidfd_lock);
    return 0;
}

void pidfd_close_range(unsigned int first, unsigned int last, int cloexec) {
    unsigned int fd;
    int i;

    if (!current_task) return;
    if (cloexec) {
        mutex_lock(&pidfd_lock);
        for (i = 0; i < pidfd_capacity; i++) {
            fd = (unsigned int)(PIDFD_BASE_FD + i);
            if (fd >= first && fd <= last && pidfds[i].in_use &&
                pidfds[i].owner_pid == current_task->pid)
                pidfds[i].flags |= PIDFD_CLOEXEC;
        }
        mutex_unlock(&pidfd_lock);
        return;
    }
    for (i = pidfd_capacity - 1; i >= 0; i--) {
        fd = (unsigned int)(PIDFD_BASE_FD + i);
        if (fd >= first && fd <= last && pidfd_is_fd((int)fd))
            pidfd_close_fd((int)fd);
    }
}

void pidfd_close_cloexec(pid_t pid) {
    int i;

    mutex_lock(&pidfd_lock);
    for (i = 0; i < pidfd_capacity; i++) {
        if (!pidfds[i].in_use || pidfds[i].owner_pid != pid ||
            !(pidfds[i].flags & PIDFD_CLOEXEC)) continue;
        memset(&pidfds[i], 0, sizeof(pidfd_entry_t));
    }
    mutex_unlock(&pidfd_lock);
}

void pidfd_close_task(pid_t pid) {
    int i;
    int any;

    mutex_lock(&pidfd_lock);
    any = 0;
    for (i = 0; i < pidfd_capacity; i++) {
        if (pidfds[i].in_use && (pidfds[i].owner_pid == pid ||
                                 pidfds[i].pid == pid))
            memset(&pidfds[i], 0, sizeof(pidfd_entry_t));
        if (pidfds[i].in_use) any = 1;
    }
    if (!any) {
        kfree(pidfds);
        pidfds = NULL;
        pidfd_capacity = 0;
    }
    mutex_unlock(&pidfd_lock);
}

static int sys_pidfd_send_signal(int pidfd, int sig, const void *info,
                                 unsigned int flags) {
    pid_t pid;

    if (flags != 0) return -EINVAL;
    if (info) return -EINVAL;
    if (sig < 0 || sig >= 65) return -EINVAL;
    pid = pidfd_lookup(pidfd);
    if (pid < 0) return -EBADF;
    return sys_kill_impl((int)pid, (const char *)(uintptr_t)(uint64_t)sig, 0);
}

struct proc_vm_iovec {
    void *base;
    size_t len;
};

static uint64_t proc_vm_pd(task_t *task) {
    uint64_t pd;

    if (!task) return 0;
    pd = task->cr3 ? task->cr3 : task->pml4_phys;
    return pd;
}

static int proc_vm_range_ok(task_t *task, uint64_t addr, size_t len) {
    uint64_t pd;
    uint64_t end;
    uint64_t page;

    if (!task || len == 0) return 0;
    pd = proc_vm_pd(task);
    if (!pd) return 0;
    if (addr < 0x1000 || len > UINT64_MAX - addr) return 0;
    end = addr + (uint64_t)len - 1;
    if (end >= KERNEL_VMA) return 0;
    page = addr & ~(PAGE_SIZE - 1);
    for (;;) {
        if (!vmm_get_phys_in_pml4(pd, page)) return 0;
        if (page >= end) break;
        if (page > UINT64_MAX - PAGE_SIZE) return 0;
        page += PAGE_SIZE;
    }
    return 1;
}

static int proc_vm_remote_ok(task_t *task, uint64_t addr, size_t len) {
    if (!task || len == 0) return 0;
    if (task == current_task)
        return syscall_user_range_mapped(addr, (uint64_t)len, 1);
    return proc_vm_range_ok(task, addr, len);
}

static int proc_vm_copy(task_t *task, uint64_t addr, void *buf, size_t len,
                        int to_remote) {
    uint64_t pd;

    if (task == current_task) {
        if (to_remote)
            return copy_to_user((void *)(uintptr_t)addr, buf, len);
        return copy_from_user(buf, (const void *)(uintptr_t)addr, len);
    }
    pd = proc_vm_pd(task);
    if (!pd) return -1;
    if (!proc_vm_range_ok(task, addr, len)) return -1;
    if (to_remote)
        vmm_copy_to_pml4(pd, addr, buf, (uint64_t)len);
    else
        vmm_read_from_pml4(pd, addr, buf, (uint64_t)len);
    return 0;
}

static int sys_process_vm(int pid, const struct proc_vm_iovec *local,
                          unsigned long liovcnt,
                          const struct proc_vm_iovec *remote,
                          unsigned long riovcnt, unsigned long flags,
                          int to_remote) {
    task_t *target;
    struct proc_vm_iovec liov;
    struct proc_vm_iovec riov;
    uint8_t chunk[1024];
    uint64_t local_addr;
    uint64_t remote_addr;
    size_t local_left;
    size_t remote_left;
    size_t step;
    unsigned long li;
    unsigned long ri;
    uint64_t total;

    if (flags != 0) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (liovcnt > 1024 || riovcnt > 1024) return -EINVAL;
    if ((liovcnt == 0 || !local) && (riovcnt == 0 || !remote)) return 0;
    if (liovcnt != 0 && !local) return -EFAULT;
    if (riovcnt != 0 && !remote) return -EFAULT;
    lock_scheduler();
    target = task_find((pid_t)pid);
    if (!target || target->state == TASK_DEAD || !target->pml4_phys) {
        unlock_scheduler();
        return -ESRCH;
    }
    if (target != current_task && current_task->euid != 0 &&
        current_task->euid != target->euid &&
        current_task->uid != target->euid) {
        unlock_scheduler();
        return -EPERM;
    }
    total = 0;
    li = 0;
    ri = 0;
    local_left = 0;
    remote_left = 0;
    local_addr = 0;
    remote_addr = 0;
    for (;;) {
        while (local_left == 0 && li < liovcnt) {
            if (copy_from_user(&liov,
                               &local[li],
                               sizeof(liov)) < 0) {
                unlock_scheduler();
                return total > 0 ? (int)total : -EFAULT;
            }
            li++;
            if (liov.len == 0) continue;
            local_addr = (uint64_t)(uintptr_t)liov.base;
            if (!syscall_user_range_mapped(local_addr,
                                           (uint64_t)liov.len, 1)) {
                unlock_scheduler();
                return total > 0 ? (int)total : -EFAULT;
            }
            local_left = liov.len;
            break;
        }
        while (remote_left == 0 && ri < riovcnt) {
            if (copy_from_user(&riov,
                               &remote[ri],
                               sizeof(riov)) < 0) {
                unlock_scheduler();
                return total > 0 ? (int)total : -EFAULT;
            }
            ri++;
            if (riov.len == 0) continue;
            remote_addr = (uint64_t)(uintptr_t)riov.base;
            if (!proc_vm_remote_ok(target, remote_addr, riov.len)) {
                unlock_scheduler();
                return total > 0 ? (int)total : -EFAULT;
            }
            remote_left = riov.len;
            break;
        }
        if (local_left == 0 || remote_left == 0) break;
        step = local_left < remote_left ? local_left : remote_left;
        if (step > sizeof(chunk)) step = sizeof(chunk);
        if (!to_remote) {
            if (proc_vm_copy(target, remote_addr, chunk, step, 0) < 0 ||
                copy_to_user((void *)(uintptr_t)local_addr, chunk,
                             step) < 0) {
                unlock_scheduler();
                return total > 0 ? (int)total : -EFAULT;
            }
        } else {
            if (copy_from_user(chunk, (const void *)(uintptr_t)local_addr,
                               step) < 0 ||
                proc_vm_copy(target, remote_addr, chunk, step, 1) < 0) {
                unlock_scheduler();
                return total > 0 ? (int)total : -EFAULT;
            }
        }
        local_addr += step;
        remote_addr += step;
        local_left -= step;
        remote_left -= step;
        total += step;
        if (total >= INT32_MAX) break;
    }
    unlock_scheduler();
    return (int)total;
}

static int sys_process_vm_readv(int pid, const struct proc_vm_iovec *local,
                                unsigned long liovcnt,
                                const struct proc_vm_iovec *remote,
                                unsigned long riovcnt, unsigned long flags) {
    return sys_process_vm(pid, local, liovcnt, remote, riovcnt, flags, 0);
}

static int sys_process_vm_writev(int pid, const struct proc_vm_iovec *local,
                                 unsigned long liovcnt,
                                 const struct proc_vm_iovec *remote,
                                 unsigned long riovcnt,
                                 unsigned long flags) {
    return sys_process_vm(pid, local, liovcnt, remote, riovcnt, flags, 1);
}

void syscalls_process_init(void) {
    mutex_init(&pidfd_lock);
    syscall_table_set(SYSCALL_GETPID, (void *)(sys_getpid));
    syscall_table_set(SYSCALL_YIELD, (void *)(sys_yield));
    syscall_table_set(SYSCALL_SLEEP, (void *)(sys_sleep));
    syscall_table_set(SYSCALL_WAITPID, (void *)(sys_waitpid));
    syscall_table_set(SYSCALL_WAIT4, (void *)(sys_wait4));
    syscall_table_set(SYSCALL_WAITID, (void *)(sys_waitid));
    syscall_table_set(SYSCALL_KILL, (void *)(sys_kill));
    syscall_table_set(SYSCALL_FORK, (void *)(sys_fork));
    syscall_table_set(SYSCALL_EXEC, (void *)(sys_exec));
    syscall_table_set(SYSCALL_VFORK, (void *)(sys_vfork));
    syscall_table_set(SYSCALL_CLONE, (void *)(sys_clone));
    syscall_table_set(SYSCALL_CLONE3, (void *)(sys_clone3));
    syscall_table_set(SYSCALL_PIDFD_OPEN, (void *)(sys_pidfd_open));
    syscall_table_set(SYSCALL_PIDFD_SEND_SIGNAL,
                      (void *)(sys_pidfd_send_signal));
    syscall_table_set(SYSCALL_PROCESS_VM_READV,
                      (void *)(sys_process_vm_readv));
    syscall_table_set(SYSCALL_PROCESS_VM_WRITEV,
                      (void *)(sys_process_vm_writev));
}

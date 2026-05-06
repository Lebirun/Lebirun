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
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
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

static int sys_waitpid(int pid, const char *status_ptr, int options) {
    if (!current_task) return -ESRCH;

    pid_t pgid_filter = 0;
    if (pid == 0) {
        pgid_filter = current_task->pgid ? current_task->pgid : current_task->pid;
        pid = -(int)pgid_filter;
    }

    if (pid > 0) {
        task_t* t = task_find((pid_t)pid);
        if (!t) return -ECHILD;
        if (t->ppid != current_task->pid) return -ECHILD;

        if (options & WNOHANG) {
            if (t->state != TASK_DEAD) return 0;
        }

        uint64_t exit_code = 0;
        int r = task_join(t, &exit_code);
        if (r != 0) return -ECHILD;

        t->waited = 1;
        reap_dead_tasks();

        if (status_ptr) {
            uint64_t addr = (uint64_t)status_ptr;
            if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
            int status = (exit_code & 0xFF) << 8;
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
        task_t *dead = task_find_dead_child_of(current_task->pid, pgid_filter);
        if (dead) {
            current_task->waiting_for_any_child = 0;
            pid_t dead_pid = dead->pid;
            uint64_t exit_code = 0;
            int r = task_join(dead, &exit_code);
            if (r != 0) return -ECHILD;

            dead->waited = 1;
            reap_dead_tasks();

            if (status_ptr) {
                uint64_t addr = (uint64_t)status_ptr;
                if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
                int status = (exit_code & 0xFF) << 8;
                memcpy((void*)addr, &status, sizeof(int));
            }
            return (int)dead_pid;
        }

        if (options & WNOHANG) {
            return 0;
        }

        current_task->waiting_for_any_child = 1;
        current_task->state = TASK_BLOCKED;
        schedule();
        current_task->waiting_for_any_child = 0;

        if (!task_has_child_of(current_task->pid, pgid_filter)) {
            return -ECHILD;
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
    int id = (int)(uintptr_t)id_ptr;
    uint64_t info_addr = (uint64_t)infop;
    
    pid_t target_pid = -1;
    
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
        task_t *t = task_find(target_pid);
        if (!t) return -ECHILD;
        
        uint64_t exit_code = 0;
        int r = task_join(t, &exit_code);
        if (r != 0) return -ECHILD;
        
        t->waited = 1;
        reap_dead_tasks();

        if (info_addr && info_addr < KERNEL_VMA && info_addr >= 0x1000) {
            struct siginfo_k *info = (struct siginfo_k *)info_addr;
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
    result = (int)task_fork(regs);
    return result;
}

static int sys_exec(int bin_ptr, const char *size_ptr, int unused) {
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

static int sys_clone(int flags, const char *child_stack, int ptid) {
    (void)flags; (void)child_stack; (void)ptid;
    return sys_fork(0, NULL, 0);
}

void syscalls_process_init(void) {
    syscall_table[SYSCALL_GETPID] = sys_getpid;
    syscall_table[SYSCALL_YIELD] = sys_yield;
    syscall_table[SYSCALL_SLEEP] = sys_sleep;
    syscall_table[SYSCALL_WAITPID] = sys_waitpid;
    syscall_table[SYSCALL_WAIT4] = sys_wait4;
    syscall_table[SYSCALL_WAITID] = sys_waitid;
    syscall_table[SYSCALL_KILL] = sys_kill;
    syscall_table[SYSCALL_FORK] = sys_fork;
    syscall_table[SYSCALL_EXEC] = sys_exec;
    syscall_table[SYSCALL_VFORK] = sys_vfork;
    syscall_table[SYSCALL_CLONE] = sys_clone;
}

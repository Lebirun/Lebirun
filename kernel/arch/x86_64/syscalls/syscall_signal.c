#include "syscall_defs.h"

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define NSIG      33
#define _NSIG     NSIG

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef struct {
    unsigned long sig[2];
} sigset_k;

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_k;

typedef struct {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_k sa_mask;
} sigaction_k;

typedef struct {
    pid_t owner_pid;
    sigset_k pending;
    sigset_k blocked;
    sigaction_k actions[NSIG];
    stack_k altstack;
    int in_signal;
} task_signals_t;

static void init_signal_slot(task_signals_t *slot, pid_t pid) {
    int i;

    memset(slot, 0, sizeof(task_signals_t));
    slot->owner_pid = pid;
    for (i = 0; i < NSIG; i++) {
        slot->actions[i].sa_handler = SIG_DFL;
    }
}

static task_signals_t *get_task_signals(void) {
    task_signals_t *slot;

    if (!current_task) return NULL;
    slot = (task_signals_t *)current_task->signal_data;
    if (!slot) {
        slot = (task_signals_t *)kmalloc(sizeof(task_signals_t));
        if (!slot) return NULL;
        init_signal_slot(slot, current_task->pid);
        current_task->signal_data = slot;
    }
    if (slot->owner_pid != current_task->pid) {
        init_signal_slot(slot, current_task->pid);
    }
    return slot;
}

int task_has_sigint_ignored(void) {
    task_signals_t *sigs;

    sigs = get_task_signals();
    if (!sigs) return 0;
    return sigs->actions[2].sa_handler == SIG_IGN;
}

void task_reset_signals_on_exec(void) {
    task_signals_t *sigs;
    int i;

    sigs = get_task_signals();
    if (!sigs) return;

    sigs->pending.sig[0] = 0;
    sigs->blocked.sig[0] = 0;
    sigs->in_signal = 0;

    for (i = 1; i < NSIG; i++) {
        if (sigs->actions[i].sa_handler != SIG_IGN)
            sigs->actions[i].sa_handler = SIG_DFL;
        sigs->actions[i].sa_flags = 0;
        sigs->actions[i].sa_restorer = NULL;
        sigs->actions[i].sa_mask.sig[0] = 0;
    }
}

int task_has_pending_signals(void) {
    task_signals_t *sigs;
    unsigned long unblocked;

    sigs = get_task_signals();
    if (!sigs) return 0;
    unblocked = sigs->pending.sig[0] & ~sigs->blocked.sig[0];
    return unblocked != 0;
}

static int sys_rt_sigaction(int signum, const char *act_ptr, int oldact_ptr) {
    task_signals_t *sigs;
    uint64_t act_addr;
    uint64_t old_addr;

    if (signum < 1 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;
    
    sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    act_addr = (uint64_t)(uintptr_t)act_ptr;
    old_addr = (uint64_t)oldact_ptr;
    
    if (old_addr && old_addr < KERNEL_VMA && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sigs->actions[signum], sizeof(sigaction_k));
    }
    
    if (act_addr && act_addr < KERNEL_VMA && act_addr >= 0x1000) {
        memcpy(&sigs->actions[signum], (void *)act_addr, sizeof(sigaction_k));
    }
    
    return 0;
}

static int sys_rt_sigprocmask(int how, const char *set_ptr, int oldset_ptr) {
    task_signals_t *sigs;
    uint64_t set_addr;
    uint64_t old_addr;
    sigset_k local_set;

    sigs = get_task_signals();
    if (!sigs) return -ESRCH;

    set_addr = (uint64_t)(uintptr_t)set_ptr;
    old_addr = (uint64_t)oldset_ptr;

    if (old_addr && old_addr < KERNEL_VMA && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sigs->blocked, sizeof(sigset_k));
    }

    if (set_addr && set_addr < KERNEL_VMA && set_addr >= 0x1000) {
        memcpy(&local_set, (const void *)set_addr, sizeof(sigset_k));

        local_set.sig[0] &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

        switch (how) {
            case SIG_BLOCK:
                sigs->blocked.sig[0] |= local_set.sig[0];
                sigs->blocked.sig[1] |= local_set.sig[1];
                break;
            case SIG_UNBLOCK:
                sigs->blocked.sig[0] &= ~local_set.sig[0];
                sigs->blocked.sig[1] &= ~local_set.sig[1];
                break;
            case SIG_SETMASK:
                sigs->blocked.sig[0] = local_set.sig[0];
                sigs->blocked.sig[1] = local_set.sig[1];
                break;
            default:
                return -EINVAL;
        }
    }

    return 0;
}

static int sys_rt_sigpending(int set_ptr, const char *sigsetsize_ptr, int unused) {
    task_signals_t *sigs;
    uint64_t addr;

    (void)sigsetsize_ptr; (void)unused;
    
    sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    addr = (uint64_t)set_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    memcpy((void *)addr, &sigs->pending, sizeof(sigset_k));
    return 0;
}

static int sys_rt_sigsuspend(int mask_ptr, const char *sigsetsize_ptr, int unused) {
    task_signals_t *sigs;
    uint64_t addr;
    sigset_k old_mask;

    (void)sigsetsize_ptr; (void)unused;
    
    sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    addr = (uint64_t)mask_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    memcpy(&old_mask, &sigs->blocked, sizeof(sigset_k));
    
    memcpy(&sigs->blocked, (void *)addr, sizeof(sigset_k));
    sigs->blocked.sig[0] &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
    
    schedule();
    
    memcpy(&sigs->blocked, &old_mask, sizeof(sigset_k));
    
    return -EINTR;
}

static int sys_rt_sigreturn(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    task_signals_t *sigs;
    registers_t *regs;
    uint64_t frame_addr;
    uint64_t *frame;

    sigs = get_task_signals();
    if (sigs) {
        sigs->in_signal = 0;
    }

    regs = current_task ? current_task->syscall_frame : NULL;
    if (!regs) return 0;

    frame_addr = regs->rsp;
    if (frame_addr < 0x1000 || frame_addr >= KERNEL_VMA) return 0;

    frame = (uint64_t *)frame_addr;

    regs->rax    = frame[0];
    regs->rcx    = frame[1];
    regs->rdx    = frame[2];
    regs->rbx    = frame[3];
    regs->rbp    = frame[4];
    regs->rsi    = frame[5];
    regs->rdi    = frame[6];
    regs->rip    = frame[7];
    regs->rflags = frame[8];
    regs->rsp = frame[9];

    return (int)regs->rax;
}

static int sys_sigreturn(int unused1, const char *unused2, int unused3) {
    return sys_rt_sigreturn(unused1, unused2, unused3);
}

static int sys_rt_sigtimedwait(int set_ptr, const char *info_ptr, int timeout_ptr) {
    (void)set_ptr; (void)info_ptr; (void)timeout_ptr;
    return -EAGAIN;
}

static int sys_rt_sigqueueinfo(int pid, const char *sig_ptr, int info_ptr) {
    (void)pid; (void)sig_ptr; (void)info_ptr;
    return 0;
}

int deliver_signal_to_task(task_t *target, int sig) {
    task_signals_t *sigs;
    sigaction_k *act;

    if (!target) return -ESRCH;

    sigs = (task_signals_t *)target->signal_data;
    if (!sigs) {
        sigs = (task_signals_t *)kmalloc(sizeof(task_signals_t));
        if (!sigs) return -ENOMEM;
        init_signal_slot(sigs, target->pid);
        target->signal_data = sigs;
    }

    if (target->pid == 1) {
        act = &sigs->actions[sig];
        if (act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN)
            return 0;
    }

    if (sig == SIGKILL) {
        task_kill(target, 128 + SIGKILL);
        return 0;
    }

    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        target->state = TASK_BLOCKED;
        return 0;
    }

    if (sig == SIGCONT) {
        if (target->state == TASK_BLOCKED) {
            target->state = TASK_READY;
        }
        return 0;
    }

    act = &sigs->actions[sig];
    if (act->sa_handler == SIG_DFL) {
        switch (sig) {
            case SIGCHLD:
            case SIGURG:
            case SIGWINCH:
                break;
            default:
                if (target != current_task) {
                    task_kill(target, 128 + sig);
                } else {
                    task_exit_deferred(128 + sig);
                }
                return 0;
        }
    }

    sigs->pending.sig[(sig - 1) / 64] |= (1UL << ((sig - 1) % 64));

    if (target->state == TASK_BLOCKED) {
        wake_task(target);
    }

    return 0;
}

int collect_pids_in_pgrp(pid_t pgid, pid_t *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    if (pgid <= 0) return 0;

    int count = 0;
    int guard = 0;
    task_t *t;
    uint64_t a;
    pid_t t_pgid;

    lock_scheduler();
    t = all_tasks_head;
    while (t && guard < 4096) {
        a = (uint64_t)t;
        if (a < KERNEL_VMA) break;
        if ((a & 0xFFFF0000u) == 0xFEFE0000u) break;

        t_pgid = t->pgid ? t->pgid : t->pid;
        if (t_pgid == pgid) {
            if (count < out_cap) {
                out[count++] = t->pid;
            }
        }

        t = t->all_next;
        guard++;
    }
    unlock_scheduler();
    return count;
}

int sys_kill_impl(int pid, const char *sig_ptr, int unused) {
    (void)unused;
    int sig = (int)(uintptr_t)sig_ptr;
    
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    
    if (sig == 0) {
        if (pid > 0) {
            task_t *t = task_find((pid_t)pid);
            return t ? 0 : -ESRCH;
        }
        if (!current_task) return -ESRCH;
        pid_t pgid = 0;
        if (pid == 0) pgid = current_task->pgid ? current_task->pgid : current_task->pid;
        else pgid = (pid_t)(-pid);
        pid_t tmp[1];
        return collect_pids_in_pgrp(pgid, tmp, 1) > 0 ? 0 : -ESRCH;
    }

    if (pid > 0) {
        task_t *target = task_find((pid_t)pid);
        if (!target) return -ESRCH;
        if (target->is_kernel_task) return -EPERM;
        if (current_task && current_task->uid != 0 &&
            current_task->uid != target->uid &&
            current_task->euid != target->uid) return -EPERM;
        return deliver_signal_to_task(target, sig);
    }

    if (!current_task) return -ESRCH;

    if (pid == -1) {
        int sent = 0;
        pid_t self_pid = current_task->pid;
        uint64_t my_uid = current_task->uid;
        uint64_t my_euid = current_task->euid;
        int guard = 0;
        lock_scheduler();
        task_t *t = all_tasks_head;
        while (t && guard < 4096) {
            uint64_t a = (uint64_t)t;
            if (a < KERNEL_VMA) break;
            if ((a & 0xFFFF0000u) == 0xFEFE0000u) break;
            if (t->is_user && t->pid != self_pid && t->pid != 1 &&
                (my_uid == 0 || my_uid == t->uid || my_euid == t->uid)) {
                deliver_signal_to_task(t, sig);
                sent++;
            }
            t = t->all_next;
            guard++;
        }
        unlock_scheduler();
        return sent > 0 ? 0 : -ESRCH;
    }

    {
        pid_t pgid = 0;
        pid_t pids[256];
        int n;
        int i;

        if (pid == 0) {
            pgid = current_task->pgid ? current_task->pgid : current_task->pid;
        } else {
            pgid = (pid_t)(-pid);
        }
        if (pgid <= 0) return -EINVAL;

        n = collect_pids_in_pgrp(pgid, pids, 256);
        if (n <= 0) return -ESRCH;

        for (i = 0; i < n; i++) {
            task_t *t = task_find(pids[i]);
            if (t) {
                deliver_signal_to_task(t, sig);
            }
        }
    }

    return 0;
}

static int sys_tgkill(int tgid, const char *tid_ptr, int sig) {
    int tid;

    (void)tgid;
    tid = (int)(uintptr_t)tid_ptr;
    return sys_kill_impl(tid, (const char *)(uintptr_t)sig, 0);
}

static int sys_tkill(int tid, const char *sig_ptr, int unused) {
    return sys_kill_impl(tid, sig_ptr, unused);
}

static int sys_sigaltstack(int ss_ptr, const char *old_ss_ptr, int unused) {
    task_signals_t *sigs;
    uint64_t old_addr;
    uint64_t new_addr;

    (void)unused;
    
    sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    old_addr = (uint64_t)(uintptr_t)old_ss_ptr;
    new_addr = (uint64_t)ss_ptr;
    
    if (old_addr && old_addr < KERNEL_VMA && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sigs->altstack, sizeof(stack_k));
    }
    
    if (new_addr && new_addr < KERNEL_VMA && new_addr >= 0x1000) {
        memcpy(&sigs->altstack, (void *)new_addr, sizeof(stack_k));
    }
    
    return 0;
}

static int sys_pause(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    schedule();
    
    return -EINTR;
}

static int sys_alarm(int seconds, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    (void)seconds;
    return 0;
}

void signal_deliver_pending(registers_t *regs) {
    task_signals_t *sigs;
    int sig;
    sigaction_k *act;
    uint64_t sp;
    uint64_t *frame;

    if (!current_task || !regs) return;
    if (!current_task->is_user) return;

    sigs = get_task_signals();
    if (!sigs || sigs->in_signal) return;

    for (sig = 1; sig < 32; sig++) {
        if (!(sigs->pending.sig[0] & (1UL << (sig - 1)))) continue;
        if (sigs->blocked.sig[0] & (1UL << (sig - 1))) continue;

        sigs->pending.sig[0] &= ~(1UL << (sig - 1));
        act = &sigs->actions[sig];

        if (act->sa_handler == SIG_IGN) continue;

        if (act->sa_handler == SIG_DFL) {
            switch (sig) {
                case SIGCHLD:
                case SIGURG:
                case SIGWINCH:
                    continue;
                case SIGSTOP:
                case SIGTSTP:
                case SIGTTIN:
                case SIGTTOU:
                    current_task->state = TASK_BLOCKED;
                    return;
                case SIGCONT:
                    continue;
                default:
                    task_exit_deferred(128 + sig);
                    return;
            }
        }

        sp = regs->rsp;
        sp -= 10 * 4;
        sp &= ~0xFu;

        if (sp < 0x1000 || sp >= KERNEL_VMA) {
            task_exit_deferred(128 + sig);
            return;
        }

        frame = (uint64_t *)sp;
        frame[0] = regs->rax;
        frame[1] = regs->rcx;
        frame[2] = regs->rdx;
        frame[3] = regs->rbx;
        frame[4] = regs->rbp;
        frame[5] = regs->rsi;
        frame[6] = regs->rdi;
        frame[7] = regs->rip;
        frame[8] = regs->rflags;
        frame[9] = regs->rsp;

        sp -= 4;
        *(uint64_t *)sp = (uint64_t)sig;

        sp -= 4;
        if (act->sa_restorer) {
            *(uint64_t *)sp = (uint64_t)(uintptr_t)act->sa_restorer;
        } else {
            *(uint64_t *)sp = 0;
        }

        regs->rip = (uint64_t)(uintptr_t)act->sa_handler;
        regs->rsp = sp;

        sigs->in_signal = 1;

        if (act->sa_flags & SA_RESETHAND) {
            act->sa_handler = SIG_DFL;
        }

        return;
    }
}

void signals_init_task(task_t *task) {
    task_signals_t *slot;

    if (!task) return;
    if (task->signal_data) {
        slot = (task_signals_t *)task->signal_data;
    } else {
        slot = (task_signals_t *)kmalloc(sizeof(task_signals_t));
        if (!slot) return;
        task->signal_data = slot;
    }
    init_signal_slot(slot, task->pid);
}

void syscalls_signal_init(void) {
    syscall_table[SYSCALL_SIGACTION] = sys_rt_sigaction;
    syscall_table[SYSCALL_RT_SIGACTION] = sys_rt_sigaction;
    syscall_table[SYSCALL_SIGPROCMASK] = sys_rt_sigprocmask;
    syscall_table[SYSCALL_RT_SIGPROCMASK] = sys_rt_sigprocmask;
    syscall_table[SYSCALL_RT_SIGPENDING] = sys_rt_sigpending;
    syscall_table[SYSCALL_RT_SIGSUSPEND] = sys_rt_sigsuspend;
    syscall_table[SYSCALL_RT_SIGRETURN] = sys_rt_sigreturn;
    syscall_table[SYSCALL_RT_SIGTIMEDWAIT] = sys_rt_sigtimedwait;
    syscall_table[SYSCALL_RT_SIGQUEUEINFO] = sys_rt_sigqueueinfo;
    syscall_table[SYSCALL_TGKILL] = sys_tgkill;
    syscall_table[SYSCALL_TKILL] = sys_tkill;
    syscall_table[SYSCALL_SIGALTSTACK] = sys_sigaltstack;
    syscall_table[SYSCALL_PAUSE] = sys_pause;
    syscall_table[SYSCALL_ALARM] = sys_alarm;
    syscall_table[SYSCALL_SIGRETURN] = sys_sigreturn;
}

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

#define NSIG      64
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
    sigset_k pending;
    sigset_k blocked;
    sigaction_k actions[NSIG];
    stack_k altstack;
    int in_signal;
} task_signals_t;

static task_signals_t task_signals[256];

static task_signals_t *get_task_signals(void) {
    if (!current_task) return NULL;
    uint32_t idx = ((uint32_t)current_task->pid) & 255u;
    return &task_signals[idx];
}

static int sys_rt_sigaction(int signum, const char *act_ptr, int oldact_ptr) {
    if (signum < 1 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;
    
    task_signals_t *sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    uint32_t act_addr = (uint32_t)(uintptr_t)act_ptr;
    uint32_t old_addr = (uint32_t)oldact_ptr;
    
    if (old_addr && old_addr < 0xC0000000 && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sigs->actions[signum], sizeof(sigaction_k));
    }
    
    if (act_addr && act_addr < 0xC0000000 && act_addr >= 0x1000) {
        memcpy(&sigs->actions[signum], (void *)act_addr, sizeof(sigaction_k));
    }
    
    return 0;
}

static int sys_rt_sigprocmask(int how, const char *set_ptr, int oldset_ptr) {
    task_signals_t *sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    uint32_t set_addr = (uint32_t)(uintptr_t)set_ptr;
    uint32_t old_addr = (uint32_t)oldset_ptr;
    
    if (old_addr && old_addr < 0xC0000000 && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sigs->blocked, sizeof(sigset_k));
    }
    
    if (set_addr && set_addr < 0xC0000000 && set_addr >= 0x1000) {
        sigset_k *new_set = (sigset_k *)set_addr;
        
        new_set->sig[0] &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
        
        switch (how) {
            case SIG_BLOCK:
                sigs->blocked.sig[0] |= new_set->sig[0];
                sigs->blocked.sig[1] |= new_set->sig[1];
                break;
            case SIG_UNBLOCK:
                sigs->blocked.sig[0] &= ~new_set->sig[0];
                sigs->blocked.sig[1] &= ~new_set->sig[1];
                break;
            case SIG_SETMASK:
                sigs->blocked.sig[0] = new_set->sig[0];
                sigs->blocked.sig[1] = new_set->sig[1];
                break;
            default:
                return -EINVAL;
        }
    }
    
    return 0;
}

static int sys_rt_sigpending(int set_ptr, const char *sigsetsize_ptr, int unused) {
    (void)sigsetsize_ptr; (void)unused;
    
    task_signals_t *sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    uint32_t addr = (uint32_t)set_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    memcpy((void *)addr, &sigs->pending, sizeof(sigset_k));
    return 0;
}

static int sys_rt_sigsuspend(int mask_ptr, const char *sigsetsize_ptr, int unused) {
    (void)sigsetsize_ptr; (void)unused;
    
    task_signals_t *sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    uint32_t addr = (uint32_t)mask_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    sigset_k old_mask;
    memcpy(&old_mask, &sigs->blocked, sizeof(sigset_k));
    
    memcpy(&sigs->blocked, (void *)addr, sizeof(sigset_k));
    sigs->blocked.sig[0] &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
    
    schedule();
    
    memcpy(&sigs->blocked, &old_mask, sizeof(sigset_k));
    
    return -EINTR;
}

static int sys_rt_sigreturn(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    
    task_signals_t *sigs = get_task_signals();
    if (sigs) {
        sigs->in_signal = 0;
    }
    
    if (current_task && current_task->syscall_frame) {
        return 0;
    }
    
    return 0;
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

static int deliver_signal_to_task(task_t *target, int sig) {
    if (!target) return -ESRCH;

    if (sig == SIGKILL) {
        task_kill(target, 128 + SIGKILL);
        return 0;
    }

    if (sig == SIGTERM) {
        task_kill(target, 128 + SIGTERM);
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

    uint32_t idx = ((uint32_t)target->pid) & 255u;
    task_signals[idx].pending.sig[sig / 64] |= (1UL << (sig % 64));
    return 0;
}

static int collect_pids_in_pgrp(pid_t pgid, pid_t *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    if (pgid <= 0) return 0;

    int count = 0;
    lock_scheduler();
    task_t *t = ready_queue_head;
    if (t) {
        int guard = 0;
        do {
            if (!t) break;
            uint32_t a = (uint32_t)t;
            if (a < 0xC0000000u) break;
            if ((a & 0xFFFF0000u) == 0xFEFE0000u) break;

            pid_t t_pgid = t->pgid ? t->pgid : t->pid;
            if (t_pgid == pgid) {
                if (count < out_cap) {
                    out[count++] = t->pid;
                }
            }

            t = t->next;
            guard++;
        } while (t && t != ready_queue_head && guard < 4096);
    }
    unlock_scheduler();
    return count;
}

static int sys_kill_impl(int pid, const char *sig_ptr, int unused) {
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
        return deliver_signal_to_task(target, sig);
    }

    if (!current_task) return -ESRCH;

    pid_t pgid = 0;
    if (pid == 0) {
        pgid = current_task->pgid ? current_task->pgid : current_task->pid;
    } else {
        pgid = (pid_t)(-pid);
    }
    if (pgid <= 0) return -EINVAL;

    pid_t pids[256];
    int n = collect_pids_in_pgrp(pgid, pids, 256);
    if (n <= 0) return -ESRCH;

    for (int i = 0; i < n; i++) {
        task_t *t = task_find(pids[i]);
        if (t) {
            deliver_signal_to_task(t, sig);
        }
    }

    return 0;
}

static int sys_tgkill(int tgid, const char *tid_ptr, int sig) {
    (void)tgid;
    int tid = (int)(uintptr_t)tid_ptr;
    return sys_kill_impl(tid, (const char *)(uintptr_t)sig, 0);
}

static int sys_tkill(int tid, const char *sig_ptr, int unused) {
    return sys_kill_impl(tid, sig_ptr, unused);
}

static int sys_sigaltstack(int ss_ptr, const char *old_ss_ptr, int unused) {
    (void)unused;
    
    task_signals_t *sigs = get_task_signals();
    if (!sigs) return -ESRCH;
    
    uint32_t old_addr = (uint32_t)(uintptr_t)old_ss_ptr;
    uint32_t new_addr = (uint32_t)ss_ptr;
    
    if (old_addr && old_addr < 0xC0000000 && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sigs->altstack, sizeof(stack_k));
    }
    
    if (new_addr && new_addr < 0xC0000000 && new_addr >= 0x1000) {
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

void signal_deliver_pending(void) {
    if (!current_task) return;
    
    task_signals_t *sigs = get_task_signals();
    if (!sigs || sigs->in_signal) return;
    
    for (int sig = 1; sig < 32; sig++) {
        if (sigs->pending.sig[0] & (1UL << (sig - 1))) {
            if (sigs->blocked.sig[0] & (1UL << (sig - 1))) {
                continue;
            }
            
            sigs->pending.sig[0] &= ~(1UL << (sig - 1));
            
            sigaction_k *act = &sigs->actions[sig];
            
            if (act->sa_handler == SIG_IGN) {
                continue;
            }
            
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
                        schedule();
                        continue;
                    case SIGCONT:
                        continue;
                    default:
                        task_exit_deferred(128 + sig);
                        schedule();
                        for (;;) asm volatile ("hlt");
                        return;
                }
            }
            
            sigs->in_signal = 1;
            break;
        }
    }
}

void signals_init_task(pid_t pid) {
    uint32_t idx = ((uint32_t)pid) & 255u;
    memset(&task_signals[idx], 0, sizeof(task_signals_t));
    
    for (int i = 0; i < NSIG; i++) {
        task_signals[idx].actions[i].sa_handler = SIG_DFL;
    }
}

void syscalls_signal_init(void) {
    memset(task_signals, 0, sizeof(task_signals));
    
    syscall_table[SYSCALL_RT_SIGACTION] = sys_rt_sigaction;
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

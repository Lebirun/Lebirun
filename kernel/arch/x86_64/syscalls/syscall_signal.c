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

#define signal_user_range_mapped(addr, size) \
    syscall_user_range_present((addr), (size), 0, 1)
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define NSIG      65
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
    unsigned long sig[1];
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
    unsigned int sa_mask[2];
} sigaction_k;

typedef struct {
    int signum;
    sigaction_k action;
} signal_action_entry_t;

typedef struct queued_signal {
    int signum;
    uint8_t info[128];
    struct queued_signal *next;
} queued_signal_t;

typedef struct {
    pid_t owner_pid;
    uint8_t action_count;
    uint8_t action_capacity;
    sigset_k pending;
    sigset_k blocked;
    signal_action_entry_t *actions;
    stack_k altstack;
    int in_signal;
    queued_signal_t *queue_head;
    queued_signal_t *queue_tail;
    uint32_t queue_count;
} task_signals_t;

static void default_signal_action(sigaction_k *act);
int deliver_signal_to_task(task_t *target, int sig);
extern uint64_t pit_freq;

static void init_signal_slot(task_signals_t *slot, pid_t pid) {
    memset(slot, 0, sizeof(task_signals_t));
    slot->owner_pid = pid;
}

static void free_signal_actions(task_signals_t *slot) {
    if (!slot || !slot->actions) return;
    kfree(slot->actions);
    slot->actions = NULL;
    slot->action_count = 0;
    slot->action_capacity = 0;
}

static void free_signal_queue(task_signals_t *slot) {
    queued_signal_t *entry;
    queued_signal_t *next;

    if (!slot) return;
    entry = slot->queue_head;
    while (entry) {
        next = entry->next;
        kfree(entry);
        entry = next;
    }
    slot->queue_head = NULL;
    slot->queue_tail = NULL;
    slot->queue_count = 0;
}

static int find_signal_action(task_signals_t *slot, int signum) {
    int i;

    if (!slot || !slot->actions) return -1;
    for (i = 0; i < slot->action_count; i++) {
        if (slot->actions[i].signum == signum) return i;
    }
    return -1;
}

static void shrink_signal_actions(task_signals_t *slot) {
    int new_capacity;
    signal_action_entry_t *new_actions;

    if (!slot || !slot->actions) return;
    if (slot->action_count == 0) {
        free_signal_actions(slot);
        return;
    }
    if (slot->action_capacity <= 1 ||
        slot->action_count > slot->action_capacity / 4) return;
    new_capacity = slot->action_capacity / 2;
    if (new_capacity < 1) new_capacity = 1;
    new_actions = (signal_action_entry_t *)krealloc(
        slot->actions, new_capacity * sizeof(signal_action_entry_t));
    if (!new_actions) return;
    slot->actions = new_actions;
    slot->action_capacity = new_capacity;
}

static int signal_action_is_default(sigaction_k *act) {
    if (!act) return 1;
    return act->sa_handler == SIG_DFL && act->sa_flags == 0 &&
        act->sa_restorer == NULL && act->sa_mask[0] == 0 &&
        act->sa_mask[1] == 0;
}

static sigaction_k *ensure_signal_action(task_signals_t *slot, int signum) {
    int idx;
    int new_capacity;
    signal_action_entry_t *new_actions;

    if (!slot) return NULL;
    idx = find_signal_action(slot, signum);
    if (idx >= 0) return &slot->actions[idx].action;
    if (slot->action_count >= slot->action_capacity) {
        new_capacity = slot->action_capacity ? slot->action_capacity * 2 : 1;
        if (new_capacity > NSIG - 1) new_capacity = NSIG - 1;
        if (new_capacity <= slot->action_capacity) return NULL;
        new_actions = (signal_action_entry_t *)krealloc(
            slot->actions, new_capacity * sizeof(signal_action_entry_t));
        if (!new_actions) return NULL;
        slot->actions = new_actions;
        slot->action_capacity = new_capacity;
    }
    idx = slot->action_count;
    slot->actions[idx].signum = signum;
    default_signal_action(&slot->actions[idx].action);
    slot->action_count++;
    return &slot->actions[idx].action;
}

static sigaction_k *get_signal_action(task_signals_t *slot, int signum, sigaction_k *default_act) {
    int idx;

    idx = find_signal_action(slot, signum);
    if (idx >= 0) return &slot->actions[idx].action;
    default_signal_action(default_act);
    return default_act;
}

static int set_signal_action(task_signals_t *slot, int signum, sigaction_k *act) {
    int idx;
    sigaction_k *action;

    if (!slot || !act) return -1;
    if (signal_action_is_default(act)) {
        idx = find_signal_action(slot, signum);
        if (idx >= 0) {
            if (idx + 1 < slot->action_count) {
                memmove(&slot->actions[idx], &slot->actions[idx + 1],
                        (slot->action_count - idx - 1) *
                        sizeof(signal_action_entry_t));
            }
            slot->action_count--;
            shrink_signal_actions(slot);
        }
        return 0;
    }
    action = ensure_signal_action(slot, signum);
    if (!action) return -1;
    memcpy(action, act, sizeof(sigaction_k));
    return 0;
}

static void default_signal_action(sigaction_k *act) {
    memset(act, 0, sizeof(sigaction_k));
    act->sa_handler = SIG_DFL;
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
        free_signal_actions(slot);
        free_signal_queue(slot);
        init_signal_slot(slot, current_task->pid);
    }
    return slot;
}

static task_signals_t *ensure_task_signals(task_t *task) {
    task_signals_t *slot;

    if (!task) return NULL;
    if (task == current_task) return get_task_signals();
    slot = (task_signals_t *)task->signal_data;
    if (slot && slot->owner_pid == task->pid) return slot;
    if (slot) return NULL;
    slot = (task_signals_t *)kmalloc(sizeof(task_signals_t));
    if (!slot) return NULL;
    init_signal_slot(slot, task->pid);
    task->signal_data = slot;
    return slot;
}

static int dequeue_queued_signal(task_signals_t *sigs, uint64_t mask,
                                 uint8_t *info) {
    queued_signal_t **link;
    queued_signal_t *entry;
    queued_signal_t *scan;
    int still_pending;

    if (!sigs) return 0;
    link = &sigs->queue_head;
    while (*link) {
        entry = *link;
        if (mask & (1ULL << (entry->signum - 1))) {
            *link = entry->next;
            if (sigs->queue_tail == entry) {
                sigs->queue_tail = NULL;
                scan = sigs->queue_head;
                while (scan && scan->next) scan = scan->next;
                sigs->queue_tail = scan;
            }
            if (sigs->queue_count) sigs->queue_count--;
            still_pending = 0;
            scan = sigs->queue_head;
            while (scan) {
                if (scan->signum == entry->signum) {
                    still_pending = 1;
                    break;
                }
                scan = scan->next;
            }
            if (!still_pending)
                __atomic_fetch_and(&sigs->pending.sig[0],
                                   ~(1ULL << (entry->signum - 1)),
                                   __ATOMIC_ACQ_REL);
            if (info) memcpy(info, entry->info, sizeof(entry->info));
            still_pending = entry->signum;
            kfree(entry);
            return still_pending;
        }
        link = &entry->next;
    }
    return 0;
}

void task_free_signal_data(task_t *task) {
    task_signals_t *sigs;

    if (!task || !task->signal_data) return;
    sigs = (task_signals_t *)task->signal_data;
    free_signal_actions(sigs);
    free_signal_queue(sigs);
    kfree(sigs);
    task->signal_data = NULL;
}

void task_reset_signals_on_exec(void) {
    task_signals_t *sigs;
    sigaction_k default_act;
    signal_action_entry_t *entry;
    int i;
    int signum;

    if (!current_task) return;
    sigs = (task_signals_t *)current_task->signal_data;
    if (!sigs) return;
    if (sigs->owner_pid != current_task->pid) {
        free_signal_actions(sigs);
        free_signal_queue(sigs);
        init_signal_slot(sigs, current_task->pid);
        return;
    }

    __atomic_store_n(&sigs->pending.sig[0], 0, __ATOMIC_RELEASE);
    sigs->blocked.sig[0] = 0;
    sigs->in_signal = 0;
    free_signal_queue(sigs);

    default_signal_action(&default_act);
    for (i = sigs->action_count - 1; i >= 0; i--) {
        entry = &sigs->actions[i];
        signum = entry->signum;
        if (entry->action.sa_handler != SIG_IGN ||
                signum == SIGINT || signum == SIGQUIT ||
                signum == SIGTSTP || signum == SIGTTIN ||
                signum == SIGTTOU) {
            set_signal_action(sigs, signum, &default_act);
            continue;
        }
        entry->action.sa_flags = 0;
        entry->action.sa_restorer = NULL;
        entry->action.sa_mask[0] = 0;
        entry->action.sa_mask[1] = 0;
    }
    if (sigs->action_count == 0 && sigs->altstack.ss_sp == NULL &&
        sigs->altstack.ss_flags == 0 && sigs->altstack.ss_size == 0) {
        kfree(sigs);
        current_task->signal_data = NULL;
    }
}

int task_has_pending_signals(void) {
    task_signals_t *sigs;
    unsigned long unblocked;

    if (!current_task) return 0;
    sigs = (task_signals_t *)current_task->signal_data;
    if (!sigs) return 0;
    if (sigs->owner_pid != current_task->pid) return 0;
    unblocked = __atomic_load_n(&sigs->pending.sig[0], __ATOMIC_ACQUIRE) &
        ~sigs->blocked.sig[0];
    return unblocked != 0;
}

uint64_t signal_pending_mask(task_t *task) {
    task_signals_t *sigs;

    if (!task || !task->signal_data) return 0;
    sigs = (task_signals_t *)task->signal_data;
    if (sigs->owner_pid != task->pid) return 0;
    return __atomic_load_n(&sigs->pending.sig[0], __ATOMIC_ACQUIRE);
}

uint64_t signal_blocked_mask(task_t *task) {
    task_signals_t *sigs;

    if (!task || !task->signal_data) return 0;
    sigs = (task_signals_t *)task->signal_data;
    if (sigs->owner_pid != task->pid) return 0;
    return sigs->blocked.sig[0];
}

int signal_debug_in_handler(task_t *task) {
    task_signals_t *sigs;

    if (!task || !task->signal_data) return 0;
    sigs = (task_signals_t *)task->signal_data;
    if (sigs->owner_pid != task->pid) return 0;
    return sigs->in_signal;
}

uint32_t signal_queue_count(task_t *task) {
    task_signals_t *sigs;

    if (!task || !task->signal_data) return 0;
    sigs = (task_signals_t *)task->signal_data;
    if (sigs->owner_pid != task->pid) return 0;
    return sigs->queue_count;
}

int signal_take_pending(task_t *task, uint64_t mask) {
    task_signals_t *sigs;
    uint64_t bit;
    uint64_t previous;
    int signal_number;

    if (!task || !task->signal_data) return 0;
    sigs = (task_signals_t *)task->signal_data;
    if (sigs->owner_pid != task->pid) return 0;
    for (signal_number = 1; signal_number < 64; signal_number++) {
        bit = 1ULL << (signal_number - 1);
        if (!(mask & bit)) continue;
        previous = __atomic_fetch_and(&sigs->pending.sig[0], ~bit,
                                      __ATOMIC_ACQ_REL);
        if (!(previous & bit)) continue;
        return signal_number;
    }
    return 0;
}

static int sys_rt_sigaction(int signum, const char *act_ptr,
                            uint64_t oldact_ptr) {
    task_signals_t *sigs;
    uint64_t act_addr;
    uint64_t old_addr;
    sigaction_k local_act;
    sigaction_k default_act;
    sigaction_k *old_action;

    if (signum < 1 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;

    act_addr = (uint64_t)(uintptr_t)act_ptr;
    old_addr = (uint64_t)oldact_ptr;

    default_signal_action(&default_act);
    sigs = (current_task ? (task_signals_t *)current_task->signal_data : NULL);

    if (old_addr && old_addr < KERNEL_VMA && old_addr >= 0x1000) {
        old_action = &default_act;
        if (sigs && sigs->owner_pid == current_task->pid)
            old_action = get_signal_action(sigs, signum, &default_act);
        memcpy((void *)old_addr, old_action, sizeof(sigaction_k));
    }

    if (act_addr && act_addr < KERNEL_VMA && act_addr >= 0x1000) {
        memcpy(&local_act, (void *)act_addr, sizeof(sigaction_k));
        if (local_act.sa_handler == SIG_DFL && local_act.sa_flags == 0 &&
            local_act.sa_restorer == NULL && local_act.sa_mask[0] == 0 &&
            local_act.sa_mask[1] == 0 && (!sigs || sigs->owner_pid != current_task->pid)) {
            return 0;
        }
        if (!sigs || sigs->owner_pid != current_task->pid) {
            sigs = get_task_signals();
            if (!sigs) return -ESRCH;
        }
        if (set_signal_action(sigs, signum, &local_act) != 0) return -ESRCH;
    }

    return 0;
}

static int sys_rt_sigprocmask(int how, const char *set_ptr,
                              uint64_t oldset_ptr) {
    task_signals_t *sigs;
    uint64_t set_addr;
    uint64_t old_addr;
    sigset_k local_set;
    sigset_k empty_set;

    set_addr = (uint64_t)(uintptr_t)set_ptr;
    old_addr = (uint64_t)oldset_ptr;
    sigs = (current_task ? (task_signals_t *)current_task->signal_data : NULL);
    memset(&empty_set, 0, sizeof(empty_set));

    if (old_addr && old_addr < KERNEL_VMA && old_addr >= 0x1000) {
        if (sigs && sigs->owner_pid == current_task->pid) {
            memcpy((void *)old_addr, &sigs->blocked, sizeof(sigset_k));
        } else {
            memcpy((void *)old_addr, &empty_set, sizeof(sigset_k));
        }
    }

    if (set_addr && set_addr < KERNEL_VMA && set_addr >= 0x1000) {
        memcpy(&local_set, (const void *)set_addr, sizeof(sigset_k));

        local_set.sig[0] &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

        if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) return -EINVAL;
        if ((!sigs || sigs->owner_pid != current_task->pid) &&
            (local_set.sig[0] == 0 || how == SIG_UNBLOCK)) {
            return 0;
        }
        if (!sigs || sigs->owner_pid != current_task->pid) {
            sigs = get_task_signals();
            if (!sigs) return -ESRCH;
        }

        switch (how) {
            case SIG_BLOCK:
                sigs->blocked.sig[0] |= local_set.sig[0];
                break;
            case SIG_UNBLOCK:
                sigs->blocked.sig[0] &= ~local_set.sig[0];
                break;
            case SIG_SETMASK:
                sigs->blocked.sig[0] = local_set.sig[0];
                break;
            default:
                return -EINVAL;
        }
    }

    return 0;
}

static int sys_rt_sigpending(uint64_t set_ptr, const char *sigsetsize_ptr,
                             int unused) {
    task_signals_t *sigs;
    uint64_t addr;
    sigset_k pending_set;

    (void)sigsetsize_ptr; (void)unused;

    addr = (uint64_t)set_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    sigs = (current_task ? (task_signals_t *)current_task->signal_data : NULL);
    memset(&pending_set, 0, sizeof(pending_set));
    if (sigs && sigs->owner_pid == current_task->pid) {
        pending_set.sig[0] = __atomic_load_n(&sigs->pending.sig[0],
                                             __ATOMIC_ACQUIRE);
    }
    memcpy((void *)addr, &pending_set, sizeof(sigset_k));
    return 0;
}

static int sys_rt_sigsuspend(uint64_t mask_ptr, const char *sigsetsize_ptr,
                             int unused) {
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
    task_signals_t *sigs;
    registers_t *regs;
    uint64_t frame_addr;
    uint64_t *frame;
    uint64_t saved_rip;
    uint64_t saved_rsp;
    uint64_t saved_rflags;

    (void)unused1; (void)unused2; (void)unused3;

    if (current_task && tty_vt_debug_owner(current_task->pid))
        vt_debug_printf("[VTDBG SIGNAL] return-begin pid=%d\n", current_task->pid);
    sigs = (current_task ? (task_signals_t *)current_task->signal_data : NULL);
    if (sigs && sigs->owner_pid == current_task->pid) {
        sigs->in_signal = 0;
    }

    regs = current_task ? current_task->syscall_frame : NULL;
    if (!regs) return 0;

    frame_addr = regs->rsp;
    if (!signal_user_range_mapped(frame_addr, 18 * sizeof(uint64_t)))
        return -EFAULT;

    frame = (uint64_t *)frame_addr;
    saved_rip = frame[7];
    saved_rsp = frame[9];
    if (saved_rip < 0x1000 || saved_rip >= KERNEL_VMA ||
        saved_rsp < 0x1000 || saved_rsp >= KERNEL_VMA)
        return -EFAULT;
    saved_rflags = (frame[8] & 0x0000000000254FD5ULL) | 0x202ULL;

    regs->rax    = frame[0];
    regs->rcx    = frame[1];
    regs->rdx    = frame[2];
    regs->rbx    = frame[3];
    regs->rbp    = frame[4];
    regs->rsi    = frame[5];
    regs->rdi    = frame[6];
    regs->rip    = saved_rip;
    regs->rflags = saved_rflags;
    regs->rsp    = saved_rsp;
    regs->r8     = frame[10];
    regs->r9     = frame[11];
    regs->r10    = frame[12];
    regs->r11    = frame[13];
    regs->r12    = frame[14];
    regs->r13    = frame[15];
    regs->r14    = frame[16];
    regs->r15    = frame[17];

    if (tty_vt_debug_owner(current_task->pid))
        vt_debug_printf("[VTDBG SIGNAL] return-end pid=%d rip=%llx rsp=%llx\n",
                        current_task->pid, saved_rip, saved_rsp);
    return (int)regs->rax;
}

static int sys_sigreturn(int unused1, const char *unused2, int unused3) {
    return sys_rt_sigreturn(unused1, unused2, unused3);
}

static int sys_rt_sigtimedwait(const sigset_k *set_ptr, void *info_ptr,
                               const struct kernel_timespec *timeout_ptr,
                               size_t sigset_size) {
    sigset_k set;
    struct kernel_timespec timeout;
    task_signals_t *sigs;
    uint8_t info[128];
    uint64_t timeout_ticks;
    uint64_t seconds;
    uint64_t pending;
    uint64_t frequency;
    int signum;

    if (!set_ptr) return -EFAULT;
    if (sigset_size != sizeof(sigset_k)) return -EINVAL;
    if (copy_from_user(&set, set_ptr, sizeof(set)) < 0) return -EFAULT;
    timeout_ticks = UINT64_MAX;
    if (timeout_ptr) {
        if (copy_from_user(&timeout, timeout_ptr, sizeof(timeout)) < 0)
            return -EFAULT;
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 ||
            timeout.tv_nsec >= 1000000000L) return -EINVAL;
        frequency = pit_freq ? pit_freq : 1000;
        seconds = (uint64_t)timeout.tv_sec;
        if (seconds > UINT64_MAX / frequency) return -EINVAL;
        timeout_ticks = seconds * frequency;
        timeout_ticks += ((uint64_t)timeout.tv_nsec * frequency +
                          999999999ULL) / 1000000000ULL;
    }
    sigs = ensure_task_signals(current_task);
    if (!sigs) return -ENOMEM;
    memset(info, 0, sizeof(info));
    signum = dequeue_queued_signal(sigs, set.sig[0], info);
    if (!signum) {
        pending = __atomic_load_n(&sigs->pending.sig[0], __ATOMIC_ACQUIRE) &
            set.sig[0];
        signum = signal_take_pending(current_task, pending);
        if (signum) memcpy(info, &signum, sizeof(signum));
    }
    if (!signum && timeout_ticks != 0) {
        if (timeout_ptr)
            sleep_ticks(timeout_ticks);
        else
            block_current();
        signum = dequeue_queued_signal(sigs, set.sig[0], info);
        if (!signum) {
            pending = __atomic_load_n(&sigs->pending.sig[0],
                                      __ATOMIC_ACQUIRE) & set.sig[0];
            signum = signal_take_pending(current_task, pending);
            if (signum) memcpy(info, &signum, sizeof(signum));
        }
    }
    if (!signum) return -EAGAIN;
    if (info_ptr && copy_to_user(info_ptr, info, sizeof(info)) < 0)
        return -EFAULT;
    return signum;
}

static int sys_rt_sigqueueinfo(int pid, const char *sig_ptr, void *info_ptr) {
    task_t *target;
    task_signals_t *sigs;
    queued_signal_t *entry;
    unsigned long limit;
    int sig;
    int result;

    sig = (int)(uintptr_t)sig_ptr;
    if (sig < 1 || sig >= NSIG) return -EINVAL;
    if (!info_ptr) return -EFAULT;
    target = task_find((pid_t)pid);
    if (!target) return -ESRCH;
    if (current_task && current_task->euid != 0 &&
        current_task->euid != target->uid &&
        current_task->uid != target->uid) return -EPERM;
    sigs = ensure_task_signals(target);
    if (!sigs) return -ENOMEM;
    limit = task_rlimit_get(target, 11, 0);
    if (sigs->queue_count == UINT32_MAX || sigs->queue_count >= limit)
        return -EAGAIN;
    entry = (queued_signal_t *)kmalloc(sizeof(queued_signal_t));
    if (!entry) return -ENOMEM;
    memset(entry, 0, sizeof(*entry));
    if (copy_from_user(entry->info, info_ptr, sizeof(entry->info)) < 0) {
        kfree(entry);
        return -EFAULT;
    }
    entry->signum = sig;
    memcpy(entry->info, &sig, sizeof(sig));
    if (sigs->queue_tail) sigs->queue_tail->next = entry;
    else sigs->queue_head = entry;
    sigs->queue_tail = entry;
    sigs->queue_count++;
    result = deliver_signal_to_task(target, sig);
    if (result != 0) {
        dequeue_queued_signal(sigs, 1ULL << (sig - 1), NULL);
        return result;
    }
    return 0;
}

int deliver_signal_to_task(task_t *target, int sig) {
    task_signals_t *sigs;
    sigaction_k *act;
    sigaction_k default_act;
    int has_sigs;

    if (!target) return -ESRCH;
    if (target->state == TASK_DEAD) return 0;
    if (sig < 1 || sig >= NSIG) return -EINVAL;

    sigs = (task_signals_t *)target->signal_data;
    has_sigs = (sigs && sigs->owner_pid == target->pid);

    if (target->pid == 1) {
        if (!has_sigs) return 0;
        act = get_signal_action(sigs, sig, &default_act);
        if (act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN)
            return 0;
    }

    if (sig == SIGKILL) {
        task_kill(target, 128 + SIGKILL);
        return 0;
    }

    if (sig == SIGCONT) {
        if (target->state == TASK_STOPPED || target->state == TASK_BLOCKED) {
            target->state = TASK_READY;
        }
        return 0;
    }

    if (!has_sigs) {
        switch (sig) {
            case SIGCHLD:
            case SIGURG:
            case SIGWINCH:
                return 0;
            default:
                if (target != current_task) {
                    task_kill(target, 128 + sig);
                } else {
                    task_exit_deferred(128 + sig);
                }
                return 0;
        }
    }

    act = get_signal_action(sigs, sig, &default_act);
    if (act->sa_handler == SIG_IGN) {
        return 0;
    }

    __atomic_fetch_or(&sigs->pending.sig[(sig - 1) / 64],
                      1UL << ((sig - 1) % 64), __ATOMIC_ACQ_REL);

    if (target->state == TASK_BLOCKED) {
        wake_task(target);
        descriptor_ready_notify();
    }

    return 0;
}

static pid_t signal_find_next_pgrp(pid_t pgid, pid_t after) {
    task_t *t;
    uint64_t address;
    pid_t task_pgid;
    pid_t result;

    result = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        address = (uint64_t)t;
        if (address < KERNEL_VMA) break;
        if ((address & 0xFFFF0000u) == 0xFEFE0000u) break;
        task_pgid = t->pgid ? t->pgid : t->pid;
        if (task_pgid == pgid && t->pid > after &&
            (result == 0 || t->pid < result)) {
            result = t->pid;
        }
        t = t->all_next;
    }
    unlock_scheduler();
    return result;
}

int deliver_signal_to_pgrp(pid_t pgid, int sig) {
    task_t *task;
    pid_t last_pid;
    pid_t next_pid;
    int sent;

    if (pgid <= 0) return 0;
    last_pid = 0;
    sent = 0;
    for (;;) {
        next_pid = signal_find_next_pgrp(pgid, last_pid);
        if (next_pid <= 0) break;
        last_pid = next_pid;
        task = task_find(next_pid);
        if (task) {
            deliver_signal_to_task(task, sig);
            sent++;
        }
    }
    return sent;
}

static pid_t signal_find_next_permitted(pid_t after, pid_t self_pid,
                                        uint64_t uid, uint64_t euid) {
    task_t *t;
    uint64_t address;
    pid_t result;

    result = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        address = (uint64_t)t;
        if (address < KERNEL_VMA) break;
        if ((address & 0xFFFF0000u) == 0xFEFE0000u) break;
        if (t->is_user && t->pid != self_pid && t->pid != 1 &&
            t->pid > after &&
            (uid == 0 || uid == t->uid || euid == t->uid) &&
            (result == 0 || t->pid < result)) {
            result = t->pid;
        }
        t = t->all_next;
    }
    unlock_scheduler();
    return result;
}

int sys_kill_impl(int pid, const char *sig_ptr, int unused) {
    int sig;
    int sent;
    task_t *t;
    task_t *target;
    pid_t pgid;
    pid_t self_pid;
    pid_t last_pid;
    pid_t next_pid;
    uint64_t uid;
    uint64_t euid;

    (void)unused;
    sig = (int)(uintptr_t)sig_ptr;
    
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    
    if (sig == 0) {
        if (pid > 0) {
            t = task_find((pid_t)pid);
            return t ? 0 : -ESRCH;
        }
        if (!current_task) return -ESRCH;
        pgid = 0;
        if (pid == 0) pgid = current_task->pgid ? current_task->pgid : current_task->pid;
        else pgid = (pid_t)(-pid);
        return signal_find_next_pgrp(pgid, 0) > 0 ? 0 : -ESRCH;
    }

    if (pid > 0) {
        target = task_find((pid_t)pid);
        if (!target) return -ESRCH;
        if (target->is_kernel_task) return -EPERM;
        if (current_task && current_task->uid != 0 &&
            current_task->uid != target->uid &&
            current_task->euid != target->uid) return -EPERM;
        return deliver_signal_to_task(target, sig);
    }

    if (!current_task) return -ESRCH;

    if (pid == -1) {
        sent = 0;
        self_pid = current_task->pid;
        uid = current_task->uid;
        euid = current_task->euid;
        last_pid = 0;
        for (;;) {
            next_pid = signal_find_next_permitted(last_pid, self_pid, uid,
                                                  euid);
            if (next_pid <= 0) break;
            last_pid = next_pid;
            t = task_find(next_pid);
            if (t) {
                deliver_signal_to_task(t, sig);
                sent++;
            }
        }
        return sent > 0 ? 0 : -ESRCH;
    }

    if (pid == 0) {
        pgid = current_task->pgid ? current_task->pgid : current_task->pid;
    } else {
        pgid = (pid_t)(-pid);
    }
    if (pgid <= 0) return -EINVAL;

    sent = 0;
    last_pid = 0;
    for (;;) {
        next_pid = signal_find_next_pgrp(pgid, last_pid);
        if (next_pid <= 0) break;
        last_pid = next_pid;
        t = task_find(next_pid);
        if (t) {
            deliver_signal_to_task(t, sig);
            sent++;
        }
    }
    return sent > 0 ? 0 : -ESRCH;
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

static int sys_sigaltstack(uint64_t ss_ptr, const char *old_ss_ptr,
                           int unused) {
    task_signals_t *sigs;
    uint64_t old_addr;
    uint64_t new_addr;
    stack_k empty_stack;

    (void)unused;

    old_addr = (uint64_t)(uintptr_t)old_ss_ptr;
    new_addr = (uint64_t)ss_ptr;
    sigs = (current_task ? (task_signals_t *)current_task->signal_data : NULL);

    if (old_addr && old_addr < KERNEL_VMA && old_addr >= 0x1000) {
        if (sigs && sigs->owner_pid == current_task->pid) {
            memcpy((void *)old_addr, &sigs->altstack, sizeof(stack_k));
        } else {
            memset(&empty_stack, 0, sizeof(empty_stack));
            memcpy((void *)old_addr, &empty_stack, sizeof(stack_k));
        }
    }

    if (new_addr && new_addr < KERNEL_VMA && new_addr >= 0x1000) {
        if (!sigs || sigs->owner_pid != current_task->pid) {
            sigs = get_task_signals();
            if (!sigs) return -ESRCH;
        }
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
    sigaction_k default_act;
    uint64_t sp;
    uint64_t *frame;
    uint64_t info_address;
    uint64_t bit;
    uint64_t previous;
    uint8_t signal_info[128];
    int queued;

    if (!current_task || !regs) return;
    if (!current_task->is_user) return;

    sigs = (task_signals_t *)current_task->signal_data;
    if (!sigs || sigs->in_signal) return;
    if (sigs->owner_pid != current_task->pid) return;

    for (sig = 1; sig < NSIG; sig++) {
        bit = 1UL << (sig - 1);
        if (!(__atomic_load_n(&sigs->pending.sig[0], __ATOMIC_ACQUIRE) & bit))
            continue;
        if (sigs->blocked.sig[0] & bit) continue;

        memset(signal_info, 0, sizeof(signal_info));
        memcpy(signal_info, &sig, sizeof(sig));
        queued = dequeue_queued_signal(sigs, bit, signal_info);
        if (!queued) {
            previous = __atomic_fetch_and(&sigs->pending.sig[0], ~bit,
                                          __ATOMIC_ACQ_REL);
            if (!(previous & bit)) continue;
        }
        act = get_signal_action(sigs, sig, &default_act);

        if (act->sa_handler == SIG_IGN) continue;

        if (act->sa_handler == SIG_DFL) {
            switch (sig) {
                case SIGCHLD:
                case SIGURG:
                case SIGWINCH:
                    continue;
                case SIGCONT:
                    continue;
                default:
                    task_exit_deferred(128 + sig);
                    return;
            }
        }

        sp = regs->rsp;
        info_address = 0;
        if (act->sa_flags & SA_SIGINFO) {
            sp -= sizeof(signal_info);
            info_address = sp;
        }
        sp -= 18 * 8;
        sp &= ~0xFu;

        if (sp < 0x1000 || sp >= KERNEL_VMA ||
            !signal_user_range_mapped(sp,
                regs->rsp - sp)) {
            task_exit_deferred(128 + sig);
            return;
        }

        if (info_address)
            memcpy((void *)(uintptr_t)info_address, signal_info,
                   sizeof(signal_info));

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
        frame[10] = regs->r8;
        frame[11] = regs->r9;
        frame[12] = regs->r10;
        frame[13] = regs->r11;
        frame[14] = regs->r12;
        frame[15] = regs->r13;
        frame[16] = regs->r14;
        frame[17] = regs->r15;

        sp -= 8;
        if (act->sa_restorer) {
            *(uint64_t *)sp = (uint64_t)(uintptr_t)act->sa_restorer;
        } else {
            *(uint64_t *)sp = 0;
        }

        regs->rdi = (uint64_t)sig;
        if (act->sa_flags & SA_SIGINFO) {
            regs->rsi = info_address;
            regs->rdx = 0;
        }
        regs->rip = (uint64_t)(uintptr_t)act->sa_handler;
        regs->rsp = sp;

        sigs->in_signal = 1;

        if (tty_vt_debug_owner(current_task->pid))
            vt_debug_printf("[VTDBG SIGNAL] dispatch pid=%d sig=%d handler=%llx rsp=%llx\n",
                            current_task->pid, sig, regs->rip, regs->rsp);
        if (act->sa_flags & SA_RESETHAND) {
            default_signal_action(&default_act);
            set_signal_action(sigs, sig, &default_act);
        }

        return;
    }
}

void signals_init_task(task_t *task) {
    task_signals_t *slot;

    if (!task) return;
    if (task->signal_data) {
        slot = (task_signals_t *)task->signal_data;
        free_signal_actions(slot);
        free_signal_queue(slot);
        init_signal_slot(slot, task->pid);
    } else {
        task->signal_data = NULL;
    }
}

void syscalls_signal_init(void) {
    syscall_table_set(SYSCALL_SIGACTION, (void *)(sys_rt_sigaction));
    syscall_table_set(SYSCALL_RT_SIGACTION, (void *)(sys_rt_sigaction));
    syscall_table_set(SYSCALL_SIGPROCMASK, (void *)(sys_rt_sigprocmask));
    syscall_table_set(SYSCALL_RT_SIGPROCMASK, (void *)(sys_rt_sigprocmask));
    syscall_table_set(SYSCALL_RT_SIGPENDING, (void *)(sys_rt_sigpending));
    syscall_table_set(SYSCALL_RT_SIGSUSPEND, (void *)(sys_rt_sigsuspend));
    syscall_table_set(SYSCALL_RT_SIGRETURN, (void *)(sys_rt_sigreturn));
    syscall_table_set(SYSCALL_RT_SIGTIMEDWAIT, (void *)(sys_rt_sigtimedwait));
    syscall_table_set(SYSCALL_RT_SIGQUEUEINFO, (void *)(sys_rt_sigqueueinfo));
    syscall_table_set(SYSCALL_TGKILL, (void *)(sys_tgkill));
    syscall_table_set(SYSCALL_TKILL, (void *)(sys_tkill));
    syscall_table_set(SYSCALL_SIGALTSTACK, (void *)(sys_sigaltstack));
    syscall_table_set(SYSCALL_PAUSE, (void *)(sys_pause));
    syscall_table_set(SYSCALL_ALARM, (void *)(sys_alarm));
    syscall_table_set(SYSCALL_SIGRETURN, (void *)(sys_sigreturn));
}

#include <signal.h>
#include <errno.h>
#include <string.h>

#if !defined(__is_libk)

#include <syscall.h>

static sighandler_t signal_handlers[_NSIG];
static struct sigaction signal_actions[_NSIG];
static sigset_t signal_mask = 0;
static sigset_t pending_signals = 0;
static stack_t signal_stack = {0};

sighandler_t signal(int signum, sighandler_t handler) {
    if (signum < 1 || signum >= _NSIG) {
        errno = EINVAL;
        return SIG_ERR;
    }
    if (signum == SIGKILL || signum == SIGSTOP) {
        errno = EINVAL;
        return SIG_ERR;
    }
    sighandler_t old = signal_handlers[signum];
    signal_handlers[signum] = handler;
    return old;
}

int raise(int sig) {
    if (sig < 1 || sig >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    sighandler_t handler = signal_handlers[sig];
    if (handler == SIG_IGN) {
        return 0;
    }
    if (handler == SIG_DFL) {
        return 0;
    }
    handler(sig);
    return 0;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (signum < 1 || signum >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (signum == SIGKILL || signum == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    if (oldact) {
        memcpy(oldact, &signal_actions[signum], sizeof(struct sigaction));
    }
    if (act) {
        memcpy(&signal_actions[signum], act, sizeof(struct sigaction));
        signal_handlers[signum] = act->sa_handler;
    }
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (oldset) {
        *oldset = signal_mask;
    }
    if (set) {
        switch (how) {
            case SIG_BLOCK:
                signal_mask |= *set;
                break;
            case SIG_UNBLOCK:
                signal_mask &= ~(*set);
                break;
            case SIG_SETMASK:
                signal_mask = *set;
                break;
            default:
                errno = EINVAL;
                return -1;
        }
    }
    return 0;
}

int sigpending(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = pending_signals & signal_mask;
    return 0;
}

int sigsuspend(const sigset_t *mask) {
    sigset_t old_mask = signal_mask;
    signal_mask = *mask;
    signal_mask = old_mask;
    errno = EINTR;
    return -1;
}

int sigemptyset(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = ~((sigset_t)0);
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= (1UL << (signum - 1));
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1UL << (signum - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (!set || signum < 1 || signum >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    return (*set & (1UL << (signum - 1))) ? 1 : 0;
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    if (old_ss) {
        memcpy(old_ss, &signal_stack, sizeof(stack_t));
    }
    if (ss) {
        memcpy(&signal_stack, ss, sizeof(stack_t));
    }
    return 0;
}

#else

sighandler_t signal(int signum, sighandler_t handler) {
    (void)signum;
    (void)handler;
    return SIG_ERR;
}

int raise(int sig) {
    (void)sig;
    return -1;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    (void)signum;
    (void)act;
    (void)oldact;
    return -1;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how;
    (void)set;
    (void)oldset;
    return -1;
}

int sigpending(sigset_t *set) {
    (void)set;
    return -1;
}

int sigsuspend(const sigset_t *mask) {
    (void)mask;
    return -1;
}

int sigemptyset(sigset_t *set) {
    if (set) *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (set) *set = ~((sigset_t)0);
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    (void)set;
    (void)signum;
    return -1;
}

int sigdelset(sigset_t *set, int signum) {
    (void)set;
    (void)signum;
    return -1;
}

int sigismember(const sigset_t *set, int signum) {
    (void)set;
    (void)signum;
    return 0;
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    (void)ss;
    (void)old_ss;
    return -1;
}

#endif

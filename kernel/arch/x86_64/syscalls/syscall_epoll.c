#include "syscall_defs.h"
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/inotify.h>

#define EPOLL_INIT_COUNT 1
#define FUTEX_INIT_COUNT 32
#define EVENTFD_INIT_COUNT 1
#define TIMERFD_INIT_COUNT 1
#define SIGNALFD_INIT_COUNT 1

#define EPOLL_BASE_FD    (TASK_MAX_FDS + 1)
#define EVENTFD_BASE_FD  (TASK_MAX_FDS + 0x1001)
#define TIMERFD_BASE_FD  (TASK_MAX_FDS + 0x2001)
#define SIGNALFD_BASE_FD (TASK_MAX_FDS + 0x3001)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN     0x001
#define EPOLLPRI    0x002
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDNORM 0x040
#define EPOLLRDBAND 0x080
#define EPOLLWRNORM 0x100
#define EPOLLWRBAND 0x200
#define EPOLLMSG    0x400
#define EPOLLRDHUP  0x2000
#define EPOLLONESHOT 0x40000000
#define EPOLLET     0x80000000
#define EPOLL_CLOEXEC 0x80000

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_TRYLOCK_PI    8
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   0x80000
#define EFD_NONBLOCK  0x800

typedef struct {
    uint32_t events;
    union {
        void *ptr;
        int fd;
        uint32_t u32;
        uint64_t u64;
    } data;
} __attribute__((packed)) epoll_event_t;

typedef struct {
    int fd;
    uint32_t events;
    epoll_event_t event;
    int active;
} epoll_fd_entry_t;

typedef struct {
    int in_use;
    pid_t owner_pid;
    epoll_fd_entry_t *fds;
    int fd_count;
    int fd_capacity;
    int flags;
    mutex_t lock;
} epoll_instance_t;

typedef struct {
    int in_use;
    pid_t owner_pid;
    uint64_t counter;
    int flags;
    int semaphore;
} eventfd_instance_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} event_timespec_t;

typedef struct {
    event_timespec_t it_interval;
    event_timespec_t it_value;
} event_itimerspec_t;

typedef struct {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint8_t padding[48];
} event_signalfd_info_t;

_Static_assert(sizeof(epoll_event_t) == 12, "epoll event ABI");
_Static_assert(sizeof(event_signalfd_info_t) == 128, "signalfd ABI");

typedef struct {
    int in_use;
    pid_t owner_pid;
    int flags;
    uint64_t interval_ticks;
    uint64_t next_tick;
} timerfd_instance_t;

typedef struct {
    int in_use;
    pid_t owner_pid;
    int flags;
    uint64_t mask;
} signalfd_instance_t;

static epoll_instance_t *epoll_instances = NULL;
static int epoll_capacity = 0;
static mutex_t epoll_lock;

static eventfd_instance_t *eventfds = NULL;
static int eventfd_capacity = 0;
static mutex_t eventfd_lock;
static timerfd_instance_t *timerfds = NULL;
static int timerfd_capacity = 0;
static mutex_t timerfd_lock;
static signalfd_instance_t *signalfds = NULL;
static int signalfd_capacity = 0;
static mutex_t signalfd_lock;

extern volatile uint64_t tick_count;
extern uint64_t pit_freq;

static int epoll_grow(void) {
    int new_cap;
    epoll_instance_t *new_arr;
    int i;

    new_cap = epoll_capacity ? epoll_capacity * 2 : EPOLL_INIT_COUNT;
    new_arr = (epoll_instance_t *)krealloc(epoll_instances, new_cap * sizeof(epoll_instance_t));
    if (!new_arr) return -1;
    for (i = epoll_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(epoll_instance_t));
    }
    epoll_instances = new_arr;
    epoll_capacity = new_cap;
    return 0;
}

static int eventfd_grow(void) {
    int new_cap;
    eventfd_instance_t *new_arr;
    int i;

    new_cap = eventfd_capacity ? eventfd_capacity * 2 : EVENTFD_INIT_COUNT;
    new_arr = (eventfd_instance_t *)krealloc(eventfds, new_cap * sizeof(eventfd_instance_t));
    if (!new_arr) return -1;
    for (i = eventfd_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(eventfd_instance_t));
    }
    eventfds = new_arr;
    eventfd_capacity = new_cap;
    return 0;
}

static int timerfd_grow(void) {
    int new_capacity;
    timerfd_instance_t *new_array;
    int i;

    new_capacity = timerfd_capacity ? timerfd_capacity * 2 : TIMERFD_INIT_COUNT;
    new_array = (timerfd_instance_t *)krealloc(
        timerfds, new_capacity * sizeof(timerfd_instance_t));
    if (!new_array) return -1;
    for (i = timerfd_capacity; i < new_capacity; i++) {
        memset(&new_array[i], 0, sizeof(timerfd_instance_t));
    }
    timerfds = new_array;
    timerfd_capacity = new_capacity;
    return 0;
}

static int signalfd_grow(void) {
    int new_capacity;
    signalfd_instance_t *new_array;
    int i;

    new_capacity = signalfd_capacity ? signalfd_capacity * 2 : SIGNALFD_INIT_COUNT;
    new_array = (signalfd_instance_t *)krealloc(
        signalfds, new_capacity * sizeof(signalfd_instance_t));
    if (!new_array) return -1;
    for (i = signalfd_capacity; i < new_capacity; i++) {
        memset(&new_array[i], 0, sizeof(signalfd_instance_t));
    }
    signalfds = new_array;
    signalfd_capacity = new_capacity;
    return 0;
}

static int timerfd_allocate(void) {
    int index;

    for (index = 0; index < timerfd_capacity; index++) {
        if (!timerfds[index].in_use) return index;
    }
    if (timerfd_grow() < 0) return -1;
    return timerfd_capacity / 2;
}

static int signalfd_allocate(void) {
    int index;

    for (index = 0; index < signalfd_capacity; index++) {
        if (!signalfds[index].in_use) return index;
    }
    if (signalfd_grow() < 0) return -1;
    return signalfd_capacity / 2;
}

static uint64_t timespec_to_ticks(const event_timespec_t *value) {
    uint64_t seconds;
    uint64_t nanoseconds;
    uint64_t ticks;
    uint64_t partial;

    if (!value || value->tv_sec < 0 || value->tv_nsec < 0 ||
        value->tv_nsec >= 1000000000LL || pit_freq == 0) return UINT64_MAX;
    seconds = (uint64_t)value->tv_sec;
    nanoseconds = (uint64_t)value->tv_nsec;
    if (seconds > UINT64_MAX / pit_freq) return UINT64_MAX;
    ticks = seconds * pit_freq;
    if (nanoseconds != 0) {
        if (nanoseconds > (UINT64_MAX - 999999999ULL) / pit_freq)
            return UINT64_MAX;
        partial = (nanoseconds * pit_freq + 999999999ULL) / 1000000000ULL;
        if (partial > UINT64_MAX - ticks) return UINT64_MAX;
        ticks += partial;
    }
    return ticks;
}

static void ticks_to_timespec(uint64_t ticks, event_timespec_t *value) {
    if (!value || pit_freq == 0) return;
    value->tv_sec = (int64_t)(ticks / pit_freq);
    value->tv_nsec = (int64_t)(((ticks % pit_freq) * 1000000000ULL) /
                               pit_freq);
}

static uint64_t timerfd_expirations(timerfd_instance_t *timer, int consume) {
    uint64_t expirations;
    uint64_t elapsed;

    if (!timer || timer->next_tick == 0 || tick_count < timer->next_tick)
        return 0;
    expirations = 1;
    if (timer->interval_ticks != 0) {
        elapsed = tick_count - timer->next_tick;
        expirations += elapsed / timer->interval_ticks;
        if (consume) timer->next_tick += expirations * timer->interval_ticks;
    } else if (consume) {
        timer->next_tick = 0;
    }
    return expirations;
}

static int alloc_epoll(void) {
    int i;

    for (i = 0; i < epoll_capacity; i++) {
        if (!epoll_instances[i].in_use) goto found;
    }
    if (epoll_grow() < 0) return -1;
    i = epoll_capacity / 2;
found:
    memset(&epoll_instances[i], 0, sizeof(epoll_instance_t));
    epoll_instances[i].in_use = 1;
    epoll_instances[i].owner_pid = current_task ? current_task->pid : 0;
    mutex_init(&epoll_instances[i].lock);
    return i;
}

static epoll_instance_t *get_epoll(int fd) {
    int idx;

    idx = fd - EPOLL_BASE_FD;
    if (idx < 0 || idx >= epoll_capacity) return NULL;
    if (!epoll_instances[idx].in_use) return NULL;
    if (!current_task || epoll_instances[idx].owner_pid != current_task->pid)
        return NULL;
    return &epoll_instances[idx];
}

extern int socket_poll_events(int fd);
extern int is_socket_fd(int fd);
int event_descriptor_poll(int fd);

static int epoll_target_valid(int fd) {
    if (fd < 0 || !current_task) return 0;
    if (is_socket_fd(fd)) return 1;
    if (event_descriptor_poll(fd) >= 0) return 1;
    return current_task->fds && fd < current_task->fds_capacity &&
           current_task->fds[fd].in_use;
}

static int sys_epoll_create(int size) {
    int idx;

    if (size <= 0) return -EINVAL;
    
    mutex_lock(&epoll_lock);
    idx = alloc_epoll();
    if (idx < 0) {
        mutex_unlock(&epoll_lock);
        return -EMFILE;
    }
    mutex_unlock(&epoll_lock);
    
    return EPOLL_BASE_FD + idx;
}

static int sys_epoll_create1(int flags) {
    int idx;

    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
    mutex_lock(&epoll_lock);
    idx = alloc_epoll();
    if (idx < 0) {
        mutex_unlock(&epoll_lock);
        return -EMFILE;
    }
    epoll_instances[idx].flags = flags;
    mutex_unlock(&epoll_lock);
    
    return EPOLL_BASE_FD + idx;
}

static int sys_epoll_ctl(int epfd, const char *op_ptr, int fd) {
    int op;
    epoll_instance_t *ep;
    int found;
    int vacant;
    int i;
    int new_cap;
    epoll_fd_entry_t *new_fds;
    epoll_event_t requested_event;
    epoll_event_t *event_ptr;

    op = (int)(uintptr_t)op_ptr;
    memset(&requested_event, 0, sizeof(requested_event));
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (fd == epfd) return -EINVAL;
        if (!epoll_target_valid(fd)) return -EBADF;
        event_ptr = NULL;
        if (current_task && current_task->syscall_frame) {
            event_ptr = (epoll_event_t *)(uintptr_t)
                        current_task->syscall_frame->rsi;
        }
        if (!event_ptr || copy_from_user(&requested_event, event_ptr,
                                         sizeof(requested_event)) < 0)
            return -EFAULT;
    }

    mutex_lock(&epoll_lock);
    ep = get_epoll(epfd);
    if (!ep) {
        mutex_unlock(&epoll_lock);
        return -EBADF;
    }
    mutex_lock(&ep->lock);
    
    found = -1;
    vacant = -1;
    for (i = 0; i < ep->fd_count; i++) {
        if (ep->fds[i].fd == fd && ep->fds[i].active) {
            found = i;
            break;
        }
        if (!ep->fds[i].active && vacant < 0) vacant = i;
    }
    
    switch (op) {
        case EPOLL_CTL_ADD:
            if (found >= 0) {
                mutex_unlock(&ep->lock);
                mutex_unlock(&epoll_lock);
                return -EEXIST;
            }
            if (vacant < 0) {
                new_cap = ep->fd_count + 1;
                new_fds = (epoll_fd_entry_t *)krealloc(ep->fds, new_cap * sizeof(epoll_fd_entry_t));
                if (!new_fds) {
                    mutex_unlock(&ep->lock);
                    mutex_unlock(&epoll_lock);
                    return -ENOMEM;
                }
                ep->fds = new_fds;
                ep->fd_capacity = new_cap;
                vacant = ep->fd_count;
                ep->fd_count++;
            }
            memset(&ep->fds[vacant], 0, sizeof(epoll_fd_entry_t));
            ep->fds[vacant].fd = fd;
            ep->fds[vacant].events = requested_event.events;
            ep->fds[vacant].event = requested_event;
            ep->fds[vacant].active = 1;
            break;
            
        case EPOLL_CTL_DEL:
            if (found < 0) {
                mutex_unlock(&ep->lock);
                mutex_unlock(&epoll_lock);
                return -ENOENT;
            }
            if (found + 1 < ep->fd_count) {
                memmove(&ep->fds[found], &ep->fds[found + 1],
                        (ep->fd_count - found - 1) * sizeof(epoll_fd_entry_t));
            }
            ep->fd_count--;
            if (ep->fd_count == 0) {
                kfree(ep->fds);
                ep->fds = NULL;
                ep->fd_capacity = 0;
            } else {
                new_cap = ep->fd_count;
                new_fds = (epoll_fd_entry_t *)krealloc(
                    ep->fds, new_cap * sizeof(epoll_fd_entry_t));
                if (new_fds) {
                    ep->fds = new_fds;
                    ep->fd_capacity = new_cap;
                }
            }
            break;
            
        case EPOLL_CTL_MOD:
            if (found < 0) {
                mutex_unlock(&ep->lock);
                mutex_unlock(&epoll_lock);
                return -ENOENT;
            }
            ep->fds[found].events = requested_event.events;
            ep->fds[found].event = requested_event;
            ep->fds[found].active = 1;
            break;
            
        default:
            mutex_unlock(&ep->lock);
            mutex_unlock(&epoll_lock);
            return -EINVAL;
    }

    mutex_unlock(&ep->lock);
    mutex_unlock(&epoll_lock);
    return 0;
}

static int sys_epoll_wait(int epfd, const char *events_ptr, int maxevents) {
    epoll_instance_t *ep;
    epoll_event_t *events;
    int count;
    int i;
    uint64_t revents;
    int fd;
    uint64_t wanted;
    int sock_events;
    int descriptor_events;
    epoll_event_t result_event;
    int timeout;
    uint64_t timeout_ticks;
    uint64_t start_tick;

    events = (epoll_event_t *)(uintptr_t)events_ptr;
    if (!events || maxevents <= 0) return -EINVAL;
    timeout = 0;
    if (current_task && current_task->syscall_frame)
        timeout = (int)current_task->syscall_frame->rsi;
    timeout_ticks = timeout > 0 ?
                    ((uint64_t)timeout * pit_freq + 999) / 1000 : 0;
    if (timeout > 0 && timeout_ticks == 0) timeout_ticks = 1;
    start_tick = tick_count;

    for (;;) {
        mutex_lock(&epoll_lock);
        ep = get_epoll(epfd);
        if (!ep) {
            mutex_unlock(&epoll_lock);
            return -EBADF;
        }
        mutex_lock(&ep->lock);
        count = 0;
        for (i = 0; i < ep->fd_count && count < maxevents; i++) {
        if (!ep->fds[i].active) continue;
        
        revents = 0;
        fd = ep->fds[i].fd;
        wanted = ep->fds[i].events;
        
        if (is_socket_fd(fd)) {
            sock_events = socket_poll_events(fd);
            if ((wanted & EPOLLIN) && (sock_events & 0x01)) revents |= EPOLLIN;
            if ((wanted & EPOLLOUT) && (sock_events & 0x04)) revents |= EPOLLOUT;
            if (sock_events & 0x08) revents |= EPOLLERR;
            if (sock_events & 0x10) revents |= EPOLLHUP;
        } else if ((descriptor_events = event_descriptor_poll(fd)) >= 0) {
            revents = (uint64_t)descriptor_events & wanted;
        } else if (epoll_target_valid(fd)) {
            if (wanted & EPOLLIN) revents |= EPOLLIN;
            if (wanted & EPOLLOUT) revents |= EPOLLOUT;
        } else {
            revents = EPOLLERR | EPOLLHUP;
        }
        
            if (revents) {
                result_event.events = revents;
                result_event.data = ep->fds[i].event.data;
                if (copy_to_user(&events[count], &result_event,
                                 sizeof(result_event)) < 0) {
                    mutex_unlock(&ep->lock);
                    mutex_unlock(&epoll_lock);
                    return -EFAULT;
                }
                count++;
                if (ep->fds[i].events & EPOLLONESHOT) {
                    ep->fds[i].active = 0;
                }
            }
        }
        mutex_unlock(&ep->lock);
        mutex_unlock(&epoll_lock);
        if (count || timeout == 0) return count;
        if (timeout > 0 && tick_count - start_tick >= timeout_ticks)
            return 0;
        if (signal_pending_mask(current_task)) return -EINTR;
        sleep_ticks(1);
    }
}

static int sys_epoll_pwait(int epfd, const char *events_ptr, int maxevents) {
    return sys_epoll_wait(epfd, events_ptr, maxevents);
}

static int sys_futex(int *uaddr, const char *op_ptr, int val) {
    int op;
    int cmd;
    int current_value;

    op = (int)(uintptr_t)op_ptr;
    cmd = op & 127;
    if ((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) &&
        copy_from_user(&current_value, uaddr, sizeof(current_value)) < 0)
        return -EFAULT;
    
    switch (cmd) {
        case FUTEX_WAIT:
            if (current_value != val) {
                return -EAGAIN;
            }
            return 0;
        
        case FUTEX_WAKE:
            return 0;
        
        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE:
            return 0;
            
        case FUTEX_WAKE_OP:
            return 0;
            
        case FUTEX_WAIT_BITSET:
            if (current_value != val) {
                return -EAGAIN;
            }
            return 0;
            
        case FUTEX_WAKE_BITSET:
            return 0;
            
        default:
            return -ENOSYS;
    }
}

static int sys_eventfd(unsigned int initval, const char *flags_ptr, int unused) {
    int idx;
    int i;

    (void)flags_ptr;
    (void)unused;
    
    mutex_lock(&eventfd_lock);
    idx = -1;
    for (i = 0; i < eventfd_capacity; i++) {
        if (!eventfds[i].in_use) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        if (eventfd_grow() < 0) {
            mutex_unlock(&eventfd_lock);
            return -EMFILE;
        }
        idx = eventfd_capacity / 2;
    }
    
    memset(&eventfds[idx], 0, sizeof(eventfd_instance_t));
    eventfds[idx].in_use = 1;
    eventfds[idx].owner_pid = current_task ? current_task->pid : 0;
    eventfds[idx].counter = initval;
    eventfds[idx].flags = 0;
    eventfds[idx].semaphore = 0;
    
    mutex_unlock(&eventfd_lock);
    return EVENTFD_BASE_FD + idx;
}

static int sys_eventfd2(unsigned int initval, const char *flags_ptr, int unused) {
    int flags;
    int idx;
    int i;

    flags = (int)(uintptr_t)flags_ptr;
    (void)unused;
    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK))
        return -EINVAL;
    
    mutex_lock(&eventfd_lock);
    idx = -1;
    for (i = 0; i < eventfd_capacity; i++) {
        if (!eventfds[i].in_use) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        if (eventfd_grow() < 0) {
            mutex_unlock(&eventfd_lock);
            return -EMFILE;
        }
        idx = eventfd_capacity / 2;
    }
    
    memset(&eventfds[idx], 0, sizeof(eventfd_instance_t));
    eventfds[idx].in_use = 1;
    eventfds[idx].owner_pid = current_task ? current_task->pid : 0;
    eventfds[idx].counter = initval;
    eventfds[idx].flags = flags;
    eventfds[idx].semaphore = (flags & EFD_SEMAPHORE) ? 1 : 0;
    
    mutex_unlock(&eventfd_lock);
    return EVENTFD_BASE_FD + idx;
}

static int sys_set_robust_list(int head, const char *len_ptr, int unused) {
    (void)head; (void)len_ptr; (void)unused;
    if (current_task) {
        current_task->robust_list = (void *)(uintptr_t)head;
    }
    return 0;
}

static int sys_get_robust_list(int pid, const char *head_ptr_ptr, int len_ptr) {
    (void)pid; (void)head_ptr_ptr; (void)len_ptr;
    return 0;
}

static int sys_timerfd_create(int clockid, const char *flags_ptr, int unused) {
    int flags;
    int index;

    (void)unused;
    flags = (int)(uintptr_t)flags_ptr;
    if (clockid != 0 && clockid != 1) return -EINVAL;
    if (flags & ~(EFD_CLOEXEC | EFD_NONBLOCK)) return -EINVAL;
    mutex_lock(&timerfd_lock);
    index = timerfd_allocate();
    if (index < 0) {
        mutex_unlock(&timerfd_lock);
        return -EMFILE;
    }
    memset(&timerfds[index], 0, sizeof(timerfd_instance_t));
    timerfds[index].in_use = 1;
    timerfds[index].owner_pid = current_task ? current_task->pid : 0;
    timerfds[index].flags = flags;
    mutex_unlock(&timerfd_lock);
    return TIMERFD_BASE_FD + index;
}

static int sys_timerfd_settime(int fd, const char *flags_ptr, int new_value) {
    int flags;
    int index;
    event_itimerspec_t requested;
    event_itimerspec_t previous;
    event_itimerspec_t *old_value;
    uint64_t initial_ticks;
    uint64_t interval_ticks;
    uint64_t remaining;

    flags = (int)(uintptr_t)flags_ptr;
    if (flags & ~1) return -EINVAL;
    if (copy_from_user(&requested, (const void *)(uintptr_t)new_value,
                       sizeof(requested)) < 0) return -EFAULT;
    initial_ticks = timespec_to_ticks(&requested.it_value);
    interval_ticks = timespec_to_ticks(&requested.it_interval);
    if (initial_ticks == UINT64_MAX || interval_ticks == UINT64_MAX)
        return -EINVAL;
    old_value = NULL;
    if (current_task && current_task->syscall_frame) {
        old_value = (event_itimerspec_t *)(uintptr_t)
                    current_task->syscall_frame->rsi;
    }
    if (old_value && !user_access_ok(old_value, sizeof(previous),
                                     UACCESS_WRITE)) return -EFAULT;
    index = fd - TIMERFD_BASE_FD;
    if (index < 0 || index >= timerfd_capacity) return -EBADF;
    mutex_lock(&timerfd_lock);
    if (!timerfds[index].in_use || !current_task ||
        timerfds[index].owner_pid != current_task->pid) {
        mutex_unlock(&timerfd_lock);
        return -EBADF;
    }
    memset(&previous, 0, sizeof(previous));
    remaining = timerfds[index].next_tick > tick_count ?
                timerfds[index].next_tick - tick_count : 0;
    ticks_to_timespec(remaining, &previous.it_value);
    ticks_to_timespec(timerfds[index].interval_ticks, &previous.it_interval);
    timerfds[index].interval_ticks = interval_ticks;
    if (initial_ticks == 0) {
        timerfds[index].next_tick = 0;
    } else if (flags & 1) {
        timerfds[index].next_tick = initial_ticks;
    } else {
        timerfds[index].next_tick = tick_count + initial_ticks;
    }
    mutex_unlock(&timerfd_lock);
    if (old_value && copy_to_user(old_value, &previous, sizeof(previous)) < 0)
        return -EFAULT;
    return 0;
}

static int sys_timerfd_gettime(int fd, const char *curr_value_ptr, int unused) {
    int index;
    uint64_t remaining;
    event_itimerspec_t value;

    (void)unused;
    index = fd - TIMERFD_BASE_FD;
    if (index < 0 || index >= timerfd_capacity) return -EBADF;
    mutex_lock(&timerfd_lock);
    if (!timerfds[index].in_use || !current_task ||
        timerfds[index].owner_pid != current_task->pid) {
        mutex_unlock(&timerfd_lock);
        return -EBADF;
    }
    memset(&value, 0, sizeof(value));
    remaining = timerfds[index].next_tick > tick_count ?
                timerfds[index].next_tick - tick_count : 0;
    ticks_to_timespec(remaining, &value.it_value);
    ticks_to_timespec(timerfds[index].interval_ticks, &value.it_interval);
    mutex_unlock(&timerfd_lock);
    if (copy_to_user((void *)curr_value_ptr, &value, sizeof(value)) < 0)
        return -EFAULT;
    return 0;
}

static int signalfd_configure(int fd, const char *mask_ptr, int flags) {
    uint64_t mask;
    int index;

    if (flags & ~(EFD_CLOEXEC | EFD_NONBLOCK)) return -EINVAL;
    if (copy_from_user(&mask, mask_ptr, sizeof(mask)) < 0) return -EFAULT;
    mutex_lock(&signalfd_lock);
    if (fd >= 0) {
        index = fd - SIGNALFD_BASE_FD;
        if (index < 0 || index >= signalfd_capacity ||
            !signalfds[index].in_use || !current_task ||
            signalfds[index].owner_pid != current_task->pid) {
            mutex_unlock(&signalfd_lock);
            return -EBADF;
        }
    } else {
        index = signalfd_allocate();
        if (index < 0) {
            mutex_unlock(&signalfd_lock);
            return -EMFILE;
        }
        memset(&signalfds[index], 0, sizeof(signalfd_instance_t));
        signalfds[index].in_use = 1;
        signalfds[index].owner_pid = current_task ? current_task->pid : 0;
    }
    signalfds[index].mask = mask;
    signalfds[index].flags = flags;
    mutex_unlock(&signalfd_lock);
    return SIGNALFD_BASE_FD + index;
}

static int sys_signalfd(int fd, const char *mask_ptr, int mask_size) {
    if (mask_size != (int)sizeof(uint64_t)) return -EINVAL;
    return signalfd_configure(fd, mask_ptr, 0);
}

static int sys_signalfd4(int fd, const char *mask_ptr, int mask_size) {
    int actual_flags;

    if (mask_size != (int)sizeof(uint64_t)) return -EINVAL;
    actual_flags = 0;
    if (current_task && current_task->syscall_frame) {
        actual_flags = (int)current_task->syscall_frame->rsi;
    }
    return signalfd_configure(fd, mask_ptr, actual_flags);
}

int event_descriptor_poll(int fd) {
    int index;
    int events;
    int found;
    uint64_t pending;

    events = inotify_poll_fd(fd);
    if (events >= 0) return events ? EPOLLIN : 0;
    events = 0;
    found = 0;
    index = fd - EVENTFD_BASE_FD;
    mutex_lock(&eventfd_lock);
    if (index >= 0 && index < eventfd_capacity && eventfds &&
        eventfds[index].in_use && current_task &&
        eventfds[index].owner_pid == current_task->pid) {
        if (eventfds[index].counter != 0) events |= EPOLLIN;
        if (eventfds[index].counter != UINT64_MAX - 1) events |= EPOLLOUT;
        found = 1;
    }
    mutex_unlock(&eventfd_lock);
    if (found) return events;
    index = fd - TIMERFD_BASE_FD;
    mutex_lock(&timerfd_lock);
    if (index >= 0 && index < timerfd_capacity && timerfds &&
        timerfds[index].in_use && current_task &&
        timerfds[index].owner_pid == current_task->pid) {
        if (timerfd_expirations(&timerfds[index], 0) != 0) events |= EPOLLIN;
        found = 1;
    }
    mutex_unlock(&timerfd_lock);
    if (found) return events;
    index = fd - SIGNALFD_BASE_FD;
    mutex_lock(&signalfd_lock);
    if (index >= 0 && index < signalfd_capacity && signalfds &&
        signalfds[index].in_use && current_task &&
        signalfds[index].owner_pid == current_task->pid) {
        pending = signal_pending_mask(current_task) & signalfds[index].mask;
        if (pending != 0) events |= EPOLLIN;
        found = 1;
    }
    mutex_unlock(&signalfd_lock);
    if (found) return events;
    return -EBADF;
}

int event_descriptor_read(int fd, void *buffer, int length) {
    int index;
    uint64_t value;
    int signal_number;
    event_signalfd_info_t signal_info;
    int descriptor_flags;

    if (!buffer || length < 0) return -EINVAL;
    if (inotify_is_fd(fd)) return inotify_read_fd(fd, buffer, length);
retry_read:
    index = fd - EVENTFD_BASE_FD;
    if (index >= 0 && fd < TIMERFD_BASE_FD) {
        if (length < (int)sizeof(uint64_t)) return -EINVAL;
        if (!user_access_ok(buffer, sizeof(uint64_t), UACCESS_WRITE))
            return -EFAULT;
        mutex_lock(&eventfd_lock);
        if (!eventfds || index >= eventfd_capacity ||
            !eventfds[index].in_use || !current_task ||
            eventfds[index].owner_pid != current_task->pid) {
            mutex_unlock(&eventfd_lock);
            return -EBADF;
        }
        if (eventfds[index].counter == 0) {
            descriptor_flags = eventfds[index].flags;
            mutex_unlock(&eventfd_lock);
            if (descriptor_flags & EFD_NONBLOCK) return -EAGAIN;
            if (signal_pending_mask(current_task)) return -EINTR;
            sleep_ticks(1);
            goto retry_read;
        }
        if (eventfds[index].semaphore) {
            value = 1;
            eventfds[index].counter--;
        } else {
            value = eventfds[index].counter;
            eventfds[index].counter = 0;
        }
        mutex_unlock(&eventfd_lock);
        if (copy_to_user(buffer, &value, sizeof(value)) < 0) return -EFAULT;
        return sizeof(value);
    }
    index = fd - TIMERFD_BASE_FD;
    if (index >= 0 && fd < SIGNALFD_BASE_FD) {
        if (length < (int)sizeof(uint64_t)) return -EINVAL;
        if (!user_access_ok(buffer, sizeof(uint64_t), UACCESS_WRITE))
            return -EFAULT;
        mutex_lock(&timerfd_lock);
        if (!timerfds || index >= timerfd_capacity ||
            !timerfds[index].in_use || !current_task ||
            timerfds[index].owner_pid != current_task->pid) {
            mutex_unlock(&timerfd_lock);
            return -EBADF;
        }
        value = timerfd_expirations(&timerfds[index], 1);
        descriptor_flags = timerfds[index].flags;
        mutex_unlock(&timerfd_lock);
        if (value == 0) {
            if (descriptor_flags & EFD_NONBLOCK) return -EAGAIN;
            if (signal_pending_mask(current_task)) return -EINTR;
            sleep_ticks(1);
            goto retry_read;
        }
        if (copy_to_user(buffer, &value, sizeof(value)) < 0) return -EFAULT;
        return sizeof(value);
    }
    index = fd - SIGNALFD_BASE_FD;
    if (index >= 0 && fd < SIGNALFD_BASE_FD + 0x1000) {
        if (length < (int)sizeof(event_signalfd_info_t)) return -EINVAL;
        if (!user_access_ok(buffer, sizeof(event_signalfd_info_t),
                            UACCESS_WRITE)) return -EFAULT;
        mutex_lock(&signalfd_lock);
        if (!signalfds || index >= signalfd_capacity ||
            !signalfds[index].in_use || !current_task ||
            signalfds[index].owner_pid != current_task->pid) {
            mutex_unlock(&signalfd_lock);
            return -EBADF;
        }
        signal_number = signal_take_pending(current_task,
                                            signalfds[index].mask);
        descriptor_flags = signalfds[index].flags;
        mutex_unlock(&signalfd_lock);
        if (signal_number == 0) {
            if (descriptor_flags & EFD_NONBLOCK) return -EAGAIN;
            if (signal_pending_mask(current_task)) return -EINTR;
            sleep_ticks(1);
            goto retry_read;
        }
        memset(&signal_info, 0, sizeof(signal_info));
        signal_info.ssi_signo = (uint32_t)signal_number;
        signal_info.ssi_pid = (uint32_t)current_task->pid;
        signal_info.ssi_uid = (uint32_t)current_task->uid;
        if (copy_to_user(buffer, &signal_info, sizeof(signal_info)) < 0)
            return -EFAULT;
        return sizeof(signal_info);
    }
    return -EBADF;
}

int event_descriptor_write(int fd, const void *buffer, int length) {
    int index;
    uint64_t value;
    int descriptor_flags;

    if (!buffer || length < 0) return -EINVAL;
    if (inotify_is_fd(fd)) return -EBADF;
    index = fd - EVENTFD_BASE_FD;
    if (index < 0 || fd >= TIMERFD_BASE_FD) return -EBADF;
    if (length < (int)sizeof(uint64_t)) return -EINVAL;
    if (copy_from_user(&value, buffer, sizeof(value)) < 0) return -EFAULT;
    if (value == UINT64_MAX) return -EINVAL;
retry_write:
    mutex_lock(&eventfd_lock);
    if (!eventfds || index >= eventfd_capacity ||
        !eventfds[index].in_use || !current_task ||
        eventfds[index].owner_pid != current_task->pid) {
        mutex_unlock(&eventfd_lock);
        return -EBADF;
    }
    if (value > UINT64_MAX - 1 - eventfds[index].counter) {
        descriptor_flags = eventfds[index].flags;
        mutex_unlock(&eventfd_lock);
        if (descriptor_flags & EFD_NONBLOCK) return -EAGAIN;
        if (signal_pending_mask(current_task)) return -EINTR;
        sleep_ticks(1);
        goto retry_write;
    }
    eventfds[index].counter += value;
    mutex_unlock(&eventfd_lock);
    return sizeof(value);
}

int is_epoll_special_fd(int fd) {
    int idx;
    int result;

    if (inotify_is_fd(fd)) return 1;
    result = 0;
    idx = fd - EPOLL_BASE_FD;
    mutex_lock(&epoll_lock);
    if (idx >= 0 && idx < epoll_capacity && epoll_instances &&
        epoll_instances[idx].in_use && current_task &&
        epoll_instances[idx].owner_pid == current_task->pid) result = 1;
    mutex_unlock(&epoll_lock);
    if (result) return 1;
    idx = fd - EVENTFD_BASE_FD;
    mutex_lock(&eventfd_lock);
    if (idx >= 0 && idx < eventfd_capacity && eventfds &&
        eventfds[idx].in_use && current_task &&
        eventfds[idx].owner_pid == current_task->pid) result = 1;
    mutex_unlock(&eventfd_lock);
    if (result) return 1;
    idx = fd - TIMERFD_BASE_FD;
    mutex_lock(&timerfd_lock);
    if (idx >= 0 && idx < timerfd_capacity && timerfds &&
        timerfds[idx].in_use && current_task &&
        timerfds[idx].owner_pid == current_task->pid) result = 1;
    mutex_unlock(&timerfd_lock);
    if (result) return 1;
    idx = fd - SIGNALFD_BASE_FD;
    mutex_lock(&signalfd_lock);
    if (idx >= 0 && idx < signalfd_capacity && signalfds &&
        signalfds[idx].in_use && current_task &&
        signalfds[idx].owner_pid == current_task->pid) result = 1;
    mutex_unlock(&signalfd_lock);
    return result;
}

int epoll_close_fd(int fd) {
    int idx;
    int i;
    int any_in_use;
    epoll_instance_t *ep;

    if (inotify_is_fd(fd)) return inotify_close_fd(fd);
    idx = fd - EPOLL_BASE_FD;
    if (idx >= 0 && idx < epoll_capacity) {
        mutex_lock(&epoll_lock);
        if (!epoll_instances[idx].in_use || !current_task ||
            epoll_instances[idx].owner_pid != current_task->pid) {
            mutex_unlock(&epoll_lock);
            return -EBADF;
        }
        ep = &epoll_instances[idx];
        mutex_lock(&ep->lock);
        kfree(ep->fds);
        ep->fds = NULL;
        ep->fd_count = 0;
        ep->fd_capacity = 0;
        ep->in_use = 0;
        mutex_unlock(&ep->lock);
        any_in_use = 0;
        for (i = 0; i < epoll_capacity; i++) {
            if (epoll_instances[i].in_use) {
                any_in_use = 1;
                break;
            }
        }
        if (!any_in_use) {
            kfree(epoll_instances);
            epoll_instances = NULL;
            epoll_capacity = 0;
        }
        mutex_unlock(&epoll_lock);
        return 0;
    }
    idx = fd - EVENTFD_BASE_FD;
    if (idx >= 0 && idx < eventfd_capacity) {
        mutex_lock(&eventfd_lock);
        if (!eventfds[idx].in_use || !current_task ||
            eventfds[idx].owner_pid != current_task->pid) {
            mutex_unlock(&eventfd_lock);
            return -EBADF;
        }
        memset(&eventfds[idx], 0, sizeof(eventfd_instance_t));
        any_in_use = 0;
        for (i = 0; i < eventfd_capacity; i++) {
            if (eventfds[i].in_use) {
                any_in_use = 1;
                break;
            }
        }
        if (!any_in_use) {
            kfree(eventfds);
            eventfds = NULL;
            eventfd_capacity = 0;
        }
        mutex_unlock(&eventfd_lock);
        return 0;
    }
    idx = fd - TIMERFD_BASE_FD;
    if (idx >= 0 && idx < timerfd_capacity) {
        mutex_lock(&timerfd_lock);
        if (!timerfds[idx].in_use || !current_task ||
            timerfds[idx].owner_pid != current_task->pid) {
            mutex_unlock(&timerfd_lock);
            return -EBADF;
        }
        memset(&timerfds[idx], 0, sizeof(timerfd_instance_t));
        any_in_use = 0;
        for (i = 0; i < timerfd_capacity; i++) {
            if (timerfds[i].in_use) {
                any_in_use = 1;
                break;
            }
        }
        if (!any_in_use) {
            kfree(timerfds);
            timerfds = NULL;
            timerfd_capacity = 0;
        }
        mutex_unlock(&timerfd_lock);
        return 0;
    }
    idx = fd - SIGNALFD_BASE_FD;
    if (idx >= 0 && idx < signalfd_capacity) {
        mutex_lock(&signalfd_lock);
        if (!signalfds[idx].in_use || !current_task ||
            signalfds[idx].owner_pid != current_task->pid) {
            mutex_unlock(&signalfd_lock);
            return -EBADF;
        }
        memset(&signalfds[idx], 0, sizeof(signalfd_instance_t));
        any_in_use = 0;
        for (i = 0; i < signalfd_capacity; i++) {
            if (signalfds[i].in_use) {
                any_in_use = 1;
                break;
            }
        }
        if (!any_in_use) {
            kfree(signalfds);
            signalfds = NULL;
            signalfd_capacity = 0;
        }
        mutex_unlock(&signalfd_lock);
        return 0;
    }
    return -EBADF;
}

void event_descriptors_close_task(pid_t pid) {
    int i;
    int any_in_use;

    inotify_close_task(pid);
    mutex_lock(&epoll_lock);
    any_in_use = 0;
    for (i = 0; i < epoll_capacity; i++) {
        if (epoll_instances[i].in_use &&
            epoll_instances[i].owner_pid == pid) {
            kfree(epoll_instances[i].fds);
            memset(&epoll_instances[i], 0, sizeof(epoll_instance_t));
        }
        if (epoll_instances[i].in_use) any_in_use = 1;
    }
    if (!any_in_use) {
        kfree(epoll_instances);
        epoll_instances = NULL;
        epoll_capacity = 0;
    }
    mutex_unlock(&epoll_lock);

    mutex_lock(&eventfd_lock);
    any_in_use = 0;
    for (i = 0; i < eventfd_capacity; i++) {
        if (eventfds[i].in_use && eventfds[i].owner_pid == pid)
            memset(&eventfds[i], 0, sizeof(eventfd_instance_t));
        if (eventfds[i].in_use) any_in_use = 1;
    }
    if (!any_in_use) {
        kfree(eventfds);
        eventfds = NULL;
        eventfd_capacity = 0;
    }
    mutex_unlock(&eventfd_lock);

    mutex_lock(&timerfd_lock);
    any_in_use = 0;
    for (i = 0; i < timerfd_capacity; i++) {
        if (timerfds[i].in_use && timerfds[i].owner_pid == pid)
            memset(&timerfds[i], 0, sizeof(timerfd_instance_t));
        if (timerfds[i].in_use) any_in_use = 1;
    }
    if (!any_in_use) {
        kfree(timerfds);
        timerfds = NULL;
        timerfd_capacity = 0;
    }
    mutex_unlock(&timerfd_lock);

    mutex_lock(&signalfd_lock);
    any_in_use = 0;
    for (i = 0; i < signalfd_capacity; i++) {
        if (signalfds[i].in_use && signalfds[i].owner_pid == pid)
            memset(&signalfds[i], 0, sizeof(signalfd_instance_t));
        if (signalfds[i].in_use) any_in_use = 1;
    }
    if (!any_in_use) {
        kfree(signalfds);
        signalfds = NULL;
        signalfd_capacity = 0;
    }
    mutex_unlock(&signalfd_lock);
}

void syscalls_epoll_init(void) {
    epoll_instances = NULL;
    epoll_capacity = 0;
    eventfds = NULL;
    eventfd_capacity = 0;
    timerfds = NULL;
    timerfd_capacity = 0;
    signalfds = NULL;
    signalfd_capacity = 0;
    mutex_init(&epoll_lock);
    mutex_init(&eventfd_lock);
    mutex_init(&timerfd_lock);
    mutex_init(&signalfd_lock);
    
    syscall_table[SYSCALL_EPOLL_CREATE] = sys_epoll_create;
    syscall_table[SYSCALL_EPOLL_CREATE1] = sys_epoll_create1;
    syscall_table[SYSCALL_EPOLL_CTL] = sys_epoll_ctl;
    syscall_table[SYSCALL_EPOLL_WAIT] = sys_epoll_wait;
    syscall_table[SYSCALL_EPOLL_PWAIT] = sys_epoll_pwait;
    syscall_table[SYSCALL_FUTEX] = sys_futex;
    syscall_table[SYSCALL_EVENTFD] = sys_eventfd;
    syscall_table[SYSCALL_EVENTFD2] = sys_eventfd2;
    syscall_table[SYSCALL_SET_ROBUST_LIST] = sys_set_robust_list;
    syscall_table[SYSCALL_GET_ROBUST_LIST] = sys_get_robust_list;
    syscall_table[SYSCALL_TIMERFD_CREATE] = sys_timerfd_create;
    syscall_table[SYSCALL_TIMERFD_SETTIME] = sys_timerfd_settime;
    syscall_table[SYSCALL_TIMERFD_GETTIME] = sys_timerfd_gettime;
    syscall_table[SYSCALL_SIGNALFD] = sys_signalfd;
    syscall_table[SYSCALL_SIGNALFD4] = sys_signalfd4;
}

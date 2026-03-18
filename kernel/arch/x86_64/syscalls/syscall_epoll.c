#include "syscall_defs.h"
#include <kernel/task.h>
#include <kernel/mutex.h>

#define EPOLL_INIT_COUNT 8
#define EPOLL_INIT_EVENTS 32
#define FUTEX_INIT_COUNT 32
#define EVENTFD_INIT_COUNT 16

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
    uint64_t events;
    union {
        void *ptr;
        int fd;
        uint64_t u32;
        uint64_t u64;
    } data;
} epoll_event_t;

typedef struct {
    int fd;
    uint64_t events;
    epoll_event_t event;
    int active;
} epoll_fd_entry_t;

typedef struct {
    int in_use;
    epoll_fd_entry_t *fds;
    int fd_count;
    int fd_capacity;
    int flags;
    mutex_t lock;
} epoll_instance_t;

typedef struct {
    uint64_t *addr;
    pid_t *waiters;
    int waiter_count;
    int waiter_capacity;
    int in_use;
    mutex_t lock;
} futex_t;

typedef struct {
    int in_use;
    uint64_t counter;
    int flags;
    int semaphore;
} eventfd_instance_t;

static epoll_instance_t *epoll_instances = NULL;
static int epoll_capacity = 0;
static int epoll_base_fd = 400;
static mutex_t epoll_lock;

static futex_t *futexes = NULL;
static int futex_capacity = 0;
static mutex_t futex_lock;

static eventfd_instance_t *eventfds = NULL;
static int eventfd_capacity = 0;
static int eventfd_base_fd = 500;
static mutex_t eventfd_lock;

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

static int futex_grow(void) {
    int new_cap;
    futex_t *new_arr;
    int i;

    new_cap = futex_capacity ? futex_capacity * 2 : FUTEX_INIT_COUNT;
    new_arr = (futex_t *)krealloc(futexes, new_cap * sizeof(futex_t));
    if (!new_arr) return -1;
    for (i = futex_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(futex_t));
    }
    futexes = new_arr;
    futex_capacity = new_cap;
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

static int alloc_epoll(void) {
    int i;

    for (i = 0; i < epoll_capacity; i++) {
        if (!epoll_instances[i].in_use) goto found;
    }
    if (epoll_grow() < 0) return -1;
    i = epoll_capacity / 2;
found:
    memset(&epoll_instances[i], 0, sizeof(epoll_instance_t));
    epoll_instances[i].fd_capacity = EPOLL_INIT_EVENTS;
    epoll_instances[i].fds = (epoll_fd_entry_t *)kmalloc(EPOLL_INIT_EVENTS * sizeof(epoll_fd_entry_t));
    if (!epoll_instances[i].fds) return -1;
    memset(epoll_instances[i].fds, 0, EPOLL_INIT_EVENTS * sizeof(epoll_fd_entry_t));
    epoll_instances[i].in_use = 1;
    mutex_init(&epoll_instances[i].lock);
    return i;
}

static epoll_instance_t *get_epoll(int fd) {
    int idx = fd - epoll_base_fd;
    if (idx < 0 || idx >= epoll_capacity) return NULL;
    if (!epoll_instances[idx].in_use) return NULL;
    return &epoll_instances[idx];
}

static futex_t *find_futex(uint64_t *addr) {
    int i;

    for (i = 0; i < futex_capacity; i++) {
        if (futexes[i].in_use && futexes[i].addr == addr) {
            return &futexes[i];
        }
    }
    return NULL;
}

static futex_t *alloc_futex(uint64_t *addr) {
    int i;

    for (i = 0; i < futex_capacity; i++) {
        if (!futexes[i].in_use) goto found;
    }
    if (futex_grow() < 0) return NULL;
    i = futex_capacity / 2;
found:
    memset(&futexes[i], 0, sizeof(futex_t));
    futexes[i].in_use = 1;
    futexes[i].addr = addr;
    futexes[i].waiter_capacity = 16;
    futexes[i].waiters = (pid_t *)kmalloc(16 * sizeof(pid_t));
    if (!futexes[i].waiters) {
        futexes[i].in_use = 0;
        return NULL;
    }
    memset(futexes[i].waiters, 0, 16 * sizeof(pid_t));
    mutex_init(&futexes[i].lock);
    return &futexes[i];
}

static int sys_epoll_create(int size) {
    (void)size;
    
    mutex_lock(&epoll_lock);
    int idx = alloc_epoll();
    if (idx < 0) {
        mutex_unlock(&epoll_lock);
        return -EMFILE;
    }
    mutex_unlock(&epoll_lock);
    
    return epoll_base_fd + idx;
}

static int sys_epoll_create1(int flags) {
    mutex_lock(&epoll_lock);
    int idx = alloc_epoll();
    if (idx < 0) {
        mutex_unlock(&epoll_lock);
        return -EMFILE;
    }
    epoll_instances[idx].flags = flags;
    mutex_unlock(&epoll_lock);
    
    return epoll_base_fd + idx;
}

static int sys_epoll_ctl(int epfd, const char *op_ptr, int fd) {
    int op = (int)(uintptr_t)op_ptr;
    
    epoll_instance_t *ep = get_epoll(epfd);
    if (!ep) return -EBADF;
    
    mutex_lock(&ep->lock);
    
    int found = -1;
    for (int i = 0; i < ep->fd_count; i++) {
        if (ep->fds[i].fd == fd && ep->fds[i].active) {
            found = i;
            break;
        }
    }
    
    switch (op) {
        case EPOLL_CTL_ADD:
            if (found >= 0) {
                mutex_unlock(&ep->lock);
                return -EEXIST;
            }
            if (ep->fd_count >= ep->fd_capacity) {
                int new_cap = ep->fd_capacity * 2;
                epoll_fd_entry_t *new_fds = (epoll_fd_entry_t *)krealloc(ep->fds, new_cap * sizeof(epoll_fd_entry_t));
                if (!new_fds) {
                    mutex_unlock(&ep->lock);
                    return -ENOMEM;
                }
                memset(&new_fds[ep->fd_capacity], 0, (new_cap - ep->fd_capacity) * sizeof(epoll_fd_entry_t));
                ep->fds = new_fds;
                ep->fd_capacity = new_cap;
            }
            ep->fds[ep->fd_count].fd = fd;
            ep->fds[ep->fd_count].events = EPOLLIN | EPOLLOUT;
            ep->fds[ep->fd_count].active = 1;
            ep->fd_count++;
            break;
            
        case EPOLL_CTL_DEL:
            if (found < 0) {
                mutex_unlock(&ep->lock);
                return -ENOENT;
            }
            ep->fds[found].active = 0;
            break;
            
        case EPOLL_CTL_MOD:
            if (found < 0) {
                mutex_unlock(&ep->lock);
                return -ENOENT;
            }
            break;
            
        default:
            mutex_unlock(&ep->lock);
            return -EINVAL;
    }
    
    mutex_unlock(&ep->lock);
    return 0;
}

extern int socket_poll_events(int fd);
extern int is_socket_fd(int fd);

static int sys_epoll_wait(int epfd, const char *events_ptr, int maxevents) {
    epoll_instance_t *ep = get_epoll(epfd);
    if (!ep) return -EBADF;
    
    epoll_event_t *events = (epoll_event_t *)(uintptr_t)events_ptr;
    if (!events || maxevents <= 0) return -EINVAL;
    
    mutex_lock(&ep->lock);
    
    int count = 0;
    for (int i = 0; i < ep->fd_count && count < maxevents; i++) {
        if (!ep->fds[i].active) continue;
        
        uint64_t revents = 0;
        int fd = ep->fds[i].fd;
        uint64_t wanted = ep->fds[i].events;
        
        if (is_socket_fd(fd)) {
            int sock_events = socket_poll_events(fd);
            if ((wanted & EPOLLIN) && (sock_events & 0x01)) revents |= EPOLLIN;
            if ((wanted & EPOLLOUT) && (sock_events & 0x04)) revents |= EPOLLOUT;
            if (sock_events & 0x08) revents |= EPOLLERR;
            if (sock_events & 0x10) revents |= EPOLLHUP;
        } else {
            if (wanted & EPOLLIN) revents |= EPOLLIN;
            if (wanted & EPOLLOUT) revents |= EPOLLOUT;
        }
        
        if (revents) {
            events[count].events = revents;
            events[count].data = ep->fds[i].event.data;
            count++;
            
            if (ep->fds[i].events & EPOLLONESHOT) {
                ep->fds[i].active = 0;
            }
        }
    }
    
    mutex_unlock(&ep->lock);
    return count;
}

static int sys_epoll_pwait(int epfd, const char *events_ptr, int maxevents) {
    return sys_epoll_wait(epfd, events_ptr, maxevents);
}

static int sys_futex(int *uaddr, const char *op_ptr, int val) {
    int op = (int)(uintptr_t)op_ptr;
    int cmd = op & 127;
    
    switch (cmd) {
        case FUTEX_WAIT: {
            if (*uaddr != val) {
                return -EAGAIN;
            }
            
            mutex_lock(&futex_lock);
            futex_t *f = find_futex((uint64_t *)uaddr);
            if (!f) {
                f = alloc_futex((uint64_t *)uaddr);
                if (!f) {
                    mutex_unlock(&futex_lock);
                    return -ENOMEM;
                }
            }
            
            if (f->waiter_count < f->waiter_capacity) {
                f->waiters[f->waiter_count++] = current_task->pid;
            }
            mutex_unlock(&futex_lock);
            
            return 0;
        }
        
        case FUTEX_WAKE: {
            mutex_lock(&futex_lock);
            futex_t *f = find_futex((uint64_t *)uaddr);
            if (!f) {
                mutex_unlock(&futex_lock);
                return 0;
            }
            
            int woken = 0;
            int to_wake = (val > f->waiter_count) ? f->waiter_count : val;
            
            for (int i = 0; i < to_wake; i++) {
                f->waiters[i] = 0;
                woken++;
            }
            
            for (int i = to_wake; i < f->waiter_count; i++) {
                f->waiters[i - to_wake] = f->waiters[i];
            }
            f->waiter_count -= woken;
            
            if (f->waiter_count == 0) {
                f->in_use = 0;
            }
            
            mutex_unlock(&futex_lock);
            return woken;
        }
        
        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE:
            return 0;
            
        case FUTEX_WAKE_OP:
            return 0;
            
        case FUTEX_WAIT_BITSET:
            if (*uaddr != val) {
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
    (void)flags_ptr;
    (void)unused;
    
    mutex_lock(&eventfd_lock);
    int idx = -1;
    for (int i = 0; i < eventfd_capacity; i++) {
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
    eventfds[idx].counter = initval;
    eventfds[idx].flags = 0;
    eventfds[idx].semaphore = 0;
    
    mutex_unlock(&eventfd_lock);
    return eventfd_base_fd + idx;
}

static int sys_eventfd2(unsigned int initval, const char *flags_ptr, int unused) {
    int flags = (int)(uintptr_t)flags_ptr;
    (void)unused;
    
    mutex_lock(&eventfd_lock);
    int idx = -1;
    for (int i = 0; i < eventfd_capacity; i++) {
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
    eventfds[idx].counter = initval;
    eventfds[idx].flags = flags;
    eventfds[idx].semaphore = (flags & EFD_SEMAPHORE) ? 1 : 0;
    
    mutex_unlock(&eventfd_lock);
    return eventfd_base_fd + idx;
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
    (void)clockid;
    (void)flags_ptr;
    (void)unused;
    return -ENOSYS;
}

static int sys_timerfd_settime(int fd, const char *flags_ptr, int new_value) {
    (void)fd;
    (void)flags_ptr;
    (void)new_value;
    return -ENOSYS;
}

static int sys_timerfd_gettime(int fd, const char *curr_value_ptr, int unused) {
    (void)fd;
    (void)curr_value_ptr;
    (void)unused;
    return -ENOSYS;
}

static int sys_signalfd(int fd, const char *mask_ptr, int flags) {
    (void)fd;
    (void)mask_ptr;
    (void)flags;
    return -ENOSYS;
}

static int sys_signalfd4(int fd, const char *mask_ptr, int flags) {
    (void)fd;
    (void)mask_ptr;
    (void)flags;
    return -ENOSYS;
}

void syscalls_epoll_init(void) {
    memset(epoll_instances, 0, sizeof(epoll_instances));
    memset(futexes, 0, sizeof(futexes));
    memset(eventfds, 0, sizeof(eventfds));
    mutex_init(&epoll_lock);
    mutex_init(&futex_lock);
    mutex_init(&eventfd_lock);
    
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

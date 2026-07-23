#include "syscall_defs.h"
#include <lebirun/pit.h>
#include <lebirun/mouse.h>
#include <lebirun/evdev.h>

typedef unsigned long fd_mask;
#define NFDBITS (sizeof(fd_mask) * 8)
#define FD_WORDS(n) (((n) + NFDBITS - 1) / NFDBITS)

#define FD_ISSET_DYN(fd, bits) (((bits)[(fd) / NFDBITS] & (1UL << ((fd) % NFDBITS))) != 0)
#define FD_SET_DYN(fd, bits) ((bits)[(fd) / NFDBITS] |= (1UL << ((fd) % NFDBITS)))
#define FD_CLR_DYN(fd, bits) ((bits)[(fd) / NFDBITS] &= ~(1UL << ((fd) % NFDBITS)))

extern volatile uint64_t tick_count;
extern int is_socket_fd(int fd);
extern int socket_poll_events(int fd);
extern int task_has_pending_signals(void);
extern int event_descriptor_poll(int fd);

static int select_interrupted(void) {
    return task_has_pending_signals();
}

static int check_fd_readable(int fd) {
    int con_id;
    int sevents;
    int descriptor_events;
    vfs_node_t *node;
    uint64_t pipe_flags;
    pipe_t *pipe;
    int readable;

    if (fd < 0 || !current_task) return 0;

    if (is_socket_fd(fd)) {
        sevents = socket_poll_events(fd);
        return (sevents & 0x01) || (sevents & 0x10) ? 1 : 0;
    }
    descriptor_events = event_descriptor_poll(fd);
    if (descriptor_events >= 0)
        return (descriptor_events & 0x01) ? 1 : 0;

    if (!current_task->fds || fd >= current_task->fds_capacity) return 0;
    if (!current_task->fds[fd].in_use) {
        if (fd == 0) {
            con_id = (current_task->console_id >= 0) ? current_task->console_id : console_get_current();
            if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;
            return keyboard_has_data_for(con_id) ? 1 : 0;
        }
        return 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_STDIN) {
        con_id = (current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;
        return keyboard_has_data_for(con_id) ? 1 : 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_PIPE_R) {
        pipe = (pipe_t *)current_task->fds[fd].private_data;
        if (!pipe) return 0;
        pipe_flags = pipe_lock_irqsave(pipe);
        readable = pipe->count > 0 || pipe->writers <= 0;
        pipe_unlock_irqrestore(pipe, pipe_flags);
        return readable;
    }
    if (current_task->fds[fd].type == FD_TYPE_FILE) {
        node = (vfs_node_t *)current_task->fds[fd].node;
        if (node && strcmp(node->name, "mice") == 0)
            return mouse_has_data() ? 1 : 0;
        if (node && (strcmp(node->name, "event0") == 0 ||
                     strcmp(node->name, "event1") == 0))
            return evdev_node_has_data(node);
        return 1;
    }
    return 0;
}

static int check_fd_writable(int fd) {
    int sevents;
    int descriptor_events;
    uint64_t pipe_flags;
    pipe_t *pipe;
    int writable;

    if (fd < 0 || !current_task) return 0;

    if (is_socket_fd(fd)) {
        sevents = socket_poll_events(fd);
        return (sevents & 0x04) ? 1 : 0;
    }
    descriptor_events = event_descriptor_poll(fd);
    if (descriptor_events >= 0)
        return (descriptor_events & 0x04) ? 1 : 0;

    if (!current_task->fds || fd >= current_task->fds_capacity) return 0;
    if (!current_task->fds[fd].in_use) {
        if (fd == 1 || fd == 2) return 1;
        return 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_STDOUT || current_task->fds[fd].type == FD_TYPE_STDERR) {
        return 1;
    }
    if (current_task->fds[fd].type == FD_TYPE_PIPE_W) {
        pipe = (pipe_t *)current_task->fds[fd].private_data;
        if (!pipe) return 0;
        pipe_flags = pipe_lock_irqsave(pipe);
        writable = pipe->readers <= 0 || pipe->count < PIPE_BUF_SIZE;
        pipe_unlock_irqrestore(pipe, pipe_flags);
        return writable;
    }
    if (current_task->fds[fd].type == FD_TYPE_FILE) {
        return 1;
    }
    return 0;
}

static int sys_select(int nfds, int readfds_ptr, int writefds_ptr,
                      int exceptfds_ptr, int timeout_ptr, int unused) {
    uint64_t read_addr = (uint64_t)readfds_ptr;
    uint64_t write_addr = (uint64_t)writefds_ptr;
    uint64_t timeout_addr = (uint64_t)timeout_ptr;
    fd_mask *readfds = NULL;
    fd_mask *writefds = NULL;
    fd_mask *in_read = NULL;
    fd_mask *in_write = NULL;
    fd_mask *result_read = NULL;
    fd_mask *result_write = NULL;
    uint64_t words;
    uint64_t set_bytes;
    uint64_t allocation_bytes;
    int timeout_ms;
    uint64_t start_tick;
    uint64_t timeout_ticks;
    int count;
    int fd;
    int descriptor_events;
    struct kernel_timeval timeout_value;

    (void)exceptfds_ptr;
    (void)unused;

    if (nfds < 0) return -EINVAL;
    if (nfds > 4096) nfds = 4096;

    words = FD_WORDS(nfds);
    set_bytes = words * sizeof(fd_mask);
    if (read_addr) {
        readfds = (fd_mask *)read_addr;
        if (!user_access_ok(readfds, set_bytes,
                            UACCESS_READ | UACCESS_WRITE)) return -EFAULT;
    }
    if (write_addr) {
        writefds = (fd_mask *)write_addr;
        if (!user_access_ok(writefds, set_bytes,
                            UACCESS_READ | UACCESS_WRITE)) return -EFAULT;
    }

    allocation_bytes = set_bytes ? set_bytes * 4 : sizeof(fd_mask) * 4;
    in_read = (fd_mask *)kmalloc(allocation_bytes);
    if (!in_read) return -ENOMEM;
    in_write = in_read + words;
    result_read = in_write + words;
    result_write = result_read + words;

    memset(in_read, 0, set_bytes);
    memset(in_write, 0, set_bytes);
    if (readfds && copy_from_user(in_read, readfds, set_bytes) < 0) {
        kfree(in_read);
        return -EFAULT;
    }
    if (writefds && copy_from_user(in_write, writefds, set_bytes) < 0) {
        kfree(in_read);
        return -EFAULT;
    }

    for (fd = 0; fd < nfds; fd++) {
        if ((!readfds || !FD_ISSET_DYN(fd, in_read)) &&
                (!writefds || !FD_ISSET_DYN(fd, in_write))) continue;
        if (is_socket_fd(fd)) continue;
        descriptor_events = event_descriptor_poll(fd);
        if (descriptor_events >= 0) continue;
        if (!current_task || !current_task->fds ||
                fd >= current_task->fds_capacity ||
                !current_task->fds[fd].in_use) {
            kfree(in_read);
            return -EBADF;
        }
    }

    timeout_ms = -1;
    if (timeout_addr) {
        if (copy_from_user(&timeout_value, (const void *)timeout_addr,
                           sizeof(timeout_value)) < 0) {
            kfree(in_read);
            return -EFAULT;
        }
        if (timeout_value.tv_sec < 0 || timeout_value.tv_usec < 0 ||
            timeout_value.tv_usec >= 1000000) {
            kfree(in_read);
            return -EINVAL;
        }
        timeout_ms = (int)(timeout_value.tv_sec * 1000 +
                           timeout_value.tv_usec / 1000);
        if (timeout_ms < 0) timeout_ms = 0;
    }

    start_tick = tick_count;
    timeout_ticks = (timeout_ms > 0) ?
                    (((uint64_t)timeout_ms * pit_freq + 999) / 1000) : 0;
    if (timeout_ms > 0 && timeout_ticks == 0) timeout_ticks = 1;

    do {
        memset(result_read, 0, set_bytes);
        memset(result_write, 0, set_bytes);
        count = 0;

        for (fd = 0; fd < nfds; fd++) {
            if (readfds && FD_ISSET_DYN(fd, in_read)) {
                if (check_fd_readable(fd)) {
                    FD_SET_DYN(fd, result_read);
                    count++;
                }
            }
            if (writefds && FD_ISSET_DYN(fd, in_write)) {
                if (check_fd_writable(fd)) {
                    FD_SET_DYN(fd, result_write);
                    count++;
                }
            }
        }

        if (count > 0 || timeout_ms == 0) {
            break;
        }

        if (timeout_ms > 0 && (tick_count - start_tick) >= timeout_ticks) {
            break;
        }

        if (select_interrupted()) {
            kfree(in_read);
            return -EINTR;
        }
        sleep_ticks(1);
        if (select_interrupted()) {
            kfree(in_read);
            return -EINTR;
        }

    } while (timeout_ms < 0 || (tick_count - start_tick) < timeout_ticks);

    if (readfds && copy_to_user(readfds, result_read, set_bytes) < 0) {
        kfree(in_read);
        return -EFAULT;
    }
    if (writefds && copy_to_user(writefds, result_write, set_bytes) < 0) {
        kfree(in_read);
        return -EFAULT;
    }

    kfree(in_read);
    return count;
}

struct pollfd_k {
    int fd;
    short events;
    short revents;
};

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200

static int sys_poll(int fds_ptr, const char *nfds_ptr, int timeout) {
    uint64_t addr;
    int nfds;
    struct pollfd_k *fds;
    struct pollfd_k value;
    uint64_t start_tick;
    uint64_t timeout_ticks;
    int ready_count;
    int i;
    int curfd;
    int sevents;
    int descriptor_events;

    addr = (uint64_t)fds_ptr;
    nfds = (int)(uintptr_t)nfds_ptr;
    if (!current_task) return -ESRCH;
    if (nfds < 0) return -EINVAL;
    if (nfds != 0 && (!addr || addr >= KERNEL_VMA || addr < 0x1000))
        return -EFAULT;
    if ((uint64_t)nfds > SIZE_MAX / sizeof(struct pollfd_k)) return -EINVAL;

    fds = (struct pollfd_k *)addr;
    if (!user_access_ok(fds, (size_t)nfds * sizeof(struct pollfd_k),
                        UACCESS_READ | UACCESS_WRITE)) return -EFAULT;
    start_tick = tick_count;
    timeout_ticks = (timeout > 0) ?
                    (((uint64_t)timeout * pit_freq + 999) / 1000) : 0;
    if (timeout > 0 && timeout_ticks == 0) timeout_ticks = 1;
    ready_count = 0;

    do {
        ready_count = 0;

        for (i = 0; i < nfds; i++) {
            if (copy_from_user(&value, &fds[i], sizeof(value)) < 0)
                return -EFAULT;
            value.revents = 0;
            curfd = value.fd;

            if (curfd < 0) {
                if (copy_to_user(&fds[i], &value, sizeof(value)) < 0)
                    return -EFAULT;
                continue;
            }

            if (is_socket_fd(curfd)) {
                sevents = socket_poll_events(curfd);
                if ((value.events & (POLLIN | POLLRDNORM)) && (sevents & 0x01)) {
                    value.revents |= POLLIN | POLLRDNORM;
                }
                if ((value.events & (POLLOUT | POLLWRNORM)) && (sevents & 0x04)) {
                    value.revents |= POLLOUT | POLLWRNORM;
                }
                if (sevents & 0x08) value.revents |= POLLERR;
                if (sevents & 0x10) value.revents |= POLLHUP;
                if (value.revents) ready_count++;
                if (copy_to_user(&fds[i], &value, sizeof(value)) < 0)
                    return -EFAULT;
                continue;
            }

            descriptor_events = event_descriptor_poll(curfd);
            if (descriptor_events >= 0) {
                if ((value.events & (POLLIN | POLLRDNORM)) &&
                    (descriptor_events & POLLIN))
                    value.revents |= POLLIN | POLLRDNORM;
                if ((value.events & (POLLOUT | POLLWRNORM)) &&
                    (descriptor_events & POLLOUT))
                    value.revents |= POLLOUT | POLLWRNORM;
                if (descriptor_events & POLLERR) value.revents |= POLLERR;
                if (descriptor_events & POLLHUP) value.revents |= POLLHUP;
                if (value.revents) ready_count++;
                if (copy_to_user(&fds[i], &value, sizeof(value)) < 0)
                    return -EFAULT;
                continue;
            }

            if (!current_task->fds || curfd >= current_task->fds_capacity ||
                    !current_task->fds[curfd].in_use) {
                value.revents = POLLNVAL;
                ready_count++;
                if (copy_to_user(&fds[i], &value, sizeof(value)) < 0)
                    return -EFAULT;
                continue;
            }

            if (value.events & (POLLIN | POLLRDNORM)) {
                if (check_fd_readable(curfd)) {
                    value.revents |= POLLIN | POLLRDNORM;
                }
            }

            if (value.events & (POLLOUT | POLLWRNORM)) {
                if (check_fd_writable(curfd)) {
                    value.revents |= POLLOUT | POLLWRNORM;
                }
            }
            if (value.revents) ready_count++;
            if (copy_to_user(&fds[i], &value, sizeof(value)) < 0)
                return -EFAULT;
        }

        if (ready_count > 0 || timeout == 0) {
            break;
        }

        if (timeout > 0 && (tick_count - start_tick) >= timeout_ticks) {
            break;
        }

        if (select_interrupted()) {
            return -EINTR;
        }
        sleep_ticks(1);
        if (select_interrupted()) {
            return -EINTR;
        }

    } while (timeout < 0 || (tick_count - start_tick) < timeout_ticks);

    return ready_count;
}

static int sys_ppoll(int fds_ptr, const char *nfds_ptr, int timeout_ptr) {
    int timeout_ms;
    uint64_t ts_addr;
    struct kernel_timespec timeout;

    ts_addr = (uint64_t)timeout_ptr;
    timeout_ms = -1;
    if (ts_addr) {
        if (copy_from_user(&timeout, (const void *)ts_addr,
                           sizeof(timeout)) < 0) return -EFAULT;
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 ||
            timeout.tv_nsec >= 1000000000) return -EINVAL;
        timeout_ms = (int)(timeout.tv_sec * 1000 +
                           timeout.tv_nsec / 1000000);
        if (timeout_ms < 0) timeout_ms = 0;
    }
    return sys_poll(fds_ptr, nfds_ptr, timeout_ms);
}

void syscalls_select_init(void) {
    syscall_table[SYSCALL_SELECT] = sys_select;
    syscall_table[SYSCALL_POLL] = sys_poll;
    syscall_table[SYSCALL_PPOLL] = sys_ppoll;
}

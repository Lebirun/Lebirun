#include "syscall_defs.h"
#include <lebirun/pit.h>

typedef unsigned long fd_mask;
#define NFDBITS (sizeof(fd_mask) * 8)
#define FD_WORDS(n) (((n) + NFDBITS - 1) / NFDBITS)

#define FD_ISSET_DYN(fd, bits) (((bits)[(fd) / NFDBITS] & (1UL << ((fd) % NFDBITS))) != 0)
#define FD_SET_DYN(fd, bits) ((bits)[(fd) / NFDBITS] |= (1UL << ((fd) % NFDBITS)))
#define FD_CLR_DYN(fd, bits) ((bits)[(fd) / NFDBITS] &= ~(1UL << ((fd) % NFDBITS)))

extern volatile uint64_t tick_count;
extern int is_socket_fd(int fd);
extern int socket_poll_events(int fd);

static int check_fd_readable(int fd) {
    int con_id;
    int sevents;

    if (fd < 0 || !current_task) return 0;

    if (is_socket_fd(fd)) {
        sevents = socket_poll_events(fd);
        return (sevents & 0x01) || (sevents & 0x10) ? 1 : 0;
    }

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
        pipe_t *p = (pipe_t *)current_task->fds[fd].private_data;
        if (!p) return 0;
        if (p->count > 0) return 1;
        if (p->writers <= 0) return 1;
        return 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_FILE) {
        return 1;
    }
    return 0;
}

static int check_fd_writable(int fd) {
    int sevents;

    if (fd < 0 || !current_task) return 0;

    if (is_socket_fd(fd)) {
        sevents = socket_poll_events(fd);
        return (sevents & 0x04) ? 1 : 0;
    }

    if (!current_task->fds || fd >= current_task->fds_capacity) return 0;
    if (!current_task->fds[fd].in_use) {
        if (fd == 1 || fd == 2) return 1;
        return 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_STDOUT || current_task->fds[fd].type == FD_TYPE_STDERR) {
        return 1;
    }
    if (current_task->fds[fd].type == FD_TYPE_PIPE_W) {
        pipe_t *p = (pipe_t *)current_task->fds[fd].private_data;
        if (!p) return 0;
        if (p->readers <= 0) return 1;
        if (p->count < p->buf_size) return 1;
        return 0;
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
    int timeout_ms;
    uint64_t start_tick;
    uint64_t timeout_ticks;
    int count;
    int fd;

    (void)exceptfds_ptr;
    (void)unused;

    if (nfds < 0) return -EINVAL;
    if (nfds > 4096) nfds = 4096;

    if (read_addr && read_addr < KERNEL_VMA && read_addr >= 0x1000) {
        readfds = (fd_mask *)read_addr;
    }
    if (write_addr && write_addr < KERNEL_VMA && write_addr >= 0x1000) {
        writefds = (fd_mask *)write_addr;
    }

    words = FD_WORDS(nfds);
    set_bytes = words * sizeof(fd_mask);

    in_read = (fd_mask *)kmalloc(set_bytes * 4);
    if (!in_read) return -ENOMEM;
    in_write = in_read + words;
    result_read = in_write + words;
    result_write = result_read + words;

    memset(in_read, 0, set_bytes);
    memset(in_write, 0, set_bytes);
    if (readfds) memcpy(in_read, readfds, set_bytes);
    if (writefds) memcpy(in_write, writefds, set_bytes);

    timeout_ms = -1;
    if (timeout_addr && timeout_addr < KERNEL_VMA && timeout_addr >= 0x1000) {
        struct kernel_timeval *tv = (struct kernel_timeval *)timeout_addr;
        timeout_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
        if (timeout_ms < 0) timeout_ms = 0;
    }

    start_tick = tick_count;
    timeout_ticks = (timeout_ms > 0) ? ((uint64_t)timeout_ms * pit_freq / 1000) : 0;

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

        schedule();

    } while (timeout_ms < 0 || (tick_count - start_tick) < timeout_ticks);

    if (readfds) memcpy(readfds, result_read, set_bytes);
    if (writefds) memcpy(writefds, result_write, set_bytes);

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
    uint64_t addr = (uint64_t)fds_ptr;
    int nfds = (int)(uintptr_t)nfds_ptr;
    struct pollfd_k *fds;
    uint64_t start_tick;
    uint64_t timeout_ticks;
    int ready_count;
    int i;
    int curfd;
    int sevents;

    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    if (nfds < 0) return -EINVAL;

    fds = (struct pollfd_k *)addr;
    start_tick = tick_count;
    timeout_ticks = (timeout > 0) ? ((uint64_t)timeout * pit_freq / 1000) : 0;
    ready_count = 0;

    do {
        ready_count = 0;

        for (i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            curfd = fds[i].fd;

            if (curfd < 0) {
                continue;
            }

            if (is_socket_fd(curfd)) {
                sevents = socket_poll_events(curfd);
                if ((fds[i].events & (POLLIN | POLLRDNORM)) && (sevents & 0x01)) {
                    fds[i].revents |= POLLIN | POLLRDNORM;
                }
                if ((fds[i].events & (POLLOUT | POLLWRNORM)) && (sevents & 0x04)) {
                    fds[i].revents |= POLLOUT | POLLWRNORM;
                }
                if (sevents & 0x08) fds[i].revents |= POLLERR;
                if (sevents & 0x10) fds[i].revents |= POLLHUP;
                if (fds[i].revents) ready_count++;
                continue;
            }

            if (curfd >= current_task->fds_capacity) {
                fds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }

            if (fds[i].events & (POLLIN | POLLRDNORM)) {
                if (check_fd_readable(curfd)) {
                    fds[i].revents |= POLLIN | POLLRDNORM;
                    ready_count++;
                }
            }

            if (fds[i].events & (POLLOUT | POLLWRNORM)) {
                if (check_fd_writable(curfd)) {
                    fds[i].revents |= POLLOUT | POLLWRNORM;
                    ready_count++;
                }
            }
        }

        if (ready_count > 0 || timeout == 0) {
            break;
        }

        if (timeout > 0 && (tick_count - start_tick) >= timeout_ticks) {
            break;
        }

        schedule();

    } while (timeout < 0 || (tick_count - start_tick) < timeout_ticks);

    return ready_count;
}

static int sys_ppoll(int fds_ptr, const char *nfds_ptr, int timeout_ptr) {
    int timeout_ms;
    uint64_t ts_addr;

    ts_addr = (uint64_t)timeout_ptr;
    timeout_ms = -1;
    if (ts_addr && ts_addr < KERNEL_VMA && ts_addr >= 0x1000) {
        struct kernel_timespec *ts = (struct kernel_timespec *)ts_addr;
        timeout_ms = (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
        if (timeout_ms < 0) timeout_ms = 0;
    }
    return sys_poll(fds_ptr, nfds_ptr, timeout_ms);
}

void syscalls_select_init(void) {
    syscall_table[SYSCALL_SELECT] = sys_select;
    syscall_table[SYSCALL_POLL] = sys_poll;
    syscall_table[SYSCALL_PPOLL] = sys_ppoll;
}

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
            if (con_id < 0 || con_id >= console_get_count()) con_id = 0;
            return keyboard_has_data_for(con_id) ? 1 : 0;
        }
        return 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_STDIN) {
        con_id = (current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        if (con_id < 0 || con_id >= console_get_count()) con_id = 0;
        return keyboard_has_data_for(con_id) ? 1 : 0;
    }
    if (current_task->fds[fd].type == FD_TYPE_PIPE_R ||
        current_task->fds[fd].type == FD_TYPE_PIPE_RW) {
        pipe = (pipe_t *)current_task->fds[fd].private_data;
        if (!pipe) return 0;
        pipe_flags = pipe_lock_irqsave(pipe);
        readable = pipe->count > 0 || pipe->writers <= 0;
        pipe_unlock_irqrestore(pipe, pipe_flags);
        return readable;
    }
    if (current_task->fds[fd].type == FD_TYPE_FILE) {
        node = (vfs_node_t *)current_task->fds[fd].node;
        if (node && strcmp(vfs_node_name(node), "mice") == 0)
            return mouse_has_data() ? 1 : 0;
        if (node && (strcmp(vfs_node_name(node), "event0") == 0 ||
                     strcmp(vfs_node_name(node), "event1") == 0))
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
    if (current_task->fds[fd].type == FD_TYPE_PIPE_W ||
        current_task->fds[fd].type == FD_TYPE_PIPE_RW) {
        pipe = (pipe_t *)current_task->fds[fd].private_data;
        if (!pipe) return 0;
        pipe_flags = pipe_lock_irqsave(pipe);
        writable = pipe->readers <= 0 || pipe->count < UINT64_MAX;
        pipe_unlock_irqrestore(pipe, pipe_flags);
        return writable;
    }
    if (current_task->fds[fd].type == FD_TYPE_FILE) {
        return 1;
    }
    return 0;
}

static int select_common(int nfds, uint64_t readfds_ptr,
                         uint64_t writefds_ptr,
                         int timeout_ms) {
    uint64_t read_addr;
    uint64_t write_addr;
    fd_mask *readfds;
    fd_mask *writefds;
    fd_mask read_word;
    fd_mask write_word;
    fd_mask result_read_word;
    fd_mask result_write_word;
    uint64_t words;
    uint64_t set_bytes;
    uint64_t word_index;
    uint64_t start_tick;
    uint64_t timeout_ticks;
    int count;
    int fd;
    int bit;
    int fd_ready;
    int descriptor_events;
    uint64_t ready_generation;
    uint64_t elapsed_ticks;
    uint64_t wait_ticks;

    read_addr = (uint64_t)readfds_ptr;
    write_addr = (uint64_t)writefds_ptr;
    readfds = NULL;
    writefds = NULL;
    if (nfds < 0) return -EINVAL;

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

    for (word_index = 0; word_index < words; word_index++) {
        read_word = 0;
        write_word = 0;
        if (readfds && copy_from_user(&read_word, &readfds[word_index],
                                      sizeof(read_word)) < 0)
            return -EFAULT;
        if (writefds && copy_from_user(&write_word, &writefds[word_index],
                                       sizeof(write_word)) < 0)
            return -EFAULT;
        for (bit = 0; bit < (int)NFDBITS; bit++) {
            fd = (int)(word_index * NFDBITS + (uint64_t)bit);
            if (fd >= nfds) break;
            if (!(read_word & (1UL << bit)) &&
                !(write_word & (1UL << bit))) continue;
            if (is_socket_fd(fd)) continue;
            descriptor_events = event_descriptor_poll(fd);
            if (descriptor_events >= 0) continue;
            if (!current_task || !current_task->fds ||
                    fd >= current_task->fds_capacity ||
                    !current_task->fds[fd].in_use)
                return -EBADF;
        }
    }

    start_tick = tick_count;
    timeout_ticks = (timeout_ms > 0) ?
                    (((uint64_t)timeout_ms * pit_freq + 999) / 1000) : 0;
    if (timeout_ms > 0 && timeout_ticks == 0) timeout_ticks = 1;

    do {
        ready_generation = descriptor_ready_generation();
        count = 0;

        for (word_index = 0; word_index < words; word_index++) {
            read_word = 0;
            write_word = 0;
            if (readfds && copy_from_user(&read_word,
                    &readfds[word_index], sizeof(read_word)) < 0)
                return -EFAULT;
            if (writefds && copy_from_user(&write_word,
                    &writefds[word_index], sizeof(write_word)) < 0)
                return -EFAULT;
            for (bit = 0; bit < (int)NFDBITS; bit++) {
                fd = (int)(word_index * NFDBITS + (uint64_t)bit);
                if (fd >= nfds) break;
                fd_ready = 0;
                if ((read_word & (1UL << bit)) &&
                    check_fd_readable(fd)) fd_ready = 1;
                if ((write_word & (1UL << bit)) &&
                    check_fd_writable(fd)) fd_ready = 1;
                if (fd_ready && count < 0x7FFFFFFF) count++;
            }
        }

        if (count > 0 || timeout_ms == 0) {
            break;
        }

        if (timeout_ms > 0 && (tick_count - start_tick) >= timeout_ticks) {
            break;
        }

        if (select_interrupted()) {
            return -EINTR;
        }
        wait_ticks = UINT64_MAX;
        if (timeout_ms > 0) {
            elapsed_ticks = tick_count - start_tick;
            if (elapsed_ticks >= timeout_ticks) break;
            wait_ticks = timeout_ticks - elapsed_ticks;
        }
        wait_ticks = event_descriptor_wait_timeout(wait_ticks);
        descriptor_ready_wait(ready_generation, wait_ticks);
        if (select_interrupted()) {
            return -EINTR;
        }

    } while (timeout_ms < 0 || (tick_count - start_tick) < timeout_ticks);

    count = 0;
    for (word_index = 0; word_index < words; word_index++) {
        read_word = 0;
        write_word = 0;
        result_read_word = 0;
        result_write_word = 0;
        if (readfds && copy_from_user(&read_word, &readfds[word_index],
                                      sizeof(read_word)) < 0)
            return -EFAULT;
        if (writefds && copy_from_user(&write_word, &writefds[word_index],
                                       sizeof(write_word)) < 0)
            return -EFAULT;
        for (bit = 0; bit < (int)NFDBITS; bit++) {
            fd = (int)(word_index * NFDBITS + (uint64_t)bit);
            if (fd >= nfds) break;
            fd_ready = 0;
            if ((read_word & (1UL << bit)) && check_fd_readable(fd)) {
                result_read_word |= 1UL << bit;
                fd_ready = 1;
            }
            if ((write_word & (1UL << bit)) && check_fd_writable(fd)) {
                result_write_word |= 1UL << bit;
                fd_ready = 1;
            }
            if (fd_ready && count < 0x7FFFFFFF) count++;
        }
        if (readfds && copy_to_user(&readfds[word_index], &result_read_word,
                                    sizeof(result_read_word)) < 0)
            return -EFAULT;
        if (writefds && copy_to_user(&writefds[word_index], &result_write_word,
                                     sizeof(result_write_word)) < 0)
            return -EFAULT;
    }

    return count;
}

static int sys_select(int nfds, uint64_t readfds_ptr,
                      uint64_t writefds_ptr, uint64_t exceptfds_ptr,
                      uint64_t timeout_ptr, int unused) {
    struct kernel_timeval timeout_value;
    int timeout_ms;

    (void)exceptfds_ptr;
    (void)unused;
    timeout_ms = -1;
    if (timeout_ptr) {
        if (copy_from_user(&timeout_value,
                           (const void *)(uintptr_t)timeout_ptr,
                           sizeof(timeout_value)) < 0) return -EFAULT;
        if (timeout_value.tv_sec < 0 || timeout_value.tv_usec < 0 ||
            timeout_value.tv_usec >= 1000000) return -EINVAL;
        timeout_ms = (int)(timeout_value.tv_sec * 1000 +
                           timeout_value.tv_usec / 1000);
        if (timeout_ms < 0) timeout_ms = 0;
    }
    return select_common(nfds, readfds_ptr, writefds_ptr, timeout_ms);
}

static int sys_pselect6(int nfds, uint64_t readfds_ptr,
                        uint64_t writefds_ptr, uint64_t exceptfds_ptr,
                        uint64_t timeout_ptr, uint64_t sigmask_ptr) {
    struct kernel_timespec timeout_value;
    uint64_t milliseconds;
    int timeout_ms;

    (void)exceptfds_ptr;
    (void)sigmask_ptr;
    timeout_ms = -1;
    if (timeout_ptr) {
        if (copy_from_user(&timeout_value,
                           (const void *)(uintptr_t)timeout_ptr,
                           sizeof(timeout_value)) < 0) return -EFAULT;
        if (timeout_value.tv_sec < 0 || timeout_value.tv_nsec < 0 ||
            timeout_value.tv_nsec >= 1000000000) return -EINVAL;
        milliseconds = (uint64_t)timeout_value.tv_sec * 1000;
        milliseconds += ((uint64_t)timeout_value.tv_nsec + 999999) /
                        1000000;
        timeout_ms = milliseconds > 0x7FFFFFFFULL ? 0x7FFFFFFF :
                     (int)milliseconds;
    }
    return select_common(nfds, readfds_ptr, writefds_ptr, timeout_ms);
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

static int sys_poll(uint64_t fds_ptr, const char *nfds_ptr, int timeout) {
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
    uint64_t ready_generation;
    uint64_t elapsed_ticks;
    uint64_t wait_ticks;

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
        ready_generation = descriptor_ready_generation();
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
                    value.revents |= value.events & (POLLIN | POLLRDNORM);
                }
                if ((value.events & (POLLOUT | POLLWRNORM)) && (sevents & 0x04)) {
                    value.revents |= value.events & (POLLOUT | POLLWRNORM);
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
                    value.revents |= value.events & (POLLIN | POLLRDNORM);
                if ((value.events & (POLLOUT | POLLWRNORM)) &&
                    (descriptor_events & POLLOUT))
                    value.revents |= value.events & (POLLOUT | POLLWRNORM);
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
                    value.revents |= value.events & (POLLIN | POLLRDNORM);
                }
            }

            if (value.events & (POLLOUT | POLLWRNORM)) {
                if (check_fd_writable(curfd)) {
                    value.revents |= value.events & (POLLOUT | POLLWRNORM);
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
        wait_ticks = UINT64_MAX;
        if (timeout > 0) {
            elapsed_ticks = tick_count - start_tick;
            if (elapsed_ticks >= timeout_ticks) break;
            wait_ticks = timeout_ticks - elapsed_ticks;
        }
        wait_ticks = event_descriptor_wait_timeout(wait_ticks);
        descriptor_ready_wait(ready_generation, wait_ticks);
        if (select_interrupted()) {
            return -EINTR;
        }

    } while (timeout < 0 || (tick_count - start_tick) < timeout_ticks);

    return ready_count;
}

static int sys_ppoll(uint64_t fds_ptr, const char *nfds_ptr,
                     uint64_t timeout_ptr) {
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
    syscall_table_set(SYSCALL_SELECT, (void *)(sys_select));
    syscall_table_set(SYSCALL_PSELECT6, (void *)(sys_pselect6));
    syscall_table_set(SYSCALL_POLL, (void *)(sys_poll));
    syscall_table_set(SYSCALL_PPOLL, (void *)(sys_ppoll));
}

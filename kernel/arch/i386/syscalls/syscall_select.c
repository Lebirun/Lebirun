#include "syscall_defs.h"
#include <kernel/pit.h>

#define FD_SETSIZE 64
typedef unsigned long fd_mask;
#define NFDBITS (sizeof(fd_mask) * 8)

typedef struct {
    fd_mask fds_bits[FD_SETSIZE / NFDBITS];
} fd_set_k;

#define FD_ISSET_K(fd, set) (((set)->fds_bits[(fd) / NFDBITS] & (1UL << ((fd) % NFDBITS))) != 0)
#define FD_SET_K(fd, set) ((set)->fds_bits[(fd) / NFDBITS] |= (1UL << ((fd) % NFDBITS)))
#define FD_CLR_K(fd, set) ((set)->fds_bits[(fd) / NFDBITS] &= ~(1UL << ((fd) % NFDBITS)))
#define FD_ZERO_K(set) memset((set), 0, sizeof(fd_set_k))

extern volatile uint32_t tick_count;

static int check_fd_readable(int fd) {
    if (fd < 0 || fd >= TASK_MAX_FDS) return 0;
    if (fd == 0) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;
        return keyboard_has_data_for(con_id) ? 1 : 0;
    }
    if (current_task && current_task->fds[fd].in_use) {
        if (current_task->fds[fd].type == FD_TYPE_PIPE_R) {
            return 1;
        }
        return 1;
    }
    return 0;
}

static int check_fd_writable(int fd) {
    if (fd < 0 || fd >= TASK_MAX_FDS) return 0;
    if (fd == 1 || fd == 2) return 1;
    if (current_task && current_task->fds[fd].in_use) {
        if (current_task->fds[fd].type == FD_TYPE_PIPE_W) {
            return 1;
        }
        return 1;
    }
    return 0;
}

static int sys_select(int nfds, const char *readfds_ptr, int writefds_ptr) {
    uint32_t read_addr = (uint32_t)(uintptr_t)readfds_ptr;
    uint32_t write_addr = (uint32_t)writefds_ptr;
    
    fd_set_k *readfds = NULL;
    fd_set_k *writefds = NULL;
    
    if (read_addr && read_addr < 0xC0000000 && read_addr >= 0x1000) {
        readfds = (fd_set_k *)read_addr;
    }
    if (write_addr && write_addr < 0xC0000000 && write_addr >= 0x1000) {
        writefds = (fd_set_k *)write_addr;
    }
    
    if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;
    if (nfds < 0) return -EINVAL;
    
    fd_set_k result_read, result_write;
    FD_ZERO_K(&result_read);
    FD_ZERO_K(&result_write);
    
    int count = 0;
    
    for (int fd = 0; fd < nfds; fd++) {
        if (readfds && FD_ISSET_K(fd, readfds)) {
            if (check_fd_readable(fd)) {
                FD_SET_K(fd, &result_read);
                count++;
            }
        }
        if (writefds && FD_ISSET_K(fd, writefds)) {
            if (check_fd_writable(fd)) {
                FD_SET_K(fd, &result_write);
                count++;
            }
        }
    }
    
    if (readfds) memcpy(readfds, &result_read, sizeof(fd_set_k));
    if (writefds) memcpy(writefds, &result_write, sizeof(fd_set_k));
    
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
    uint32_t addr = (uint32_t)fds_ptr;
    int nfds = (int)(uintptr_t)nfds_ptr;
    
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    if (nfds < 0) return -EINVAL;
    
    struct pollfd_k *fds = (struct pollfd_k *)addr;
    
    uint32_t start_tick = tick_count;
    uint32_t timeout_ticks = (timeout > 0) ? ((uint32_t)timeout * pit_freq / 1000) : 0;
    
    int ready_count = 0;
    
    do {
        ready_count = 0;
        
        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            
            if (fds[i].fd < 0) {
                continue;
            }
            
            if (fds[i].fd >= TASK_MAX_FDS) {
                fds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }
            
            if (fds[i].events & (POLLIN | POLLRDNORM)) {
                if (check_fd_readable(fds[i].fd)) {
                    fds[i].revents |= POLLIN | POLLRDNORM;
                    ready_count++;
                }
            }
            
            if (fds[i].events & (POLLOUT | POLLWRNORM)) {
                if (check_fd_writable(fds[i].fd)) {
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
    (void)timeout_ptr;
    return sys_poll(fds_ptr, nfds_ptr, -1);
}

void syscalls_select_init(void) {
    syscall_table[SYSCALL_SELECT] = sys_select;
    syscall_table[SYSCALL_POLL] = sys_poll;
    syscall_table[SYSCALL_PPOLL] = sys_ppoll;
}

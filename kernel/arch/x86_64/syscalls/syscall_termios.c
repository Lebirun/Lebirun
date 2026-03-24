#include "syscall_defs.h"
#include <kernel/creds.h>
#include <kernel/pty.h>

static int kernel_ptr_mapped(uint64_t addr) {
    if (addr < KERNEL_VMA) return 0;
    return vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), addr) != 0;
}

static int vfs_node_ptr_sane(vfs_node_t *node) {
    if (!node) return 0;
    uint64_t a = (uint64_t)node;
    if ((a & 0xFFFFFF00) == 0xFEFEFE00) return 0;
    if (a < KERNEL_VMA || a >= 0xFFFFFFFFFFFFFF00ULL) return 0;
    if (!kernel_ptr_mapped(a)) return 0;
    if (!kernel_ptr_mapped(a + (uint64_t)sizeof(vfs_node_t) - 1)) return 0;
    return 1;
}

static int user_range_mapped(uint64_t addr, uint64_t size) {
    if (!current_task) return 0;
    if (size == 0) return 1;
    uint64_t end = addr + size - 1;
    if (end < addr) return 0;
    if (addr < 0x1000 || end >= KERNEL_VMA) return 0;

    uint64_t p = addr & ~0xFFFu;
    uint64_t pend = end & ~0xFFFu;
    for (;;) {
        if (vmm_get_phys_in_pml4(current_task->cr3, p) == 0) return 0;
        if (p == pend) break;
        if (p > 0xFFFFFFFFFFFFF000ULL) return 0;
        p += 0x1000u;
    }
    return 1;
}

static int vfs_name_is(const char name[VFS_MAX_NAME], const char *lit) {
    size_t n = strlen(lit);
    if (n >= VFS_MAX_NAME) return 0;
    if (memcmp(name, lit, n) != 0) return 0;
    return name[n] == '\0';
}

struct kernel_termios tty_termios[NUM_CONSOLES];
struct kernel_winsize tty_winsize[NUM_CONSOLES];
int tty_pgrp[NUM_CONSOLES];

static int ioctl_fcntl_dupfd_compat(int oldfd, int cmd, int minfd) {
    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !current_task->fds[oldfd].in_use) return -EBADF;
    if (minfd < 0) minfd = 0;
    if (minfd >= TASK_MAX_FDS) return -EINVAL;

    int newfd = -1;
    for (int i = minfd; i < current_task->fds_capacity; i++) {
        if (!current_task->fds[i].in_use) {
            newfd = i;
            current_task->fds[i].in_use = 1;
            current_task->fds[i].ref_count = 1;
            break;
        }
    }
    if (newfd < 0) return -EMFILE;

    memcpy(&current_task->fds[newfd], &current_task->fds[oldfd], sizeof(task_fd_t));
    current_task->fds[newfd].ref_count = 1;

    if (current_task->fds[oldfd].private_data &&
        (current_task->fds[oldfd].type == FD_TYPE_PIPE_R || current_task->fds[oldfd].type == FD_TYPE_PIPE_W)) {
        pipe_t *p = (pipe_t *)current_task->fds[oldfd].private_data;
        if (current_task->fds[oldfd].type == FD_TYPE_PIPE_R) p->readers++;
        else p->writers++;
    }

    (void)cmd;
    return newfd;
}

static int ioctl_fcntl_basic_compat(int fd, int cmd, int arg) {
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity || !current_task->fds[fd].in_use) return -EBADF;

    switch (cmd) {
        case 1: 
            return (current_task->fds[fd].flags & 1) ? 1 : 0;
        case 2: 
            if (arg & 1) current_task->fds[fd].flags |= 1;
            else current_task->fds[fd].flags &= ~1u;
            return 0;
        case 3: 
            return (int)(current_task->fds[fd].flags & ~1u);
        case 4: 
            current_task->fds[fd].flags = (current_task->fds[fd].flags & 1) | ((uint64_t)arg & ~1u);
            return 0;
        default:
            return -EINVAL;
    }
}

static void termios_init_defaults(int tty_id) {
    struct kernel_termios *t = &tty_termios[tty_id];
    
    t->c_iflag = ICRNL | IXON;
    
    t->c_oflag = OPOST | ONLCR;
    
    t->c_cflag = CS8 | CREAD | CLOCAL;
    
    t->c_lflag = ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ISIG | IEXTEN;
    
    memset(t->c_cc, 0, NCCS);
    t->c_cc[VEOF]   = 4;
    t->c_cc[VEOL]   = 0;
    t->c_cc[VERASE] = 127;
    t->c_cc[VKILL]  = 21;
    t->c_cc[VINTR]  = 3;
    t->c_cc[VQUIT]  = 28;
    t->c_cc[VSUSP]  = 26;
    t->c_cc[VSTART] = 17;
    t->c_cc[VSTOP]  = 19;
    t->c_cc[VMIN]   = 1;
    t->c_cc[VTIME]  = 0;
    
    t->c_ispeed = 38400;
    t->c_ospeed = 38400;
    
    framebuffer_t *fb = fb_get();
    if (fb && (fb->font || fb->cols)) {
        tty_winsize[tty_id].ws_col = fb->cols;
        tty_winsize[tty_id].ws_row = fb->rows;
        tty_winsize[tty_id].ws_xpixel = fb->width;
        tty_winsize[tty_id].ws_ypixel = fb->height;
    } else {
        tty_winsize[tty_id].ws_col = 80;
        tty_winsize[tty_id].ws_row = 25;
        tty_winsize[tty_id].ws_xpixel = 0;
        tty_winsize[tty_id].ws_ypixel = 0;
    }
    
    tty_pgrp[tty_id] = 0;
}

static int get_tty_id_for_fd(int fd) {
    if (fd < 0) return -1;
    if (!current_task) return (fd >= 0 && fd <= 2) ? console_get_current() : -1;
    if (fd >= current_task->fds_capacity) return -1;
    if (!current_task->fds[fd].in_use) {
        if (fd >= 0 && fd <= 2) return (current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        return -1;
    }
    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type == FD_TYPE_STDIN || tfd->type == FD_TYPE_STDOUT || tfd->type == FD_TYPE_STDERR) {
        if (current_task->console_id >= 0) return current_task->console_id;
        return console_get_current();
    }
    if (tfd->type == FD_TYPE_FILE && tfd->node) {
        vfs_node_t *node = (vfs_node_t *)tfd->node;
        if (!vfs_node_ptr_sane(node)) {
            return -1;
        }
        if (VFS_GET_TYPE(node->flags) == VFS_CHARDEVICE &&
            (vfs_name_is(node->name, "tty") || vfs_name_is(node->name, "console"))) {
            if (current_task->console_id >= 0) return current_task->console_id;
            return console_get_current();
        }
    }
    return -1;
}

static int sys_tcgetattr(int fd, const char *termios_ptr, int unused) {
    (void)unused;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0 || tty_id >= NUM_CONSOLES) return -ENOTTY;
    
    uint64_t addr = (uint64_t)termios_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    memcpy((void*)addr, &tty_termios[tty_id], sizeof(struct kernel_termios));
    return 0;
}

static int sys_tcsetattr(int fd, const char *actions_ptr, int termios_ptr) {
    int actions = (int)(uintptr_t)actions_ptr;
    (void)actions;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0 || tty_id >= NUM_CONSOLES) return -ENOTTY;
    
    uint64_t addr = (uint64_t)termios_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    memcpy(&tty_termios[tty_id], (void*)addr, sizeof(struct kernel_termios));
    return 0;
}

static int sys_ioctl(int fd, const char *request_ptr, int arg) {
    unsigned long request;
    int tty_id;
    int pty_fd;

    request = (unsigned long)(uintptr_t)request_ptr;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) {
        return -EBADF;
    }

    if (current_task->fds[fd].in_use) {
        task_fd_t *tfd = &current_task->fds[fd];
        uint64_t node_addr = (uint64_t)tfd->node;
        if (node_addr && ((node_addr & 0xFFFFFF00) == 0xFEFEFE00)) {
            tfd->node = NULL;
        }
    } else if (current_task) {
        return -EBADF;
    }

    if (request == 0ul || request == 0x406ul) {
        return ioctl_fcntl_dupfd_compat(fd, (int)request, arg);
    }
    if (request == 1ul || request == 2ul || request == 3ul || request == 4ul) {
        return ioctl_fcntl_basic_compat(fd, (int)request, arg);
    }

    if (current_task->fds[fd].in_use && current_task->fds[fd].node) {
        vfs_node_t *vn = (vfs_node_t *)current_task->fds[fd].node;
        if (vn->ioctl) {
            return vn->ioctl(vn, request, (void *)(uintptr_t)arg);
        }
    }

    if (current_task->fds[fd].in_use && current_task->fds[fd].private_data) {
        pty_fd = (int)(uintptr_t)current_task->fds[fd].private_data;
        if (is_pty_master(pty_fd) || is_pty_slave(pty_fd)) {
            return pty_ioctl(pty_fd, request, (void *)(uintptr_t)arg);
        }
    }

    tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0 || tty_id >= NUM_CONSOLES) tty_id = -1;
    
    switch (request) {
        case TIOCSCTTY:
            if (tty_id < 0) return -ENOTTY;
            if (current_task && tty_pgrp[tty_id] == 0) {
                tty_pgrp[tty_id] = current_task->pid;
            }
            return 0;

        case TIOCNOTTY:
            if (tty_id < 0) return -ENOTTY;
            (void)arg;
            return 0;

        case TIOCGSID:

            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            *(int *)(uintptr_t)arg = (int)creds_get_sid(0);
            return 0;

        case TIOCGETA:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct kernel_termios))) return -EFAULT;
            memcpy((void*)(uintptr_t)arg, &tty_termios[tty_id], sizeof(struct kernel_termios));
            return 0;
            
        case TIOCSETA:
        case TIOCSETAW:
        case TIOCSETAF:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct kernel_termios))) return -EFAULT;
            memcpy(&tty_termios[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_termios));
            return 0;
            
        case TIOCGWINSZ:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct kernel_winsize))) return -EFAULT;
            {
                framebuffer_t *fb = fb_get();
                if (fb && (fb->font || fb->cols)) {
                    tty_winsize[tty_id].ws_col = fb->cols;
                    tty_winsize[tty_id].ws_row = fb->rows;
                    tty_winsize[tty_id].ws_xpixel = fb->width;
                    tty_winsize[tty_id].ws_ypixel = fb->height;
                }
            }
            memcpy((void*)(uintptr_t)arg, &tty_winsize[tty_id], sizeof(struct kernel_winsize));
            return 0;
            
        case TIOCSWINSZ:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct kernel_winsize))) return -EFAULT;
            memcpy(&tty_winsize[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_winsize));
            {
                int pgrp = tty_pgrp[tty_id];
                if (pgrp > 0) {
                    pid_t pids[64];
                    int npids;
                    int si;

                    npids = collect_pids_in_pgrp((pid_t)pgrp, pids, 64);
                    for (si = 0; si < npids; si++) {
                        task_t *t = task_find(pids[si]);
                        if (t) deliver_signal_to_task(t, 28);
                    }
                }
            }
            return 0;
            
        case TIOCGPGRP:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            {
                int pgrp = tty_pgrp[tty_id];
                if (pgrp == 0 && current_task) {
                    pgrp = current_task->pgid ? current_task->pgid : current_task->pid;
                    if (pgrp == 0) pgrp = 1;
                }

                *(int*)(uintptr_t)arg = pgrp;
            }
            return 0;
            
        case TIOCSPGRP:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            tty_pgrp[tty_id] = *(int *)(uintptr_t)arg;
            return 0;
            
        case FIONREAD:
            if (fd == 0 && tty_id >= 0) {
                if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
                *(int*)(uintptr_t)arg = keyboard_has_data_for(tty_id) ? 1 : 0;
                return 0;
            }
            return -ENOTTY;
            
        case FIONBIO:
            return 0;
            
        default:
            return -EINVAL;
    }
}

static int sys_tcflush(int fd, const char *queue_ptr, int unused) {
    (void)unused;
    int queue = (int)(uintptr_t)queue_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -ENOTTY;
    
    (void)queue;
    return 0;
}

static int sys_tcflow(int fd, const char *action_ptr, int unused) {
    (void)unused;
    int action = (int)(uintptr_t)action_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -ENOTTY;
    
    (void)action;
    return 0;
}

static int sys_tcdrain(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -ENOTTY;
    
    return 0;
}

static int sys_tcgetpgrp(int fd, const char *unused1, int unused2) {
    int tty_id;
    int pgrp;

    (void)unused1; (void)unused2;
    
    tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -ENOTTY;

    pgrp = tty_pgrp[tty_id];
    if (pgrp == 0 && current_task) {
        pgrp = current_task->pgid ? current_task->pgid : current_task->pid;
        if (pgrp == 0) pgrp = 1;
    }

    return pgrp;
}

static int sys_tcsetpgrp(int fd, const char *pgrp_ptr, int unused) {
    (void)unused;
    int pgrp = (int)(uintptr_t)pgrp_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -ENOTTY;
    
    tty_pgrp[tty_id] = pgrp;
    return 0;
}

void syscalls_termios_init(void) {
    syscall_table[SYSCALL_TCGETATTR] = sys_tcgetattr;
    syscall_table[SYSCALL_TCSETATTR] = sys_tcsetattr;
    syscall_table[SYSCALL_IOCTL] = sys_ioctl;
    syscall_table[SYSCALL_TCFLUSH] = sys_tcflush;
    syscall_table[SYSCALL_TCFLOW] = sys_tcflow;
    syscall_table[SYSCALL_TCDRAIN] = sys_tcdrain;
    syscall_table[SYSCALL_TCGETPGRP] = sys_tcgetpgrp;
    syscall_table[SYSCALL_TCSETPGRP] = sys_tcsetpgrp;
    
    for (int i = 0; i < NUM_CONSOLES; i++) {
        termios_init_defaults(i);
    }
}

#include "syscall_defs.h"
#include <lebirun/creds.h>
#include <lebirun/pty.h>

static int kernel_ptr_mapped(uint64_t addr) {
    if (addr < KERNEL_VMA) return 0;
    return vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), addr) != 0;
}

static int vfs_node_ptr_sane(vfs_node_t *node) {
    uint64_t a;

    if (!node) return 0;
    a = (uint64_t)node;
    if ((a & 0xFFFFFF00) == 0xFEFEFE00) return 0;
    if (a < KERNEL_VMA || a >= 0xFFFFFFFFFFFFFF00ULL) return 0;
    if (!kernel_ptr_mapped(a)) return 0;
    if (!kernel_ptr_mapped(a + (uint64_t)sizeof(vfs_node_t) - 1)) return 0;
    return 1;
}

static int user_range_mapped(uint64_t addr, uint64_t size) {
    uint64_t end;
    uint64_t p;
    uint64_t pend;

    if (!current_task) return 0;
    if (size == 0) return 1;
    end = addr + size - 1;
    if (end < addr) return 0;
    if (addr < 0x1000 || end >= KERNEL_VMA) return 0;

    p = addr & ~0xFFFu;
    pend = end & ~0xFFFu;
    for (;;) {
        if (vmm_get_phys_in_pml4(current_task->cr3, p) == 0) return 0;
        if (p == pend) break;
        if (p > 0xFFFFFFFFFFFFF000ULL) return 0;
        p += 0x1000u;
    }
    return 1;
}

static int vfs_name_is(const char name[VFS_MAX_NAME], const char *lit) {
    size_t n;

    n = strlen(lit);
    if (n >= VFS_MAX_NAME) return 0;
    if (memcmp(name, lit, n) != 0) return 0;
    return name[n] == '\0';
}

static int vfs_name_is_tty(const char name[VFS_MAX_NAME]) {
    int i;

    if (vfs_name_is(name, "tty")) return 1;
    if (name[0] != 't' || name[1] != 't' || name[2] != 'y') return 0;
    i = 3;
    if (name[i] < '0' || name[i] > '9') return 0;
    while (name[i] >= '0' && name[i] <= '9') i++;
    return name[i] == '\0';
}

struct kernel_termios *tty_termios;
struct kernel_winsize *tty_winsize;
int *tty_pgrp;
int tty_count;

#define TTY_OUTPUT_STOPPED 0x80000000U
#define TCXONC 0x540A

static int sys_tcflow(int fd, const char *action_ptr, int unused);

int tty_get_foreground_pgrp(int tty_id) {
    if (!tty_pgrp || tty_id < 0 || tty_id >= tty_count) return 0;
    return (int)((uint32_t)tty_pgrp[tty_id] & ~TTY_OUTPUT_STOPPED);
}

int tty_output_is_stopped(int tty_id) {
    if (!tty_pgrp || tty_id < 0 || tty_id >= tty_count) return 0;
    return ((uint32_t)tty_pgrp[tty_id] & TTY_OUTPUT_STOPPED) != 0;
}

static void tty_set_foreground_pgrp(int tty_id, int pgrp) {
    uint32_t state;

    state = (uint32_t)tty_pgrp[tty_id] & TTY_OUTPUT_STOPPED;
    tty_pgrp[tty_id] = (int)(state | ((uint32_t)pgrp & ~TTY_OUTPUT_STOPPED));
}

static struct vt_mode_s vt_mode = { VT_AUTO, 0, 0, 0, 0 };

static int ioctl_fcntl_dupfd_compat(int oldfd, int cmd, int minfd) {
    pipe_t *p;
    int newfd;
    int ret;
    int i;

    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !current_task->fds[oldfd].in_use) return -EBADF;
    if (minfd < 0) minfd = 0;

    newfd = -1;
    for (i = minfd; i < current_task->fds_capacity; i++) {
        if (!current_task->fds[i].in_use) {
            newfd = i;
            memset(&current_task->fds[i], 0, sizeof(task_fd_t));
            current_task->fds[i].in_use = 1;
            current_task->fds[i].ref_count = 1;
            break;
        }
    }
    if (newfd < 0) {
        ret = task_fd_ensure_capacity(current_task, minfd >= current_task->fds_capacity ? minfd : current_task->fds_capacity);
        if (ret != 0) return -EMFILE;
        for (i = minfd; i < current_task->fds_capacity; i++) {
            if (!current_task->fds[i].in_use) {
                newfd = i;
                memset(&current_task->fds[i], 0, sizeof(task_fd_t));
                current_task->fds[i].in_use = 1;
                current_task->fds[i].ref_count = 1;
                break;
            }
        }
    }
    if (newfd < 0) return -EMFILE;

    memcpy(&current_task->fds[newfd], &current_task->fds[oldfd], sizeof(task_fd_t));
    current_task->fds[newfd].ref_count = 1;

    if (current_task->fds[newfd].type == FD_TYPE_FILE && current_task->fds[newfd].node) {
        vfs_open((vfs_node_t *)current_task->fds[newfd].node, 0);
        task_fd_position_share(&current_task->fds[oldfd], &current_task->fds[newfd]);
    }
    if (current_task->fds[oldfd].private_data &&
        FD_TYPE_IS_PIPE(current_task->fds[oldfd].type)) {
        p = (pipe_t *)current_task->fds[oldfd].private_data;
        pipe_retain_reference(p, current_task->fds[oldfd].type);
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

static int tty_valid_id(int tty_id) {
    if (!tty_termios || !tty_winsize || !tty_pgrp) return 0;
    if (tty_id < 0 || tty_id >= tty_count) return 0;
    return 1;
}

static void termios_init_defaults(int tty_id) {
    struct kernel_termios *t;
    framebuffer_t *fb;

    t = &tty_termios[tty_id];
    
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
    
    fb = fb_get();
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
    task_fd_t *tfd;
    vfs_node_t *node;

    if (fd < 0) return -1;
    if (!current_task) return (fd >= 0 && fd <= 2) ? console_get_current() : -1;
    if (fd >= current_task->fds_capacity) return -1;
    if (!current_task->fds[fd].in_use) {
        if (fd >= 0 && fd <= 2) return (current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        return -1;
    }
    tfd = &current_task->fds[fd];
    if (tfd->type == FD_TYPE_STDIN || tfd->type == FD_TYPE_STDOUT || tfd->type == FD_TYPE_STDERR) {
        if (current_task->console_id >= 0) return current_task->console_id;
        return console_get_current();
    }
    if (tfd->type == FD_TYPE_FILE && tfd->node) {
        node = (vfs_node_t *)tfd->node;
        if (!vfs_node_ptr_sane(node)) {
            return -1;
        }
        if (VFS_GET_TYPE(node->flags) == VFS_CHARDEVICE &&
            (vfs_name_is_tty(vfs_node_name(node)) || vfs_name_is(vfs_node_name(node), "console"))) {
            if (current_task->console_id >= 0) return current_task->console_id;
            return console_get_current();
        }
    }
    return -1;
}

static int sys_tcgetattr(int fd, const char *termios_ptr, int unused) {
    int tty_id;
    uint64_t addr;

    (void)unused;

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;

    addr = (uint64_t)termios_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    memcpy((void*)addr, &tty_termios[tty_id], sizeof(struct kernel_termios));
    return 0;
}

static int sys_tcsetattr(int fd, const char *actions_ptr,
                         uint64_t termios_ptr) {
    int actions;
    int tty_id;
    uint64_t addr;

    actions = (int)(uintptr_t)actions_ptr;

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;

    addr = (uint64_t)termios_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;

    if (actions == TCSAFLUSH) {
        keyboard_flush_for(tty_id);
        syscall_core_flush_tty_input(tty_id);
    }
    memcpy(&tty_termios[tty_id], (void*)addr, sizeof(struct kernel_termios));
    return 0;
}

static int sys_ioctl(int fd, const char *request_ptr, uint64_t arg) {
    unsigned long request;
    int tty_id;
    int graphics_result;
    int pty_fd;
    task_fd_t *tfd;
    uint64_t node_addr;
    vfs_node_t *vn;
    framebuffer_t *fb;
    struct vt_stat_s *vst;
    struct vt_stat2_s vst2;
    uint64_t state_capacity;
    uint64_t state_words;
    uint64_t state_word;
    uint64_t state_index;
    uint64_t state_bits;
    int pgrp;
    int active;
    int vi;
    int found;
    int ci;
    int target_vt;

    request = (unsigned long)(uintptr_t)request_ptr;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) {
        return -EBADF;
    }

    if (current_task->fds[fd].in_use) {
        tfd = &current_task->fds[fd];
        node_addr = (uint64_t)tfd->node;
        if (node_addr && ((node_addr & 0xFFFFFF00) == 0xFEFEFE00)) {
            tfd->node = NULL;
        }
    } else if (current_task) {
        return -EBADF;
    }

    if (request == 0ul || request == 0x406ul) {
        return ioctl_fcntl_dupfd_compat(fd, (int)request, (int)arg);
    }
    if (request == 1ul || request == 2ul || request == 3ul || request == 4ul) {
        return ioctl_fcntl_basic_compat(fd, (int)request, (int)arg);
    }

    if (current_task->fds[fd].in_use && current_task->fds[fd].node) {
        vn = (vfs_node_t *)current_task->fds[fd].node;
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
    if (!tty_valid_id(tty_id)) tty_id = -1;
    
    switch (request) {
        case TIOCSCTTY:
            if (tty_id < 0) return -ENOTTY;
            if (current_task && tty_get_foreground_pgrp(tty_id) == 0) {
                tty_set_foreground_pgrp(tty_id, current_task->pid);
            }
            return 0;

        case TIOCNOTTY:
            if (tty_id < 0) return -ENOTTY;
            if (current_task) {
                if (creds_get_sid(0) == current_task->pid)
                    tty_set_foreground_pgrp(tty_id, 0);
                current_task->console_id = -1;
            }
            (void)arg;
            return 0;

        case TCXONC:
            return sys_tcflow(fd, (const char *)(uintptr_t)arg, 0);

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
            if (request == TIOCSETAF) {
                keyboard_flush_for(tty_id);
                syscall_core_flush_tty_input(tty_id);
            }
            memcpy(&tty_termios[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_termios));
            return 0;
            
        case TIOCGWINSZ:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct kernel_winsize))) return -EFAULT;
            {
                fb = fb_get();
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
                pgrp = tty_get_foreground_pgrp(tty_id);
                if (pgrp > 0)
                    deliver_signal_to_pgrp((pid_t)pgrp, 28);
            }
            return 0;
            
        case TIOCGPGRP:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            {
                pgrp = tty_get_foreground_pgrp(tty_id);
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
            tty_set_foreground_pgrp(tty_id, *(int *)(uintptr_t)arg);
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

        case VT_OPENQRY:
        {
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            active = console_get_current();
            found = -1;
            for (vi = 0; vi < tty_count; vi++) {
                if (vi != active) {
                    found = vi + 1;
                    break;
                }
            }
            *(int *)(uintptr_t)arg = found;
            return 0;
        }

        case VT_GETSTATE:
        {
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct vt_stat_s))) return -EFAULT;
            vst = (struct vt_stat_s *)(uintptr_t)arg;
            memset(vst, 0, sizeof(*vst));
            vst->v_active = (uint16_t)(console_get_current() + 1);
            vst->v_state = 0;
            for (ci = 0; ci < tty_count && ci < 16; ci++) {
                vst->v_state |= (uint16_t)(1 << (ci + 1));
            }
            return 0;
        }

        case VT_GETSTATE2:
        {
            if (!arg || copy_from_user(&vst2, (void *)(uintptr_t)arg,
                                       sizeof(vst2)) < 0)
                return -EFAULT;
            state_capacity = vst2.v_state_words;
            state_words = ((uint64_t)tty_count + 63) / 64;
            vst2.v_active = (uint64_t)console_get_current() + 1;
            vst2.v_count = (uint64_t)tty_count;
            vst2.v_state_words = state_words;
            if (copy_to_user((void *)(uintptr_t)arg, &vst2,
                             sizeof(vst2)) < 0)
                return -EFAULT;
            if (!vst2.v_state_ptr) return 0;
            if (state_capacity < state_words) return -ENOSPC;
            for (state_index = 0; state_index < state_words;
                 state_index++) {
                state_bits = (uint64_t)tty_count - state_index * 64;
                if (state_bits >= 64)
                    state_word = UINT64_MAX;
                else
                    state_word = (1ULL << state_bits) - 1;
                if (copy_to_user((void *)(uintptr_t)(vst2.v_state_ptr +
                                 state_index * sizeof(state_word)),
                                 &state_word, sizeof(state_word)) < 0)
                    return -EFAULT;
            }
            return 0;
        }

        case VT_ACTIVATE:
        {
            target_vt = arg;
            if (target_vt < 1 || target_vt > tty_count) return -ENXIO;
            console_switch(target_vt - 1);
            return 0;
        }

        case VT_WAITACTIVE:
        {
            target_vt = arg;
            if (target_vt < 1 || target_vt > tty_count) return -ENXIO;
            while (console_get_current() != (target_vt - 1)) {
                schedule();
            }
            return 0;
        }

        case VT_GETMODE:
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct vt_mode_s))) return -EFAULT;
            memcpy((void *)(uintptr_t)arg, &vt_mode, sizeof(struct vt_mode_s));
            return 0;

        case VT_SETMODE:
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(struct vt_mode_s))) return -EFAULT;
            memcpy(&vt_mode, (void *)(uintptr_t)arg, sizeof(struct vt_mode_s));
            return 0;

        case VT_RELDISP:
            return 0;

        case VT_DISALLOCATE:
            return 0;

        case KDSETMODE:
            if (arg != KD_TEXT && arg != KD_GRAPHICS) return -EINVAL;
            if (!tty_valid_id(tty_id)) return -ENOTTY;
            graphics_result = console_set_graphics_mode(
                tty_id, arg == KD_GRAPHICS,
                current_task ? current_task->pid : 0);
            if (graphics_result == -2) return -EBUSY;
            if (graphics_result != 0) return -ENODEV;
            return 0;

        case KDGETMODE:
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            if (!tty_valid_id(tty_id)) return -ENOTTY;
            *(int *)(uintptr_t)arg = console_get_graphics_mode(tty_id) ?
                                     KD_GRAPHICS : KD_TEXT;
            return 0;

        case KDMKTONE:
            return 0;

        case KDGKBTYPE:
            if (!arg || !user_range_mapped((uint64_t)arg, sizeof(int))) return -EFAULT;
            *(int *)(uintptr_t)arg = KB_101;
            return 0;

        default:
            return -EINVAL;
    }
}

static int sys_tcflush(int fd, const char *queue_ptr, int unused) {
    int queue;
    int tty_id;

    (void)unused;
    queue = (int)(uintptr_t)queue_ptr;

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;
    
    if (queue == TCIFLUSH || queue == TCIOFLUSH) {
        keyboard_flush_for(tty_id);
        syscall_core_flush_tty_input(tty_id);
        return 0;
    }
    if (queue == TCOFLUSH)
        return 0;
    return -EINVAL;
}

static int sys_tcflow(int fd, const char *action_ptr, int unused) {
    int action;
    int tty_id;

    (void)unused;
    action = (int)(uintptr_t)action_ptr;

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;
    
    if (action == TCOOFF) {
        tty_pgrp[tty_id] = (int)((uint32_t)tty_pgrp[tty_id] |
                                 TTY_OUTPUT_STOPPED);
        return 0;
    }
    if (action == TCOON) {
        tty_pgrp[tty_id] = (int)((uint32_t)tty_pgrp[tty_id] &
                                 ~TTY_OUTPUT_STOPPED);
        descriptor_ready_notify();
        return 0;
    }
    if (action == TCIOFF || action == TCION) return 0;
    return -EINVAL;
}

static int sys_tcdrain(int fd, const char *unused1, int unused2) {
    int tty_id;

    (void)unused1; (void)unused2;
    
    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;
    
    return 0;
}

static int sys_tcgetpgrp(int fd, const char *unused1, int unused2) {
    int tty_id;
    int pgrp;

    (void)unused1; (void)unused2;
    
    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;

    pgrp = tty_get_foreground_pgrp(tty_id);
    if (pgrp == 0 && current_task) {
        pgrp = current_task->pgid ? current_task->pgid : current_task->pid;
        if (pgrp == 0) pgrp = 1;
    }

    return pgrp;
}

static int sys_tcsetpgrp(int fd, const char *pgrp_ptr, int unused) {
    int pgrp;
    int tty_id;

    (void)unused;
    pgrp = (int)(uintptr_t)pgrp_ptr;

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;
    
    tty_set_foreground_pgrp(tty_id, pgrp);
    return 0;
}

void syscalls_termios_init(void) {
    int i;
    int count;

    syscall_table_set(SYSCALL_TCGETATTR, (void *)(sys_tcgetattr));
    syscall_table_set(SYSCALL_TCSETATTR, (void *)(sys_tcsetattr));
    syscall_table_set(SYSCALL_IOCTL, (void *)(sys_ioctl));
    syscall_table_set(SYSCALL_TCFLUSH, (void *)(sys_tcflush));
    syscall_table_set(SYSCALL_TCFLOW, (void *)(sys_tcflow));
    syscall_table_set(SYSCALL_TCDRAIN, (void *)(sys_tcdrain));
    syscall_table_set(SYSCALL_TCGETPGRP, (void *)(sys_tcgetpgrp));
    syscall_table_set(SYSCALL_TCSETPGRP, (void *)(sys_tcsetpgrp));
    
    count = console_get_count();
    if (count <= 0) count = 1;

    tty_termios = (struct kernel_termios *)kmalloc(count * sizeof(struct kernel_termios));
    tty_winsize = (struct kernel_winsize *)kmalloc(count * sizeof(struct kernel_winsize));
    tty_pgrp = (int *)kmalloc(count * sizeof(int));

    if (!tty_termios || !tty_winsize || !tty_pgrp) {
        if (tty_termios) kfree(tty_termios);
        if (tty_winsize) kfree(tty_winsize);
        if (tty_pgrp) kfree(tty_pgrp);
        tty_termios = NULL;
        tty_winsize = NULL;
        tty_pgrp = NULL;
        count = 0;
    }

    tty_count = count;
    memset(tty_termios, 0, tty_count * sizeof(struct kernel_termios));
    memset(tty_winsize, 0, tty_count * sizeof(struct kernel_winsize));
    memset(tty_pgrp, 0, tty_count * sizeof(int));

    for (i = 0; i < tty_count; i++) {
        termios_init_defaults(i);
    }
}

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

static struct vt_mode_s *vt_modes;
static pid_t *vt_owners;
static uint8_t *kbd_modes;
static int vt_pending_switch = -1;

extern int is_socket_fd(int fd);
extern int socket_ioctl(int fd, unsigned long request, uint64_t arg);
extern void socket_retain_task_fd(task_fd_t *descriptor);

static int vt_release_limit(void) {
    int64_t limit;

    if (tty_count <= 0) return -1;
    limit = (int64_t)tty_count * tty_count;
    if (limit > INT32_MAX) return -1;
    return tty_count * tty_count;
}

static int vt_release_encode(int source_vt, int target_vt) {
    if (tty_count <= 0 || source_vt < 0 || target_vt < 0 ||
        source_vt >= tty_count || target_vt >= tty_count)
        return -1;
    if (vt_release_limit() < 0) return -1;
    return source_vt * tty_count + target_vt;
}

static int vt_acquire_encode(int source_vt, int target_vt) {
    int64_t encoded;
    int limit;
    int slot;

    limit = vt_release_limit();
    if (limit < 0 || source_vt < 0 || source_vt >= tty_count ||
        target_vt < -1 || target_vt >= tty_count)
        return -1;
    slot = target_vt + 1;
    encoded = (int64_t)limit +
        (int64_t)source_vt * (tty_count + 1) + slot;
    if (encoded > INT32_MAX) return -1;
    return (int)encoded;
}

static int vt_pending_phase(int pending) {
    int limit;
    int source;
    int slot;

    limit = vt_release_limit();
    if (pending < 0 || limit < 0) return 0;
    if (pending < limit) return 1;
    source = (pending - limit) / (tty_count + 1);
    slot = (pending - limit) % (tty_count + 1);
    if (source >= 0 && source < tty_count &&
        slot >= 0 && slot <= tty_count)
        return 2;
    return 0;
}

static int vt_pending_source(int pending) {
    int limit;
    int phase;

    limit = vt_release_limit();
    phase = vt_pending_phase(pending);
    if (phase == 1) return pending / tty_count;
    if (phase == 2) return (pending - limit) / (tty_count + 1);
    return -1;
}

static int vt_pending_target(int pending) {
    int limit;
    int phase;

    limit = vt_release_limit();
    phase = vt_pending_phase(pending);
    if (phase == 1) return pending % tty_count;
    if (phase == 2)
        return ((pending - limit) % (tty_count + 1)) - 1;
    return -1;
}

static int vt_pending_take(int phase, int source_vt) {
    int pending;
    int expected;

    for (;;) {
        pending = __atomic_load_n(&vt_pending_switch, __ATOMIC_ACQUIRE);
        if (vt_pending_phase(pending) != phase ||
            vt_pending_source(pending) != source_vt)
            return -1;
        expected = pending;
        if (__atomic_compare_exchange_n(&vt_pending_switch, &expected, -1,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return pending;
    }
}

int tty_vt_switch_request(int target_vt) {
    int active;
    int encoded;
    int expected;
    int pending;
    int phase;
    int source;
    int result;
    int updated;
    struct vt_mode_s *mode;

    if (!vt_modes || !vt_owners)
        return 1;
    active = console_get_current();
    if (active < 0 || active >= tty_count) return 1;
    mode = &vt_modes[active];
    if (mode->mode != VT_PROCESS || vt_owners[active] <= 0) return 1;
    encoded = vt_release_encode(active, target_vt);
    if (encoded < 0) return 1;
    for (;;) {
        pending = __atomic_load_n(&vt_pending_switch, __ATOMIC_ACQUIRE);
        if (pending >= 0) {
            phase = vt_pending_phase(pending);
            source = vt_pending_source(pending);
            if (source == active && phase == 1) {
                if (pending == encoded) return 0;
                expected = pending;
                if (__atomic_compare_exchange_n(&vt_pending_switch,
                                                &expected, encoded, 0,
                                                __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE))
                    return 0;
                continue;
            }
            if (source == active && phase == 2) {
                updated = vt_acquire_encode(active, target_vt);
                if (updated < 0) return 0;
                expected = pending;
                if (__atomic_compare_exchange_n(&vt_pending_switch,
                                                &expected, updated, 0,
                                                __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE))
                    return 0;
                continue;
            }
            if (phase != 0 && source >= 0 && source < tty_count &&
                vt_modes[source].mode == VT_PROCESS &&
                vt_owners[source] > 0)
                return 0;
            expected = pending;
            __atomic_compare_exchange_n(&vt_pending_switch, &expected, -1,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE);
            continue;
        }
        expected = -1;
        if (__atomic_compare_exchange_n(&vt_pending_switch, &expected,
                                        encoded, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    result = sys_kill_impl(vt_owners[active],
                           (const char *)(uintptr_t)mode->relsig, 0);
    if (result < 0 && result != -EINTR) {
        expected = encoded;
        __atomic_compare_exchange_n(&vt_pending_switch, &expected, -1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        memset(mode, 0, sizeof(*mode));
        vt_owners[active] = 0;
        return 1;
    }
    return 0;
}

void tty_vt_switch_complete(int target_vt) {
    int encoded;
    int expected;
    int result;
    struct vt_mode_s *mode;

    if (!vt_modes || !vt_owners) return;
    if (target_vt < 0 || target_vt >= tty_count) return;
    mode = &vt_modes[target_vt];
    if (mode->mode != VT_PROCESS || vt_owners[target_vt] <= 0 ||
        mode->acqsig <= 0)
        return;
    encoded = vt_acquire_encode(target_vt, -1);
    if (encoded < 0) return;
    expected = -1;
    if (!__atomic_compare_exchange_n(&vt_pending_switch, &expected, encoded,
                                     0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    result = sys_kill_impl(vt_owners[target_vt],
                           (const char *)(uintptr_t)mode->acqsig, 0);
    if (result < 0 && result != -EINTR) {
        expected = encoded;
        __atomic_compare_exchange_n(&vt_pending_switch, &expected, -1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        memset(mode, 0, sizeof(*mode));
        vt_owners[target_vt] = 0;
    }
}

void tty_vt_release_owner(pid_t pid) {
    int i;
    int pending;
    int phase;
    int target_vt;

    if (!vt_modes || !vt_owners) return;
    target_vt = -1;
    for (i = 0; i < tty_count; i++) {
        if (vt_owners[i] != pid) continue;
        memset(&vt_modes[i], 0, sizeof(vt_modes[i]));
        vt_owners[i] = 0;
        if (kbd_modes) kbd_modes[i] = K_XLATE;
        if (i == console_get_current() &&
            console_get_graphics_mode(i))
            console_set_graphics_mode(i, 0, pid);
        pending = __atomic_load_n(&vt_pending_switch, __ATOMIC_ACQUIRE);
        phase = vt_pending_phase(pending);
        if (phase != 0 && vt_pending_source(pending) == i)
            pending = vt_pending_take(phase, i);
        else
            pending = -1;
        if (pending >= 0 && phase == 1 && i == console_get_current())
            target_vt = vt_pending_target(pending);
    }
    if (target_vt >= 0) {
        if (console_switch_committed(target_vt) == 0)
            tty_vt_switch_complete(target_vt);
    }
}

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
    if (FD_TYPE_IS_PTY(current_task->fds[newfd].type))
        pty_retain_endpoint((int)(uintptr_t)current_task->fds[newfd].private_data);
    if (current_task->fds[newfd].type == FD_TYPE_SOCKET)
        socket_retain_task_fd(&current_task->fds[newfd]);
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
            if (vfs_node_name(node)[0] == 't' &&
                vfs_node_name(node)[1] == 't' &&
                vfs_node_name(node)[2] == 'y' &&
                vfs_node_name(node)[3] >= '0' &&
                vfs_node_name(node)[3] <= '9' &&
                node->inode < (uint64_t)tty_count)
                return (int)node->inode;
            if (current_task->console_id >= 0) return current_task->console_id;
            return console_get_current();
        }
    }
    return -1;
}

static int sys_tcgetattr(int fd, const char *termios_ptr, int unused) {
    int tty_id;
    int pty_fd;
    int result;
    uint64_t addr;
    struct kernel_termios value;

    (void)unused;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        addr = (uint64_t)termios_ptr;
        if (!addr || !user_access_ok((void *)(uintptr_t)addr,
                                     sizeof(value), UACCESS_WRITE))
            return -EFAULT;
        result = pty_ioctl(pty_fd, TCGETS, &value);
        if (result < 0) return -ENOTTY;
        if (copy_to_user((void *)(uintptr_t)addr, &value,
                         sizeof(value)) < 0)
            return -EFAULT;
        return 0;
    }

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
    int pty_fd;
    int result;
    uint64_t addr;
    struct kernel_termios value;

    actions = (int)(uintptr_t)actions_ptr;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        if (actions < TCSANOW || actions > TCSAFLUSH) return -EINVAL;
        addr = termios_ptr;
        if (!addr || !user_access_ok((void *)(uintptr_t)addr,
                                     sizeof(value), UACCESS_READ))
            return -EFAULT;
        if (copy_from_user(&value, (void *)(uintptr_t)addr,
                           sizeof(value)) < 0)
            return -EFAULT;
        result = pty_ioctl(pty_fd, TCSETS + actions, &value);
        return result < 0 ? -ENOTTY : 0;
    }

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
    int pending;
    int expected;
    int switch_result;
    int target_vt;
    struct vt_mode_s requested_mode;

    request = (uint32_t)(uintptr_t)request_ptr;

    if (!current_task) return -ESRCH;
    if (is_socket_fd(fd)) return socket_ioctl(fd, request, arg);
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

    if (request == FIONBIO) {
        if (!arg || !syscall_user_range_present(arg, sizeof(int), 1, 0))
            return -EFAULT;
        tfd = &current_task->fds[fd];
        if (*(int *)(uintptr_t)arg)
            tfd->flags |= VFS_O_NONBLOCK;
        else
            tfd->flags &= ~VFS_O_NONBLOCK;
        return 0;
    }

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) tty_id = -1;

    if (FD_TYPE_IS_PTY(current_task->fds[fd].type)) {
        pty_fd = (int)(uintptr_t)current_task->fds[fd].private_data;
        return pty_ioctl(pty_fd, request, (void *)(uintptr_t)arg);
    }

    if (current_task->fds[fd].in_use && current_task->fds[fd].node) {
        vn = (vfs_node_t *)current_task->fds[fd].node;
        if (vn->ioctl) {
            return vn->ioctl(vn, request, (void *)(uintptr_t)arg);
        }
    }

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
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
            *(int *)(uintptr_t)arg = (int)creds_get_sid(0);
            return 0;

        case TIOCGETA:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct kernel_termios), 1, 0)) return -EFAULT;
            memcpy((void*)(uintptr_t)arg, &tty_termios[tty_id], sizeof(struct kernel_termios));
            return 0;
            
        case TIOCSETA:
        case TIOCSETAW:
        case TIOCSETAF:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct kernel_termios), 1, 0)) return -EFAULT;
            if (request == TIOCSETAF) {
                keyboard_flush_for(tty_id);
                syscall_core_flush_tty_input(tty_id);
            }
            memcpy(&tty_termios[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_termios));
            return 0;
            
        case TIOCGWINSZ:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct kernel_winsize), 1, 0)) return -EFAULT;
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
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct kernel_winsize), 1, 0)) return -EFAULT;
            memcpy(&tty_winsize[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_winsize));
            {
                pgrp = tty_get_foreground_pgrp(tty_id);
                if (pgrp > 0)
                    deliver_signal_to_pgrp((pid_t)pgrp, 28);
            }
            return 0;
            
        case TIOCGPGRP:
            if (tty_id < 0) return -ENOTTY;
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
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
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
            tty_set_foreground_pgrp(tty_id, *(int *)(uintptr_t)arg);
            return 0;
            
        case FIONREAD:
            if (fd == 0 && tty_id >= 0) {
                if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
                *(int*)(uintptr_t)arg = keyboard_has_data_for(tty_id) ? 1 : 0;
                return 0;
            }
            return -ENOTTY;
            
        case FIONBIO:
            return 0;

        case VT_OPENQRY:
        {
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
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
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct vt_stat_s), 1, 0)) return -EFAULT;
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
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct vt_mode_s), 1, 0)) return -EFAULT;
            if (!tty_valid_id(tty_id) || !vt_modes) return -ENOTTY;
            memcpy((void *)(uintptr_t)arg, &vt_modes[tty_id],
                   sizeof(struct vt_mode_s));
            return 0;

        case VT_SETMODE:
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(struct vt_mode_s), 1, 0)) return -EFAULT;
            if (!tty_valid_id(tty_id) || !vt_modes || !vt_owners)
                return -ENOTTY;
            memcpy(&requested_mode, (void *)(uintptr_t)arg,
                   sizeof(requested_mode));
            if (requested_mode.mode != VT_AUTO &&
                requested_mode.mode != VT_PROCESS)
                return -EINVAL;
            if (requested_mode.relsig < 0 || requested_mode.relsig > 64 ||
                requested_mode.acqsig < 0 || requested_mode.acqsig > 64 ||
                (requested_mode.mode == VT_PROCESS &&
                 requested_mode.relsig == 0))
                return -EINVAL;
            if (requested_mode.mode == VT_AUTO ||
                vt_owners[tty_id] != current_task->pid) {
                pending = __atomic_load_n(&vt_pending_switch,
                                          __ATOMIC_ACQUIRE);
                if (vt_pending_source(pending) == tty_id)
                    vt_pending_take(vt_pending_phase(pending), tty_id);
            }
            vt_modes[tty_id] = requested_mode;
            if (requested_mode.mode == VT_PROCESS)
                vt_owners[tty_id] = current_task->pid;
            else
                vt_owners[tty_id] = 0;
            return 0;

        case VT_RELDISP:
            if (!tty_valid_id(tty_id) || !vt_modes || !vt_owners)
                return -ENOTTY;
            if (vt_owners[tty_id] != current_task->pid) return -EPERM;
            if (arg == VT_ACKACQ) {
                if (console_get_current() != tty_id) return -EINVAL;
                pending = vt_pending_take(2, tty_id);
                if (pending < 0) return 0;
                target_vt = vt_pending_target(pending);
                if (target_vt >= 0)
                    tty_vt_switch_request(target_vt);
                return 0;
            }
            if (arg == 0) {
                vt_pending_take(1, tty_id);
                return 0;
            }
            if (arg != 1) return -EINVAL;
            pending = vt_pending_take(1, tty_id);
            if (pending < 0) return 0;
            target_vt = vt_pending_target(pending);
            if (!tty_valid_id(target_vt)) return -EINVAL;
            switch_result = console_switch_committed(target_vt);
            if (switch_result != 0) {
                expected = -1;
                __atomic_compare_exchange_n(&vt_pending_switch, &expected,
                                            pending, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE);
                return switch_result > 0 ? -EAGAIN : -EIO;
            }
            tty_vt_switch_complete(target_vt);
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
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
            if (!tty_valid_id(tty_id)) return -ENOTTY;
            *(int *)(uintptr_t)arg = console_get_graphics_mode(tty_id) ?
                                     KD_GRAPHICS : KD_TEXT;
            return 0;

        case KDMKTONE:
            return 0;

        case KDGKBTYPE:
            if (!arg || !syscall_user_range_present((uint64_t)arg, sizeof(int), 1, 0)) return -EFAULT;
            *(int *)(uintptr_t)arg = KB_101;
            return 0;

        case KDGKBMODE:
            if (!arg || !syscall_user_range_present((uint64_t)arg,
                    sizeof(int), 1, 0)) return -EFAULT;
            if (!tty_valid_id(tty_id) || !kbd_modes) return -ENOTTY;
            *(int *)(uintptr_t)arg = kbd_modes[tty_id];
            return 0;

        case KDSKBMODE:
            if (!tty_valid_id(tty_id) || !kbd_modes) return -ENOTTY;
            if (arg != K_RAW && arg != K_XLATE && arg != K_MEDIUMRAW &&
                arg != K_UNICODE && arg != K_OFF)
                return -EINVAL;
            kbd_modes[tty_id] = (uint8_t)arg;
            return 0;

        case KDGETLED:
        case KDGKBLED:
            if (!arg || !syscall_user_range_present((uint64_t)arg,
                    sizeof(char), 1, 0)) return -EFAULT;
            *(char *)(uintptr_t)arg = 0;
            return 0;

        case KDSETLED:
        case KDSKBLED:
            return arg <= 7 ? 0 : -EINVAL;

        default:
            return -EINVAL;
    }
}

static int sys_tcflush(int fd, const char *queue_ptr, int unused) {
    int queue;
    int tty_id;
    int pty_fd;
    int result;

    (void)unused;
    queue = (int)(uintptr_t)queue_ptr;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        result = pty_ioctl(pty_fd, TCFLSH,
                           (void *)(uintptr_t)queue);
        return result < 0 ? result : 0;
    }

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
    int pty_fd;
    int result;

    (void)unused;
    action = (int)(uintptr_t)action_ptr;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        result = pty_ioctl(pty_fd, TCXONC,
                           (void *)(uintptr_t)action);
        return result < 0 ? result : 0;
    }

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
    int pty_fd;
    int result;

    (void)unused1; (void)unused2;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        result = pty_ioctl(pty_fd, TCSBRK, (void *)(uintptr_t)1);
        return result < 0 ? result : 0;
    }
    
    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;
    
    return 0;
}

static int sys_tcgetpgrp(int fd, const char *unused1, int unused2) {
    int tty_id;
    int pgrp;
    int pty_fd;
    int result;

    (void)unused1; (void)unused2;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        pgrp = 0;
        result = pty_ioctl(pty_fd, TIOCGPGRP, &pgrp);
        return result < 0 ? -ENOTTY : pgrp;
    }
    
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
    int pty_fd;
    int result;

    (void)unused;
    pgrp = (int)(uintptr_t)pgrp_ptr;

    pty_fd = pty_task_endpoint(fd);
    if (pty_fd >= 0) {
        result = pty_ioctl(pty_fd, TIOCSPGRP, &pgrp);
        return result < 0 ? -ENOTTY : 0;
    }

    tty_id = get_tty_id_for_fd(fd);
    if (!tty_valid_id(tty_id)) return -ENOTTY;
    
    tty_set_foreground_pgrp(tty_id, pgrp);
    return 0;
}

void syscalls_termios_init(void) {
    int i;
    int count;
    size_t state_size;
    uint8_t *state_cursor;
    void *state_storage;

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

    state_size = (size_t)count *
        (sizeof(struct kernel_termios) + sizeof(struct kernel_winsize) +
         sizeof(int) + sizeof(struct vt_mode_s) + sizeof(pid_t) +
         sizeof(uint8_t));
    state_storage = kmalloc(state_size);
    if (!state_storage) {
        tty_termios = NULL;
        tty_winsize = NULL;
        tty_pgrp = NULL;
        vt_modes = NULL;
        vt_owners = NULL;
        kbd_modes = NULL;
        count = 0;
    } else {
        state_cursor = (uint8_t *)state_storage;
        tty_termios = (struct kernel_termios *)state_cursor;
        state_cursor += (size_t)count * sizeof(struct kernel_termios);
        tty_winsize = (struct kernel_winsize *)state_cursor;
        state_cursor += (size_t)count * sizeof(struct kernel_winsize);
        tty_pgrp = (int *)state_cursor;
        state_cursor += (size_t)count * sizeof(int);
        vt_modes = (struct vt_mode_s *)state_cursor;
        state_cursor += (size_t)count * sizeof(struct vt_mode_s);
        vt_owners = (pid_t *)state_cursor;
        state_cursor += (size_t)count * sizeof(pid_t);
        kbd_modes = state_cursor;
    }

    tty_count = count;
    memset(tty_termios, 0, tty_count * sizeof(struct kernel_termios));
    memset(tty_winsize, 0, tty_count * sizeof(struct kernel_winsize));
    memset(tty_pgrp, 0, tty_count * sizeof(int));
    memset(vt_modes, 0, tty_count * sizeof(struct vt_mode_s));
    memset(vt_owners, 0, tty_count * sizeof(pid_t));
    memset(kbd_modes, K_XLATE, tty_count * sizeof(uint8_t));

    for (i = 0; i < tty_count; i++) {
        termios_init_defaults(i);
    }
}

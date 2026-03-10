#include "syscall_defs.h"
#include <kernel/pit.h>
#include <kernel/debug.h>
#include <kernel/pty.h>
#include <kernel/common.h>

#define fd_table (current_task->fds)

static int kernel_ptr_mapped(uint32_t addr) {
    if (addr < 0xC0000000) return 0;
    return vmm_get_phys_in_pd(vmm_get_kernel_cr3(), addr) != 0;
}

static int vfs_node_ptr_sane(vfs_node_t *node) {
    if (!node) return 0;
    uint32_t a = (uint32_t)node;
    if ((a & 0xFFFFFF00) == 0xFEFEFE00) return 0;
    if (a < 0xC0000000 || a >= 0xFFFFFF00) return 0;
    if (!kernel_ptr_mapped(a)) return 0;
    if (!kernel_ptr_mapped(a + (uint32_t)sizeof(vfs_node_t) - 1)) return 0;
    return 1;
}

static void fd_release_entry(task_fd_t *tfd) {
    if (!tfd || !tfd->in_use) return;

    vfs_node_t *node_to_close = NULL;
    pipe_t *pipe_to_release = NULL;
    int pipe_type = 0;

    if (tfd->type == FD_TYPE_PIPE_R || tfd->type == FD_TYPE_PIPE_W) {
        pipe_to_release = (pipe_t *)tfd->private_data;
        pipe_type = tfd->type;
    } else if (tfd->type == FD_TYPE_FILE && tfd->node) {
        node_to_close = (vfs_node_t *)tfd->node;
    }

    memset(tfd, 0, sizeof(*tfd));

    if (pipe_to_release) {
        if (pipe_type == FD_TYPE_PIPE_R) pipe_to_release->readers--;
        else pipe_to_release->writers--;
        waitq_wake_all(&pipe_to_release->read_waitq);
        waitq_wake_all(&pipe_to_release->write_waitq);
        if (pipe_to_release->readers <= 0 && pipe_to_release->writers <= 0) {
            kfree(pipe_to_release);
        }
    }

    if (node_to_close) {
        vfs_close(node_to_close);
    }
}

static int fd_alloc_from(int start) {
    int capacity;

    if (!current_task) return -1;
    capacity = current_task->fds_capacity;
    for (int i = start; i < capacity; i++) {
        if (!fd_table[i].in_use) {
            memset(&fd_table[i], 0, sizeof(task_fd_t));
            fd_table[i].in_use = 1;
            fd_table[i].ref_count = 1;
            return i;
        }
    }
    return -1;
}

static int fd_alloc(void) {
    return fd_alloc_from(0);
}

static int sys_dup(int oldfd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !fd_table[oldfd].in_use) return -EBADF;
    int newfd = fd_alloc();
    if (newfd < 0) return -EMFILE;
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(task_fd_t));
    fd_table[newfd].ref_count = 1;
    if (fd_table[oldfd].private_data && (fd_table[oldfd].type == FD_TYPE_PIPE_R || fd_table[oldfd].type == FD_TYPE_PIPE_W)) {
        pipe_t *p = (pipe_t *)fd_table[oldfd].private_data;
        if (fd_table[oldfd].type == FD_TYPE_PIPE_R) p->readers++;
        else p->writers++;
    }
    return newfd;
}

static int sys_dup2(int oldfd, const char *newfd_ptr, int unused) {
    (void)unused;
    int newfd = (int)(uintptr_t)newfd_ptr;
    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !fd_table[oldfd].in_use) return -EBADF;
    if (newfd < 0 || newfd >= current_task->fds_capacity) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (fd_table[newfd].in_use) {
        fd_release_entry(&fd_table[newfd]);
    }
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(task_fd_t));
    fd_table[newfd].ref_count = 1;
    if (fd_table[oldfd].private_data && (fd_table[oldfd].type == FD_TYPE_PIPE_R || fd_table[oldfd].type == FD_TYPE_PIPE_W)) {
        pipe_t *p = (pipe_t *)fd_table[oldfd].private_data;
        if (fd_table[oldfd].type == FD_TYPE_PIPE_R) p->readers++;
        else p->writers++;
    }
    return newfd;
}

static int sys_pipe(int pipefd_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    uint32_t addr = (uint32_t)pipefd_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    int *pipefd = (int *)addr;
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(pipe_t));
    waitq_init(&p->read_waitq);
    waitq_init(&p->write_waitq);
    p->readers = 1;
    p->writers = 1;
    int rfd = fd_alloc();
    if (rfd < 0) { kfree(p); return -EMFILE; }
    int wfd = fd_alloc();
    if (wfd < 0) { fd_table[rfd].in_use = 0; kfree(p); return -EMFILE; }
    fd_table[rfd].type = FD_TYPE_PIPE_R;
    fd_table[rfd].private_data = p;
    fd_table[wfd].type = FD_TYPE_PIPE_W;
    fd_table[wfd].private_data = p;
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

static int sys_getcwd(int buf_ptr, const char *size_ptr, int unused) {
    (void)unused;
    uint32_t buf_addr = (uint32_t)buf_ptr;
    uint32_t size = (uint32_t)(uintptr_t)size_ptr;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (size == 0) return -1;
    const char *cwd = current_task ? current_task->cwd : "/";
    if (!cwd[0]) cwd = "/";
    uint32_t len = 0;
    while (cwd[len]) len++;
    if (len + 1 > size) return -1;
    memcpy((void *)buf_addr, cwd, len + 1);
    return buf_ptr;
}

static int sys_chdir(int path_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint32_t addr = (uint32_t)path_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    const char *path = (const char *)addr;
    if (strncmp(path, "/ro", 3) == 0 && (path[3] == '\0' || path[3] == '/')) {
        return -EACCES;
    }
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;
    if (!current_task) return -EFAULT;
    char *resolved = vfs_get_path(node, current_task->cwd, sizeof(current_task->cwd));
    if (!resolved) {
        int i = 0;
        while (path[i] && i < 254) { current_task->cwd[i] = path[i]; i++; }
        current_task->cwd[i] = '\0';
    }
    return 0;
}

static int sys_access(int path_ptr, const char *mode_ptr, int unused) {
    (void)unused;
    uint32_t addr = (uint32_t)path_ptr;
    int mode = (int)(uintptr_t)mode_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    const char *path = (const char *)addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    (void)mode;
    return 0;
}

static inline uint32_t vfs_mask_to_unix_perms(uint32_t mask) {
    if (mask != 0 && (mask & ~0x07u) == 0) {
        uint32_t perms = 0;
        if (mask & VFS_PERM_READ) perms |= 0444;
        if (mask & VFS_PERM_WRITE) perms |= 0222;
        if (mask & VFS_PERM_EXEC) perms |= 0111;
        return perms;
    }
    return mask & 07777u;
}

static inline uint32_t vfs_node_to_unix_mode(const vfs_node_t *node) {
    uint32_t perms = 0;
    if (node && node->mask) {
        perms = vfs_mask_to_unix_perms(node->mask);
    }

    if (!node) {
        return S_IFREG | 0644;
    }

    switch (VFS_GET_TYPE(node->flags)) {
        case VFS_DIRECTORY:
            return S_IFDIR | (perms ? perms : 0755);
        case VFS_SYMLINK:
            return S_IFLNK | (perms ? perms : 0777);
        case VFS_CHARDEVICE:
            return S_IFCHR | (perms ? perms : 0660);
        case VFS_BLOCKDEVICE:
            return S_IFBLK | (perms ? perms : 0660);
        case VFS_PIPE:
            return S_IFIFO | (perms ? perms : 0644);
        default:
            return S_IFREG | (perms ? perms : 0644);
    }
}

static int sys_stat(int path_ptr, const char *buf_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    uint32_t buf_addr = (uint32_t)(uintptr_t)buf_ptr;
    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    const char *path = (const char *)path_addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    struct kernel_stat *st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    st->st_dev = 1;
    st->st_ino = node->inode ? node->inode : 1;
    st->__st_ino_truncated = (long)st->st_ino;

    st->st_mode = vfs_node_to_unix_mode(node);
    st->st_nlink = 1;
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_rdev = 0;
    st->st_size = node->length;
    st->st_blksize = 4096;
    st->st_blocks = (node->length + 511) / 512;
    st->__st_atim32.tv_sec = node->atime;
    st->__st_mtim32.tv_sec = node->mtime;
    st->__st_ctim32.tv_sec = node->ctime;
    st->st_atim.tv_sec = node->atime;
    st->st_mtim.tv_sec = node->mtime;
    st->st_ctim.tv_sec = node->ctime;
    return 0;
}

static int sys_fstat(int fd, const char *buf_ptr, int unused) {
    uint32_t buf_addr;
    struct kernel_stat *st;
    int pty_fd;
    uint64_t size;
    uint32_t flags;
    int ret;

    (void)unused;
    buf_addr = (uint32_t)(uintptr_t)buf_ptr;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    
    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR | 0620;
        st->st_rdev = 0x8801;
        st->st_blksize = 1024;
        st->st_nlink = 1;
        return 0;
    }
    
    if (fd >= 0 && fd < current_task->fds_capacity && fd_table[fd].in_use) {
        if (fd_table[fd].type == FD_TYPE_PIPE_R || fd_table[fd].type == FD_TYPE_PIPE_W) {
            st->st_mode = S_IFIFO | 0600;
            st->st_blksize = 4096;
            st->st_nlink = 1;
            return 0;
        }
        
        if (fd_table[fd].private_data) {
            pty_fd = (int)(uintptr_t)fd_table[fd].private_data;
            if (is_pty_master(pty_fd) || is_pty_slave(pty_fd)) {
                st->st_mode = S_IFCHR | 0620;
                st->st_rdev = 0x8801;
                st->st_blksize = 1024;
                st->st_nlink = 1;
                return 0;
            }
        }
        
        vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
        if (node) {
            uint32_t na = (uint32_t)node;
            if ((na & 0xFFFF0000u) == 0xFEFE0000u) {
                node = NULL;
            }
        }
        if (node) {
            st->st_dev = 1;
            st->st_ino = node->inode ? node->inode : 1;
            st->__st_ino_truncated = (long)st->st_ino;

            st->st_mode = vfs_node_to_unix_mode(node);
            st->st_nlink = 1;
            st->st_uid = node->uid;
            st->st_gid = node->gid;
            st->st_size = node->length;
            st->st_blksize = 4096;
            st->st_blocks = (node->length + 511) / 512;
            st->st_atim.tv_sec = node->atime;
            st->st_mtim.tv_sec = node->mtime;
            st->st_ctim.tv_sec = node->ctime;
            st->__st_atim32.tv_sec = node->atime;
            st->__st_mtim32.tv_sec = node->mtime;
            st->__st_ctim32.tv_sec = node->ctime;
            return 0;
        }
    }
    
    size = 0;
    flags = 0;
    ret = vfs_stat_fd(fd, &size, &flags);
    if (ret < 0) return -EBADF;
    st->st_dev = 1;
    st->st_ino = 1;
    st->__st_ino_truncated = 1;
    if (VFS_GET_TYPE(flags) == VFS_DIRECTORY) st->st_mode = S_IFDIR | 0755;
    else st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511) / 512;
    return 0;
}

static int sys_lseek_new(int fd, const char *offset_ptr, int whence) {
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type == FD_TYPE_PIPE_R || tfd->type == FD_TYPE_PIPE_W) return -ESPIPE;
    if (tfd->type == FD_TYPE_STDIN || tfd->type == FD_TYPE_STDOUT || tfd->type == FD_TYPE_STDERR) return -ESPIPE;
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;

    vfs_node_t *node = (vfs_node_t *)tfd->node;
    if (!vfs_node_ptr_sane(node)) return -EBADF;

    int32_t offset = (int32_t)(uintptr_t)offset_ptr;
    int32_t base;
    switch (whence) {
        case VFS_SEEK_SET:
            base = 0;
            break;
        case VFS_SEEK_CUR:
            base = (int32_t)tfd->offset;
            break;
        case VFS_SEEK_END:
            base = (int32_t)node->length;
            break;
        default:
            return -EINVAL;
    }

    int32_t new_offset = base + offset;
    if (new_offset < 0) return -EINVAL;
    tfd->offset = (uint32_t)new_offset;
    return (int)new_offset;
}

static unsigned long sig_mask = 0;
static struct kernel_sigaction sig_handlers[32];

static int sys_sigaction(int signum, const char *act_ptr, int oldact_ptr) {
    if (signum < 1 || signum >= 32) return -1;
    uint32_t act_addr = (uint32_t)(uintptr_t)act_ptr;
    uint32_t old_addr = (uint32_t)oldact_ptr;
    if (old_addr && old_addr < 0xC0000000 && old_addr >= 0x1000) {
        memcpy((void *)old_addr, &sig_handlers[signum], sizeof(struct kernel_sigaction));
    }
    if (act_addr && act_addr < 0xC0000000 && act_addr >= 0x1000) {
        memcpy(&sig_handlers[signum], (void *)act_addr, sizeof(struct kernel_sigaction));
    }
    return 0;
}

static int sys_sigprocmask(int how, const char *set_ptr, int oldset_ptr) {
    uint32_t set_addr = (uint32_t)(uintptr_t)set_ptr;
    uint32_t old_addr = (uint32_t)oldset_ptr;
    if (old_addr && old_addr < 0xC0000000 && old_addr >= 0x1000) {
        *(unsigned long *)old_addr = sig_mask;
    }
    if (set_addr && set_addr < 0xC0000000 && set_addr >= 0x1000) {
        unsigned long new_mask = *(unsigned long *)set_addr;
        switch (how) {
            case 0: sig_mask |= new_mask; break;
            case 1: sig_mask &= ~new_mask; break;
            case 2: sig_mask = new_mask; break;
            default: return -1;
        }
    }
    return 0;
}

extern volatile uint32_t tick_count;
#define pit_ticks tick_count

static uint32_t boot_time = 0;
static uint32_t boot_tick_count_posix = 0;

static int sys_clock_gettime(int clock_id, const char *tp_ptr, int unused) {
    uint32_t tp_addr;
    struct kernel_timespec *ts;
    uint32_t ticks;
    uint32_t elapsed_ticks;
    uint32_t ms;
    
    (void)unused;
    if (boot_time == 0) {
        extern uint32_t rtc_get_time(void);
        boot_time = rtc_get_time();
        boot_tick_count_posix = tick_count;
    }
    tp_addr = (uint32_t)(uintptr_t)tp_ptr;
    if (!tp_addr || tp_addr >= 0xC0000000 || tp_addr < 0x1000) return -1;
    ts = (struct kernel_timespec *)tp_addr;
    ticks = pit_ticks;
    elapsed_ticks = ticks - boot_tick_count_posix;
    ms = (elapsed_ticks * 1000) / pit_freq;
    ts->tv_sec = boot_time + ms / 1000;
    ts->tv_nsec = (ms % 1000) * 1000000;
    (void)clock_id;
    return 0;
}

static int sys_gettimeofday(int tv_ptr, const char *tz_ptr, int unused) {
    uint32_t tv_addr;
    struct kernel_timeval *tv;
    uint32_t ticks;
    uint32_t elapsed_ticks;
    uint32_t ms;
    
    (void)tz_ptr; (void)unused;
    if (boot_time == 0) {
        extern uint32_t rtc_get_time(void);
        boot_time = rtc_get_time();
        boot_tick_count_posix = tick_count;
    }
    tv_addr = (uint32_t)tv_ptr;
    if (!tv_addr || tv_addr >= 0xC0000000 || tv_addr < 0x1000) return -1;
    tv = (struct kernel_timeval *)tv_addr;
    ticks = pit_ticks;
    elapsed_ticks = ticks - boot_tick_count_posix;
    ms = (elapsed_ticks * 1000) / pit_freq;
    tv->tv_sec = boot_time + ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

static int sys_munmap(int addr, const char *len_ptr, int unused) {
    (void)addr; (void)len_ptr; (void)unused;
    return 0;
}

static int sys_mprotect(int addr, const char *len_ptr, int prot) {
    (void)addr; (void)len_ptr; (void)prot;
    return 0;
}

static int sys_execve(int path_ptr, const char *argv_ptr, int envp_ptr) {
    registers_t *regs;
    uint8_t *buf;
    char **argv;
    char **envp;
    vfs_node_t *node;
    uint32_t path_addr;
    uint32_t argv_addr;
    uint32_t envp_addr;
    uint32_t size;
    uint32_t read_len;
    int argc;
    int envc;
    int result;
    int i;
    const char *path;
    char shebang_interp[256];
    char shebang_arg[256];
    int shebang_has_arg;
    int shebang_line_end;
    int si;
    int sj;
    vfs_node_t *interp_node;
    uint8_t *interp_buf;
    uint32_t interp_size;
    uint32_t interp_read;

    path_addr = (uint32_t)path_ptr;
    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) {
        return -EFAULT;
    }
    path = (const char *)path_addr;
    
    
    argv_addr = (uint32_t)argv_ptr;
    envp_addr = (uint32_t)envp_ptr;
    
    node = vfs_namei(path);
    if (!node) {
        return -ENOENT;
    }
    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) {
        return -EACCES;
    }
    size = node->length;
    if (size == 0) {
        return -ENOEXEC;
    }
    
    buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        return -ENOMEM;
    }
    
    read_len = vfs_read(node, 0, size, buf);
    if (read_len != size) {
        kfree(buf);
        return -EIO;
    }
    
    if (size >= 2 && buf[0] == '#' && buf[1] == '!') {
        shebang_line_end = 2;
        while (shebang_line_end < (int)size && shebang_line_end < 256 &&
               buf[shebang_line_end] != '\n' && buf[shebang_line_end] != '\r') {
            shebang_line_end++;
        }

        si = 2;
        while (si < shebang_line_end && (buf[si] == ' ' || buf[si] == '\t')) si++;
        sj = 0;
        while (si < shebang_line_end && buf[si] != ' ' && buf[si] != '\t' &&
               sj < 255) {
            shebang_interp[sj++] = buf[si++];
        }
        shebang_interp[sj] = '\0';
        if (sj == 0) {
            kfree(buf);
            return -ENOEXEC;
        }

        shebang_has_arg = 0;
        while (si < shebang_line_end && (buf[si] == ' ' || buf[si] == '\t')) si++;
        if (si < shebang_line_end) {
            sj = 0;
            while (si < shebang_line_end && sj < 255) {
                shebang_arg[sj++] = buf[si++];
            }
            while (sj > 0 && (shebang_arg[sj - 1] == ' ' || shebang_arg[sj - 1] == '\t')) sj--;
            shebang_arg[sj] = '\0';
            if (sj > 0) shebang_has_arg = 1;
        }

        kfree(buf);

        interp_node = vfs_namei(shebang_interp);
        if (!interp_node) {
            return -ENOENT;
        }
        interp_size = interp_node->length;
        if (interp_size == 0) {
            return -ENOEXEC;
        }
        interp_buf = (uint8_t *)kmalloc(interp_size);
        if (!interp_buf) {
            return -ENOMEM;
        }
        interp_read = vfs_read(interp_node, 0, interp_size, interp_buf);
        if (interp_read != interp_size) {
            kfree(interp_buf);
            return -EIO;
        }

        buf = interp_buf;
        size = interp_size;

        argc = 0;
        if (argv_addr) {
            uint32_t *argv_array = (uint32_t *)argv_addr;
            while (argv_array[argc] && argc < 256) {
                argc++;
            }
        }

        {
            int new_argc;
            char **new_argv;
            int na;
            int kern_args;

            new_argc = 1 + shebang_has_arg + 1 + (argc > 1 ? argc - 1 : 0);
            new_argv = (char **)kmalloc((new_argc + 1) * sizeof(char *));
            if (!new_argv) {
                kfree(buf);
                return -ENOMEM;
            }

            na = 0;
            new_argv[na] = (char *)kmalloc(strlen(shebang_interp) + 1);
            if (!new_argv[na]) { kfree(new_argv); kfree(buf); return -ENOMEM; }
            strcpy(new_argv[na], shebang_interp);
            na++;

            if (shebang_has_arg) {
                new_argv[na] = (char *)kmalloc(strlen(shebang_arg) + 1);
                if (!new_argv[na]) {
                    for (i = 0; i < na; i++) kfree(new_argv[i]);
                    kfree(new_argv); kfree(buf); return -ENOMEM;
                }
                strcpy(new_argv[na], shebang_arg);
                na++;
            }

            new_argv[na] = (char *)kmalloc(strlen(path) + 1);
            if (!new_argv[na]) {
                for (i = 0; i < na; i++) kfree(new_argv[i]);
                kfree(new_argv); kfree(buf); return -ENOMEM;
            }
            strcpy(new_argv[na], path);
            na++;

            kern_args = na;

            if (argc > 1 && argv_addr) {
                uint32_t *argv_array = (uint32_t *)argv_addr;
                for (i = 1; i < argc; i++) {
                    uint32_t str_addr = argv_array[i];
                    if (!str_addr || str_addr >= 0xC0000000 || str_addr < 0x1000) {
                        for (sj = 0; sj < kern_args; sj++) kfree(new_argv[sj]);
                        kfree(new_argv); kfree(buf); return -EFAULT;
                    }
                    new_argv[na] = (char *)str_addr;
                    na++;
                }
            }
            new_argv[na] = NULL;

            envc = 0;
            envp = NULL;
            if (envp_addr) {
                uint32_t *envp_array = (uint32_t *)envp_addr;
                while (envp_array[envc] && envc < 256) {
                    envc++;
                }
                if (envc > 0) {
                    envp = (char **)kmalloc((envc + 1) * sizeof(char *));
                    if (!envp) {
                        for (i = 0; i < kern_args; i++) kfree(new_argv[i]);
                        kfree(new_argv); kfree(buf); return -ENOMEM;
                    }
                    for (i = 0; i < envc; i++) {
                        uint32_t str_addr = envp_array[i];
                        if (!str_addr || str_addr >= 0xC0000000 || str_addr < 0x1000) {
                            kfree(envp);
                            for (sj = 0; sj < kern_args; sj++) kfree(new_argv[sj]);
                            kfree(new_argv); kfree(buf); return -EFAULT;
                        }
                        envp[i] = (char *)str_addr;
                    }
                    envp[envc] = NULL;
                }
            }

            regs = current_task->syscall_frame;
            if (!regs) {
                if (envp) kfree(envp);
                for (i = 0; i < kern_args; i++) kfree(new_argv[i]);
                kfree(new_argv); kfree(buf); return -EFAULT;
            }

            result = task_exec_with_args(buf, size, regs, na, new_argv, envc, envp);
            buf = NULL;
            if (envp) { kfree(envp); envp = NULL; }
            for (i = 0; i < kern_args; i++) kfree(new_argv[i]);
            kfree(new_argv);

            if (result == 0) {
                syscall_set_exec_completed();
                if (current_task) {
                    current_task->exec_completed = 1;
                }
                __asm__ volatile ("" ::: "memory");
            }
            return result;
        }
    }
    
    argc = 0;
    argv = NULL;
    if (argv_addr) {
        uint32_t *argv_array = (uint32_t *)argv_addr;
        while (argv_array[argc] && argc < 256) {
            argc++;
        }
        if (argc > 0) {
            argv = (char **)kmalloc((argc + 1) * sizeof(char *));
            if (!argv) {
                kfree(buf);
                return -ENOMEM;
            }
            for (i = 0; i < argc; i++) {
                uint32_t str_addr = argv_array[i];
                if (!str_addr || str_addr >= 0xC0000000 || str_addr < 0x1000) {
                    kfree(argv);
                    kfree(buf);
                    return -EFAULT;
                }
                argv[i] = (char *)str_addr;
            }
            argv[argc] = NULL;
        }
    }

    if (argc == 0) {
        argv = (char **)kmalloc(2 * sizeof(char *));
        if (!argv) {
            kfree(buf);
            return -ENOMEM;
        }
        argv[0] = (char *)path_addr;
        argv[1] = NULL;
        argc = 1;
    }
    
    envc = 0;
    envp = NULL;
    if (envp_addr) {
        uint32_t *envp_array = (uint32_t *)envp_addr;
        while (envp_array[envc] && envc < 256) {
            envc++;
        }
        if (envc > 0) {
            envp = (char **)kmalloc((envc + 1) * sizeof(char *));
            if (!envp) {
                if (argv) kfree(argv);
                kfree(buf);
                return -ENOMEM;
            }
            for (i = 0; i < envc; i++) {
                uint32_t str_addr = envp_array[i];
                if (!str_addr || str_addr >= 0xC0000000 || str_addr < 0x1000) {
                    kfree(envp);
                    if (argv) kfree(argv);
                    kfree(buf);
                    return -EFAULT;
                }
                envp[i] = (char *)str_addr;
            }
            envp[envc] = NULL;
        }
    }
    
    regs = current_task->syscall_frame;
    if (!regs) {
        if (envp) kfree(envp);
        if (argv) kfree(argv);
        kfree(buf);
        return -EFAULT;
    }
    
    result = task_exec_with_args(buf, size, regs, argc, argv, envc, envp);
    
    buf = NULL;
    
    if (envp) {
        kfree(envp);
        envp = NULL;
    }
    
    
    if (argv) {
        kfree(argv);
        argv = NULL;
    }
    
    
    if (result == 0) {
        syscall_set_exec_completed();
        if (current_task) {
            current_task->exec_completed = 1;
        }
        
        __asm__ volatile ("" ::: "memory");
    }
    
    return result;
}

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_DUPFD_CLOEXEC 1030

static int sys_fcntl(int fd, const char *cmd_ptr, int arg) {
    int cmd = (int)(uintptr_t)cmd_ptr;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int minfd = (arg < 0) ? 0 : arg;
            if (minfd >= current_task->fds_capacity) return -EINVAL;
            int newfd = fd_alloc_from(minfd);
            if (newfd < 0) return -EMFILE;
            memcpy(&fd_table[newfd], &fd_table[fd], sizeof(task_fd_t));
            fd_table[newfd].ref_count = 1;
            if (fd_table[fd].private_data && (fd_table[fd].type == FD_TYPE_PIPE_R || fd_table[fd].type == FD_TYPE_PIPE_W)) {
                pipe_t *p = (pipe_t *)fd_table[fd].private_data;
                if (fd_table[fd].type == FD_TYPE_PIPE_R) p->readers++;
                else p->writers++;
            }
            return newfd;
        }
        case F_GETFD:
            return (fd_table[fd].flags & 1) ? 1 : 0;
        case F_SETFD:
            if (arg & 1)
                fd_table[fd].flags |= 1;
            else
                fd_table[fd].flags &= ~1;
            return 0;
        case F_GETFL:
            return fd_table[fd].flags & ~1;
        case F_SETFL:
            fd_table[fd].flags = (fd_table[fd].flags & 1) | (arg & ~1);
            return 0;
        default:
            return -EINVAL;
    }
}

static int sys_truncate(int path_ptr, const char *len_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    uint32_t length = (uint32_t)(uintptr_t)len_ptr;
    
    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    
    const char *path = (const char *)path_addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -1;

    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -1;
    
    if (node->truncate) {
        return node->truncate(node, length);
    }
    
    return -1;
}

static int sys_ftruncate(int fd, const char *len_ptr, int unused) {
    (void)unused;
    uint32_t length = (uint32_t)(uintptr_t)len_ptr;
    
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;

    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -EISDIR;
    
    if (node->truncate) {
        return node->truncate(node, length);
    }
    
    return -ENOSYS;
}

static int sys_umask(int mask, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    static int current_mask = 022;
    int old = current_mask;
    current_mask = mask & 0777;
    return old;
}

struct linux_dirent {
    unsigned long d_ino;
    unsigned long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static int sys_getdents(int fd, const char *dirp_ptr, int count) {
    uint32_t dirp_addr;
    task_fd_t *tfd;
    vfs_node_t *node;
    uint8_t *buf;
    int written;
    uint32_t dir_offset;
    dirent_t *entry;
    dirent_t local_copy;
    int name_len;
    int reclen;
    struct linux_dirent *de;
    uint32_t flags;
    int i;
    int guard;

    dirp_addr = (uint32_t)(uintptr_t)dirp_ptr;
    if (!current_task) return -ESRCH;
    if (!dirp_addr || dirp_addr >= 0xC0000000 || dirp_addr < 0x1000) return -EFAULT;
    if (count <= 0) return -EINVAL;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (!vfs_node_ptr_sane(node)) return -EBADF;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;

    buf = (uint8_t *)dirp_addr;
    written = 0;
    dir_offset = tfd->offset;
    guard = 0;

    while (written < count && guard < 4096) {
        entry = vfs_readdir(node, dir_offset);
        if (!entry) break;

        __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
        for (i = 0; i < VFS_MAX_NAME; i++) {
            local_copy.name[i] = entry->name[i];
        }
        local_copy.inode = entry->inode;
        local_copy.type = entry->type;
        __asm__ volatile("push %0; popf" : : "r"(flags));

        name_len = 0;
        while (local_copy.name[name_len] && name_len < 63) name_len++;
        reclen = (int)sizeof(struct linux_dirent) + name_len + 1;
        reclen = (reclen + 3) & ~3;

        if (written + reclen > count) break;

        de = (struct linux_dirent *)(buf + written);
        de->d_ino = local_copy.inode ? local_copy.inode : dir_offset + 1;
        de->d_off = dir_offset + 1;
        de->d_reclen = (unsigned short)reclen;

        if (local_copy.type == VFS_DIRECTORY) de->d_type = 4;
        else if (local_copy.type == VFS_FILE) de->d_type = 8;
        else if (local_copy.type == VFS_SYMLINK) de->d_type = 10;
        else if (local_copy.type == VFS_CHARDEVICE) de->d_type = 2;
        else if (local_copy.type == VFS_BLOCKDEVICE) de->d_type = 6;
        else if (local_copy.type == VFS_PIPE) de->d_type = 1;
        else de->d_type = 0;

        for (i = 0; i < name_len; i++) {
            de->d_name[i] = local_copy.name[i];
        }
        de->d_name[name_len] = '\0';

        written += reclen;
        dir_offset++;
        guard++;
    }

    tfd->offset = dir_offset;
    return written;
}

static int sys_rename(int oldpath_ptr, const char *newpath_ptr, int unused) {
    uint32_t old_addr = (uint32_t)oldpath_ptr;
    uint32_t new_addr = (uint32_t)(uintptr_t)newpath_ptr;
    (void)unused;
    
    if (!old_addr || old_addr >= 0xC0000000 || old_addr < 0x1000) return -1;
    if (!new_addr || new_addr >= 0xC0000000 || new_addr < 0x1000) return -1;
    
    const char *oldpath = (const char *)old_addr;
    const char *newpath = (const char *)new_addr;
    
    vfs_node_t *old_node = vfs_namei(oldpath);
    if (!old_node) return -1;
    
    vfs_node_t *old_parent = old_node->parent;
    if (!old_parent || !old_parent->rename) return -1;
    
    char new_parent_path[256];
    char new_name[64];
    int len = 0;
    while (newpath[len]) len++;
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (newpath[i] == '/') last_slash = i;
    }
    
    vfs_node_t *new_parent;
    if (last_slash <= 0) {
        new_parent_path[0] = '/';
        new_parent_path[1] = '\0';
        int j = (last_slash == 0) ? 1 : 0;
        int k = 0;
        while (newpath[j] && k < 63) new_name[k++] = newpath[j++];
        new_name[k] = '\0';
    } else {
        for (int i = 0; i < last_slash && i < 255; i++) new_parent_path[i] = newpath[i];
        new_parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        int k = 0;
        for (int i = last_slash + 1; i < len && k < 63; i++) new_name[k++] = newpath[i];
        new_name[k] = '\0';
    }
    
    new_parent = vfs_namei(new_parent_path);
    if (!new_parent) return -1;
    
    char old_name[64];
    int old_len = 0;
    while (oldpath[old_len]) old_len++;
    int old_last_slash = -1;
    for (int i = 0; i < old_len; i++) {
        if (oldpath[i] == '/') old_last_slash = i;
    }
    int k = 0;
    for (int i = old_last_slash + 1; i < old_len && k < 63; i++) old_name[k++] = oldpath[i];
    old_name[k] = '\0';
    
    return old_parent->rename(old_parent, old_name, new_parent, new_name);
}

static int sys_link(int oldpath_ptr, const char *newpath_ptr, int unused) {
    (void)oldpath_ptr; (void)newpath_ptr; (void)unused;
    return -1;
}

static int sys_symlink(int target_ptr, const char *linkpath_ptr, int unused) {
    (void)target_ptr; (void)linkpath_ptr; (void)unused;
    return -1;
}

static int sys_readlink(int path_ptr, const char *buf_ptr, int bufsiz) {
    uint32_t path_addr = (uint32_t)path_ptr;
    uint32_t buf_addr = (uint32_t)(uintptr_t)buf_ptr;

    if (!current_task) return -ESRCH;
    if (bufsiz <= 0) return -EINVAL;

    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint32_t)bufsiz >= 0xC0000000) return -EFAULT;

    vfs_node_t *node = vfs_namei_nofollow((const char *)path_addr);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) return -EINVAL;

    char target[VFS_MAX_PATH];
    uint32_t n = vfs_read(node, 0, sizeof(target) - 1, (uint8_t *)target);
    if (n >= sizeof(target)) n = sizeof(target) - 1;
    target[n] = '\0';

    uint32_t copy_len = n;
    if (copy_len > (uint32_t)bufsiz) copy_len = (uint32_t)bufsiz;

    for (uint32_t i = 0; i < copy_len; i++) {
        ((char *)buf_addr)[i] = target[i];
    }

    return (int)copy_len;
}

static int sys_dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !fd_table[oldfd].in_use) return -EBADF;
    if (newfd < 0 || newfd >= current_task->fds_capacity) return -EBADF;
    if (oldfd == newfd) return -EINVAL;
    if (fd_table[newfd].in_use) {
        fd_release_entry(&fd_table[newfd]);
    }
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(task_fd_t));
    fd_table[newfd].ref_count = 1;
    if (fd_table[oldfd].private_data && (fd_table[oldfd].type == FD_TYPE_PIPE_R || fd_table[oldfd].type == FD_TYPE_PIPE_W)) {
        pipe_t *p = (pipe_t *)fd_table[oldfd].private_data;
        if (fd_table[oldfd].type == FD_TYPE_PIPE_R) p->readers++;
        else p->writers++;
    }
    return newfd;
}

static int sys_pipe2(int *pipefd, int flags) {
    (void)flags;
    if (!current_task) return -ESRCH;
    if (!pipefd) return -EFAULT;
    uint32_t addr = (uint32_t)pipefd;
    if (addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(pipe_t));
    waitq_init(&p->read_waitq);
    waitq_init(&p->write_waitq);
    p->readers = 1;
    p->writers = 1;
    int rfd = fd_alloc();
    if (rfd < 0) { kfree(p); return -EMFILE; }
    int wfd = fd_alloc();
    if (wfd < 0) { fd_table[rfd].in_use = 0; kfree(p); return -EMFILE; }
    fd_table[rfd].type = FD_TYPE_PIPE_R;
    fd_table[rfd].private_data = p;
    fd_table[wfd].type = FD_TYPE_PIPE_W;
    fd_table[wfd].private_data = p;
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

static int sys_fchdir(int fd) {
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;
    char *resolved = vfs_get_path(node, current_task->cwd, sizeof(current_task->cwd));
    (void)resolved;
    return 0;
}

static int sys_fchmod(int fd, int mode) {
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (node->chmod) {
        return node->chmod(node, mode & 07777);
    }
    node->mask = mode & 07777;
    return 0;
}

static int sys_fchown(int fd, int uid, int gid) {
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (node->chown) {
        return node->chown(node, uid, gid);
    }
    if (uid != -1) node->uid = uid;
    if (gid != -1) node->gid = gid;
    return 0;
}

static int sys_fsync(int fd) {
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    return 0;
}

static int sys_fdatasync(int fd) {
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    return 0;
}

static int sys_flock(int fd, int operation) {
    (void)operation;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    return 0;
}

static int sys_pread64(int fd, void *buf, size_t count, long long offset) {
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    if (!buf) return -EFAULT;
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -EISDIR;
    return vfs_read(node, (uint32_t)offset, count, buf);
}

static int sys_pwrite64(int fd, const void *buf, size_t count, long long offset) {
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    if (!buf) return -EFAULT;
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -EISDIR;
    return vfs_write(node, (uint32_t)offset, count, (uint8_t *)buf);
}

struct iovec {
    void *iov_base;
    size_t iov_len;
};

static int sys_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!iov || iovcnt <= 0) return -EINVAL;
    
    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        if (!iov[i].iov_base) continue;
        
        int ret = vfs_read_fd(fd, iov[i].iov_base, iov[i].iov_len);
        if (ret < 0) {
            if (total > 0) return total;
            return ret;
        }
        total += ret;
        if ((size_t)ret < iov[i].iov_len) break;
    }
    return total;
}

struct linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static int sys_getdents64(int fd, void *dirp, unsigned int count) {
    uint32_t dirp_addr;
    task_fd_t *tfd;
    vfs_node_t *node;
    uint8_t *buf;
    int written;
    uint32_t dir_offset;
    dirent_t *entry;
    dirent_t local_copy;
    int name_len;
    int reclen;
    struct linux_dirent64 *de;
    uint32_t flags;
    int i;
    int guard;

    dirp_addr = (uint32_t)dirp;
    if (!current_task) return -ESRCH;
    if (!dirp_addr || dirp_addr >= 0xC0000000 || dirp_addr < 0x1000) return -EFAULT;
    if (count == 0) return -EINVAL;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (!vfs_node_ptr_sane(node)) return -EBADF;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;

    buf = (uint8_t *)dirp;
    written = 0;
    dir_offset = tfd->offset;
    guard = 0;
    
    while ((unsigned int)written < count && guard < 4096) {
        entry = vfs_readdir(node, dir_offset);
        if (!entry) break;

        __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
        for (i = 0; i < VFS_MAX_NAME; i++) {
            local_copy.name[i] = entry->name[i];
        }
        local_copy.inode = entry->inode;
        local_copy.type = entry->type;
        __asm__ volatile("push %0; popf" : : "r"(flags));
        
        name_len = 0;
        while (local_copy.name[name_len] && name_len < 63) name_len++;
        reclen = sizeof(unsigned long long) + sizeof(long long) + sizeof(unsigned short) + sizeof(unsigned char) + name_len + 1;
        reclen = (reclen + 7) & ~7;
        
        if ((unsigned int)(written + reclen) > count) break;
        
        de = (struct linux_dirent64 *)(buf + written);
        de->d_ino = local_copy.inode ? local_copy.inode : dir_offset + 1;
        de->d_off = dir_offset + 1;
        de->d_reclen = reclen;
        
        if (local_copy.type == VFS_DIRECTORY) de->d_type = 4;
        else if (local_copy.type == VFS_FILE) de->d_type = 8;
        else if (local_copy.type == VFS_SYMLINK) de->d_type = 10;
        else if (local_copy.type == VFS_CHARDEVICE) de->d_type = 2;
        else if (local_copy.type == VFS_BLOCKDEVICE) de->d_type = 6;
        else if (local_copy.type == VFS_PIPE) de->d_type = 1;
        else de->d_type = 0;
        
        for (i = 0; i < name_len; i++) {
            de->d_name[i] = local_copy.name[i];
        }
        de->d_name[name_len] = '\0';
        
        written += reclen;
        dir_offset++;
        guard++;
    }
    
    tfd->offset = dir_offset;
    return written;
}

void syscalls_posix_init(void) {
    memset(sig_handlers, 0, sizeof(sig_handlers));
    syscall_table[SYSCALL_DUP] = sys_dup;
    syscall_table[SYSCALL_DUP2] = sys_dup2;
    syscall_table[SYSCALL_DUP3] = sys_dup3;
    syscall_table[SYSCALL_PIPE] = sys_pipe;
    syscall_table[SYSCALL_PIPE2] = sys_pipe2;
    syscall_table[SYSCALL_SIGACTION] = sys_sigaction;
    syscall_table[SYSCALL_SIGPROCMASK] = sys_sigprocmask;
    syscall_table[SYSCALL_STAT] = sys_stat;
    syscall_table[SYSCALL_FSTAT] = sys_fstat;
    syscall_table[SYSCALL_GETCWD] = sys_getcwd;
    syscall_table[SYSCALL_CHDIR] = sys_chdir;
    syscall_table[SYSCALL_FCHDIR] = sys_fchdir;
    syscall_table[SYSCALL_ACCESS] = sys_access;
    syscall_table[SYSCALL_CLOCK_GETTIME] = sys_clock_gettime;
    syscall_table[SYSCALL_GETTIMEOFDAY] = sys_gettimeofday;
    syscall_table[SYSCALL_MUNMAP] = sys_munmap;
    syscall_table[SYSCALL_MPROTECT] = sys_mprotect;
    syscall_table[SYSCALL_EXECVE] = sys_execve;
    syscall_table[SYSCALL_LSEEK] = sys_lseek_new;
    syscall_table[SYSCALL_FCNTL] = sys_fcntl;
    syscall_table[SYSCALL_TRUNCATE] = sys_truncate;
    syscall_table[SYSCALL_FTRUNCATE] = sys_ftruncate;
    syscall_table[SYSCALL_UMASK] = sys_umask;
    syscall_table[SYSCALL_RENAME] = sys_rename;
    syscall_table[SYSCALL_LINK] = sys_link;
    syscall_table[SYSCALL_SYMLINK] = sys_symlink;
    syscall_table[SYSCALL_READLINK] = sys_readlink;
    syscall_table[SYSCALL_GETDENTS] = sys_getdents;
    syscall_table[SYSCALL_GETDENTS64] = sys_getdents64;
    syscall_table[SYSCALL_FCHMOD] = sys_fchmod;
    syscall_table[SYSCALL_FCHOWN] = sys_fchown;
    syscall_table[SYSCALL_FSYNC] = sys_fsync;
    syscall_table[SYSCALL_FDATASYNC] = sys_fdatasync;
    syscall_table[SYSCALL_FLOCK] = sys_flock;
    syscall_table[SYSCALL_PREAD64] = sys_pread64;
    syscall_table[SYSCALL_PWRITE64] = sys_pwrite64;
    syscall_table[SYSCALL_READV] = sys_readv;
}

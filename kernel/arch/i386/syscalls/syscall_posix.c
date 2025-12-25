#include "syscall_defs.h"
#include <kernel/pit.h>

#define MAX_FDS 64
#define PIPE_BUF_SIZE 4096

typedef struct {
    int in_use;
    int type;
    vfs_node_t *node;
    uint32_t offset;
    uint32_t flags;
    int ref_count;
    void *private_data;
} fd_entry_t;

typedef struct {
    uint8_t buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int readers;
    int writers;
    wait_queue_t read_waitq;
    wait_queue_t write_waitq;
} pipe_t;

static fd_entry_t fd_table[MAX_FDS];

#define FD_TYPE_FILE   0
#define FD_TYPE_PIPE_R 1
#define FD_TYPE_PIPE_W 2
#define FD_TYPE_STDIN  3
#define FD_TYPE_STDOUT 4
#define FD_TYPE_STDERR 5

static void fd_table_init(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].ref_count = 0;
    }
    fd_table[0].in_use = 1;
    fd_table[0].type = FD_TYPE_STDIN;
    fd_table[0].ref_count = 1;
    fd_table[1].in_use = 1;
    fd_table[1].type = FD_TYPE_STDOUT;
    fd_table[1].ref_count = 1;
    fd_table[2].in_use = 1;
    fd_table[2].type = FD_TYPE_STDERR;
    fd_table[2].ref_count = 1;
}

static int fd_alloc(void) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].in_use = 1;
            fd_table[i].ref_count = 1;
            return i;
        }
    }
    return -1;
}

static int sys_dup(int oldfd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (oldfd < 0 || oldfd >= MAX_FDS || !fd_table[oldfd].in_use) return -1;
    int newfd = fd_alloc();
    if (newfd < 0) return -1;
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(fd_entry_t));
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
    if (oldfd < 0 || oldfd >= MAX_FDS || !fd_table[oldfd].in_use) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    if (oldfd == newfd) return newfd;
    if (fd_table[newfd].in_use) {
        if (fd_table[newfd].private_data && (fd_table[newfd].type == FD_TYPE_PIPE_R || fd_table[newfd].type == FD_TYPE_PIPE_W)) {
            pipe_t *p = (pipe_t *)fd_table[newfd].private_data;
            if (fd_table[newfd].type == FD_TYPE_PIPE_R) p->readers--;
            else p->writers--;
            if (p->readers == 0 && p->writers == 0) kfree(p);
        }
        fd_table[newfd].in_use = 0;
    }
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(fd_entry_t));
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
    uint32_t addr = (uint32_t)pipefd_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -1;
    int *pipefd = (int *)addr;
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -1;
    memset(p, 0, sizeof(pipe_t));
    waitq_init(&p->read_waitq);
    waitq_init(&p->write_waitq);
    p->readers = 1;
    p->writers = 1;
    int rfd = fd_alloc();
    if (rfd < 0) { kfree(p); return -1; }
    int wfd = fd_alloc();
    if (wfd < 0) { fd_table[rfd].in_use = 0; kfree(p); return -1; }
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
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -1;
    const char *path = (const char *)addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -1;
    if (!(node->flags & VFS_DIRECTORY)) return -1;
    if (!current_task) return -1;
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
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -1;
    const char *path = (const char *)addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -1;
    (void)mode;
    return 0;
}

static int sys_stat(int path_ptr, const char *buf_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    uint32_t buf_addr = (uint32_t)(uintptr_t)buf_ptr;
    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    const char *path = (const char *)path_addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -1;
    struct kernel_stat *st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    st->st_dev = 1;
    st->st_ino = node->inode ? node->inode : 1;
    st->__st_ino_truncated = (long)st->st_ino;
    
    uint32_t mode;
    if (node->flags & VFS_DIRECTORY) mode = S_IFDIR | 0755;
    else if (node->flags & VFS_SYMLINK) mode = S_IFLNK | 0777;
    else if (node->flags & VFS_CHARDEVICE) mode = S_IFCHR | 0660;
    else if (node->flags & VFS_BLOCKDEVICE) mode = S_IFBLK | 0660;
    else if (node->flags & VFS_PIPE) mode = S_IFIFO | 0644;
    else mode = S_IFREG | (node->mask ? node->mask : 0644);
    
    st->st_mode = mode;
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
    (void)unused;
    uint32_t buf_addr = (uint32_t)(uintptr_t)buf_ptr;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    struct kernel_stat *st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    
    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR | 0620;
        st->st_rdev = 0x8801;
        st->st_blksize = 1024;
        st->st_nlink = 1;
        return 0;
    }
    
    if (fd >= 0 && fd < MAX_FDS && fd_table[fd].in_use) {
        if (fd_table[fd].type == FD_TYPE_PIPE_R || fd_table[fd].type == FD_TYPE_PIPE_W) {
            st->st_mode = S_IFIFO | 0600;
            st->st_blksize = 4096;
            st->st_nlink = 1;
            return 0;
        }
        
        vfs_node_t *node = fd_table[fd].node;
        if (node) {
            st->st_dev = 1;
            st->st_ino = node->inode ? node->inode : 1;
            st->__st_ino_truncated = (long)st->st_ino;
            
            uint32_t mode;
            if (node->flags & VFS_DIRECTORY) mode = S_IFDIR | 0755;
            else if (node->flags & VFS_SYMLINK) mode = S_IFLNK | 0777;
            else if (node->flags & VFS_CHARDEVICE) mode = S_IFCHR | 0660;
            else if (node->flags & VFS_BLOCKDEVICE) mode = S_IFBLK | 0660;
            else if (node->flags & VFS_PIPE) mode = S_IFIFO | 0644;
            else mode = S_IFREG | (node->mask ? node->mask : 0644);
            
            st->st_mode = mode;
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
    
    uint32_t size = 0, flags = 0;
    int ret = vfs_stat_fd(fd, &size, &flags);
    if (ret < 0) return -1;
    st->st_dev = 1;
    st->st_ino = 1;
    st->__st_ino_truncated = 1;
    if (flags & VFS_DIRECTORY) st->st_mode = S_IFDIR | 0755;
    else st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511) / 512;
    return 0;
}

static int sys_lseek_new(int fd, const char *offset_ptr, int whence) {
    if (fd < 0 || fd <= 2) return -1;
    int32_t offset = (int32_t)(uintptr_t)offset_ptr;
    return vfs_seek(fd, offset, whence);
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

static uint32_t boot_time = 1735000000;

static int sys_clock_gettime(int clock_id, const char *tp_ptr, int unused) {
    (void)unused;
    uint32_t tp_addr = (uint32_t)(uintptr_t)tp_ptr;
    if (!tp_addr || tp_addr >= 0xC0000000 || tp_addr < 0x1000) return -1;
    struct kernel_timespec *ts = (struct kernel_timespec *)tp_addr;
    uint32_t ticks = pit_ticks;
    uint32_t ms = (ticks * 1000) / pit_freq;
    ts->tv_sec = boot_time + ms / 1000;
    ts->tv_nsec = (ms % 1000) * 1000000;
    (void)clock_id;
    return 0;
}

static int sys_gettimeofday(int tv_ptr, const char *tz_ptr, int unused) {
    (void)tz_ptr; (void)unused;
    uint32_t tv_addr = (uint32_t)tv_ptr;
    if (!tv_addr || tv_addr >= 0xC0000000 || tv_addr < 0x1000) return -1;
    struct kernel_timeval *tv = (struct kernel_timeval *)tv_addr;
    uint32_t ticks = pit_ticks;
    uint32_t ms = (ticks * 1000) / pit_freq;
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
    uint32_t path_addr = (uint32_t)path_ptr;
    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    const char *path = (const char *)path_addr;
    
    uint32_t argv_addr = (uint32_t)argv_ptr;
    uint32_t envp_addr = (uint32_t)envp_ptr;
    
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    if (node->flags & VFS_DIRECTORY) return -EACCES;
    uint32_t size = node->length;
    if (size == 0 || size > 0x1000000) return -ENOEXEC;
    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (!buf) return -ENOMEM;
    uint32_t read_len = vfs_read(node, 0, size, buf);
    if (read_len != size) { kfree(buf); return -EIO; }
    
    int argc = 0;
    char **argv = NULL;
    if (argv_addr) {
        uint32_t *argv_array = (uint32_t *)argv_addr;
        while (argv_array[argc] && argc < 256) argc++;
        if (argc > 0) {
            argv = (char **)kmalloc(argc * sizeof(char *));
            if (!argv) { kfree(buf); return -ENOMEM; }
            for (int i = 0; i < argc; i++) {
                uint32_t str_addr = argv_array[i];
                if (!str_addr || str_addr >= 0xC0000000 || str_addr < 0x1000) {
                    kfree(argv); kfree(buf); return -EFAULT;
                }
                argv[i] = (char *)str_addr;
            }
        }
    }
    
    int envc = 0;
    char **envp = NULL;
    if (envp_addr) {
        uint32_t *envp_array = (uint32_t *)envp_addr;
        while (envp_array[envc] && envc < 256) envc++;
        if (envc > 0) {
            envp = (char **)kmalloc(envc * sizeof(char *));
            if (!envp) { if (argv) kfree(argv); kfree(buf); return -ENOMEM; }
            for (int i = 0; i < envc; i++) {
                uint32_t str_addr = envp_array[i];
                if (!str_addr || str_addr >= 0xC0000000 || str_addr < 0x1000) {
                    kfree(envp); if (argv) kfree(argv); kfree(buf); return -EFAULT;
                }
                envp[i] = (char *)str_addr;
            }
        }
    }
    
    if (!fork_regs_ptr) { if (envp) kfree(envp); if (argv) kfree(argv); kfree(buf); return -EFAULT; }
    
    int result = task_exec_with_args(buf, size, fork_regs_ptr, argc, argv, envc, envp);
    
    kfree(buf);
    if (envp) kfree(envp);
    if (argv) kfree(argv);
    return result;
}

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

static int sys_fcntl(int fd, const char *cmd_ptr, int arg) {
    int cmd = (int)(uintptr_t)cmd_ptr;
    if (fd < 0) return -1;
    switch (cmd) {
        case F_DUPFD:
            return sys_dup(fd, NULL, 0);
        case F_GETFD:
        case F_GETFL:
            return 0;
        case F_SETFD:
        case F_SETFL:
            return 0;
        default:
            return -1;
    }
    (void)arg;
}

static int sys_truncate(int path_ptr, const char *len_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    uint32_t length = (uint32_t)(uintptr_t)len_ptr;
    
    if (!path_addr || path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    
    const char *path = (const char *)path_addr;
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -1;
    
    if (node->flags & VFS_DIRECTORY) return -1;
    
    if (node->truncate) {
        return node->truncate(node, length);
    }
    
    return -1;
}

static int sys_ftruncate(int fd, const char *len_ptr, int unused) {
    (void)unused;
    uint32_t length = (uint32_t)(uintptr_t)len_ptr;
    
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    
    if (node->flags & VFS_DIRECTORY) return -1;
    
    if (node->truncate) {
        return node->truncate(node, length);
    }
    
    return -1;
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
    char d_name[];
};

static int sys_getdents(int fd, const char *dirp_ptr, int count) {
    uint32_t dirp_addr = (uint32_t)(uintptr_t)dirp_ptr;
    if (!dirp_addr || dirp_addr >= 0xC0000000 || dirp_addr < 0x1000) return -1;
    if (count <= 0) return -1;
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    if (!(node->flags & VFS_DIRECTORY)) return -1;
    
    uint8_t *buf = (uint8_t *)dirp_addr;
    int written = 0;
    uint32_t dir_offset = fd_table[fd].offset;
    
    while (written < count) {
        dirent_t *entry = vfs_readdir(node, dir_offset);
        if (!entry) break;
        
        int name_len = 0;
        while (entry->name[name_len] && name_len < 63) name_len++;
        int reclen = sizeof(unsigned long) * 2 + sizeof(unsigned short) + name_len + 1 + 1;
        reclen = (reclen + 3) & ~3;
        
        if (written + reclen > count) break;
        
        struct linux_dirent *de = (struct linux_dirent *)(buf + written);
        de->d_ino = entry->inode ? entry->inode : dir_offset + 1;
        de->d_off = dir_offset + 1;
        de->d_reclen = reclen;
        
        for (int i = 0; i < name_len; i++) {
            de->d_name[i] = entry->name[i];
        }
        de->d_name[name_len] = '\0';
        
        uint8_t d_type = 0;
        if (entry->type == VFS_DIRECTORY) d_type = 4;
        else if (entry->type == VFS_FILE) d_type = 8;
        else if (entry->type == VFS_SYMLINK) d_type = 10;
        else if (entry->type == VFS_CHARDEVICE) d_type = 2;
        else if (entry->type == VFS_BLOCKDEVICE) d_type = 6;
        else if (entry->type == VFS_PIPE) d_type = 1;
        buf[written + reclen - 1] = d_type;
        
        written += reclen;
        dir_offset++;
    }
    
    fd_table[fd].offset = dir_offset;
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
    (void)path_ptr; (void)buf_ptr; (void)bufsiz;
    return -1;
}

static int sys_dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    if (oldfd < 0 || oldfd >= MAX_FDS || !fd_table[oldfd].in_use) return -EBADF;
    if (newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return -EINVAL;
    if (fd_table[newfd].in_use) {
        if (fd_table[newfd].private_data && (fd_table[newfd].type == FD_TYPE_PIPE_R || fd_table[newfd].type == FD_TYPE_PIPE_W)) {
            pipe_t *p = (pipe_t *)fd_table[newfd].private_data;
            if (fd_table[newfd].type == FD_TYPE_PIPE_R) p->readers--;
            else p->writers--;
            if (p->readers == 0 && p->writers == 0) kfree(p);
        }
        fd_table[newfd].in_use = 0;
    }
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(fd_entry_t));
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
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -EBADF;
    if (!(node->flags & VFS_DIRECTORY)) return -ENOTDIR;
    if (!current_task) return -ESRCH;
    char *resolved = vfs_get_path(node, current_task->cwd, sizeof(current_task->cwd));
    (void)resolved;
    return 0;
}

static int sys_fchmod(int fd, int mode) {
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -EBADF;
    if (node->chmod) {
        return node->chmod(node, mode & 07777);
    }
    node->mask = mode & 07777;
    return 0;
}

static int sys_fchown(int fd, int uid, int gid) {
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -EBADF;
    if (node->chown) {
        return node->chown(node, uid, gid);
    }
    if (uid != -1) node->uid = uid;
    if (gid != -1) node->gid = gid;
    return 0;
}

static int sys_fsync(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    return 0;
}

static int sys_fdatasync(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    return 0;
}

static int sys_flock(int fd, int operation) {
    (void)operation;
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    return 0;
}

static int sys_pread64(int fd, void *buf, size_t count, long long offset) {
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    if (!buf) return -EFAULT;
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -EBADF;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;
    return vfs_read(node, (uint32_t)offset, count, buf);
}

static int sys_pwrite64(int fd, const void *buf, size_t count, long long offset) {
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    if (!buf) return -EFAULT;
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -EBADF;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;
    return vfs_write(node, (uint32_t)offset, count, (uint8_t *)buf);
}

struct iovec {
    void *iov_base;
    size_t iov_len;
};

static int sys_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
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
    uint32_t dirp_addr = (uint32_t)dirp;
    if (!dirp_addr || dirp_addr >= 0xC0000000 || dirp_addr < 0x1000) return -EFAULT;
    if (count == 0) return -EINVAL;
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -EBADF;
    if (!(node->flags & VFS_DIRECTORY)) return -ENOTDIR;
    
    uint8_t *buf = (uint8_t *)dirp;
    int written = 0;
    uint32_t dir_offset = fd_table[fd].offset;
    
    while ((unsigned int)written < count) {
        dirent_t *entry = vfs_readdir(node, dir_offset);
        if (!entry) break;
        
        int name_len = 0;
        while (entry->name[name_len] && name_len < 63) name_len++;
        int reclen = sizeof(unsigned long long) + sizeof(long long) + sizeof(unsigned short) + sizeof(unsigned char) + name_len + 1;
        reclen = (reclen + 7) & ~7;
        
        if ((unsigned int)(written + reclen) > count) break;
        
        struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + written);
        de->d_ino = entry->inode ? entry->inode : dir_offset + 1;
        de->d_off = dir_offset + 1;
        de->d_reclen = reclen;
        
        if (entry->type == VFS_DIRECTORY) de->d_type = 4;
        else if (entry->type == VFS_FILE) de->d_type = 8;
        else if (entry->type == VFS_SYMLINK) de->d_type = 10;
        else if (entry->type == VFS_CHARDEVICE) de->d_type = 2;
        else if (entry->type == VFS_BLOCKDEVICE) de->d_type = 6;
        else if (entry->type == VFS_PIPE) de->d_type = 1;
        else de->d_type = 0;
        
        for (int i = 0; i < name_len; i++) {
            de->d_name[i] = entry->name[i];
        }
        de->d_name[name_len] = '\0';
        
        written += reclen;
        dir_offset++;
    }
    
    fd_table[fd].offset = dir_offset;
    return written;
}

void syscalls_posix_init(void) {
    fd_table_init();
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

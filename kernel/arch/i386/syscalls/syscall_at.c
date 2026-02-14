#include "syscall_defs.h"

#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200
#define AT_EMPTY_PATH       0x1000

static int task_fd_alloc_from(int start) {
    if (!current_task) return -ESRCH;
    if (start < 0) start = 0;
    for (int i = start; i < TASK_MAX_FDS; i++) {
        if (!current_task->fds[i].in_use) {
            current_task->fds[i].in_use = 1;
            current_task->fds[i].ref_count = 1;
            current_task->fds[i].type = FD_TYPE_FILE;
            current_task->fds[i].node = NULL;
            current_task->fds[i].offset = 0;
            current_task->fds[i].flags = 0;
            current_task->fds[i].private_data = NULL;
            return i;
        }
    }
    return -EMFILE;
}

static const char *resolve_at_path(int dirfd, const char *pathname, char *resolved, size_t size) {
    if (!pathname) return NULL;
    
    uint32_t path_addr = (uint32_t)(uintptr_t)pathname;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return NULL;
    
    if (pathname[0] == '/') {
        return pathname;
    }
    
    if (dirfd == AT_FDCWD) {
        const char *cwd = current_task ? current_task->cwd : "/";
        if (!cwd[0]) cwd = "/";
        
        size_t cwd_len = 0;
        while (cwd[cwd_len]) cwd_len++;
        
        size_t path_len = 0;
        while (pathname[path_len]) path_len++;
        
        if (cwd_len + 1 + path_len + 1 > size) return NULL;
        
        size_t pos = 0;
        for (size_t i = 0; i < cwd_len && pos < size - 1; i++) {
            resolved[pos++] = cwd[i];
        }
        if (pos > 0 && resolved[pos - 1] != '/' && pos < size - 1) {
            resolved[pos++] = '/';
        }
        for (size_t i = 0; i < path_len && pos < size - 1; i++) {
            resolved[pos++] = pathname[i];
        }
        resolved[pos] = '\0';
        return resolved;
    }
    
    return pathname;
}

static int sys_openat(int dirfd, const char *pathname, int flags) {
    char resolved[256];
    const char *path;
    vfs_node_t *node;
    int fd;
    char parent_path[256];
    char filename[64];
    int len;
    int last_slash;
    int i;
    int j;
    int ret;
    vfs_node_t *parent;

    path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;

    if (!current_task) return -ESRCH;

    node = vfs_namei(path);

    if (!node && (flags & VFS_O_CREAT)) {
        len = 0;
        while (path[len]) len++;

        last_slash = -1;
        for (i = 0; i < len; i++) {
            if (path[i] == '/') last_slash = i;
        }

        if (last_slash < 0) {
            parent_path[0] = '/';
            parent_path[1] = '\0';
            for (i = 0; i < len && i < 63; i++) filename[i] = path[i];
            filename[len < 63 ? len : 63] = '\0';
        } else if (last_slash == 0) {
            parent_path[0] = '/';
            parent_path[1] = '\0';
            j = 0;
            for (i = 1; i < len && j < 63; i++, j++) filename[j] = path[i];
            filename[j] = '\0';
        } else {
            for (i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
            parent_path[last_slash < 255 ? last_slash : 255] = '\0';
            j = 0;
            for (i = last_slash + 1; i < len && j < 63; i++, j++) filename[j] = path[i];
            filename[j] = '\0';
        }

        parent = vfs_namei(parent_path);
        if (!parent) return -ENOENT;

        ret = vfs_create(parent, filename, VFS_FILE);
        if (ret < 0 && !(flags & VFS_O_EXCL)) {
            node = vfs_namei(path);
        } else if (ret == 0) {
            node = vfs_namei(path);
        }
    }

    if (!node) return -ENOENT;

    if ((flags & VFS_O_TRUNC) && node->truncate) {
        node->truncate(node, 0);
    }

    if ((flags & 0200000) && VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        return -ENOTDIR;
    }

    fd = task_fd_alloc_from(0);
    if (fd < 0) return fd;

    vfs_open(node, (uint32_t)flags);
    current_task->fds[fd].type = FD_TYPE_FILE;
    current_task->fds[fd].node = node;
    current_task->fds[fd].offset = (flags & VFS_O_APPEND) ? node->length : 0;
    current_task->fds[fd].flags = (uint32_t)flags;
    return fd;
}

static int sys_mkdirat(int dirfd, const char *pathname, int mode) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    char parent_path[256];
    char dirname[64];
    
    int len = 0;
    while (path[len]) len++;
    
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash < 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        for (int i = 0; i < len && i < 63; i++) dirname[i] = path[i];
        dirname[len < 63 ? len : 63] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        int j = 0;
        for (int i = 1; i < len && j < 63; i++, j++) dirname[j] = path[i];
        dirname[j] = '\0';
    } else {
        for (int i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
        parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        int j = 0;
        for (int i = last_slash + 1; i < len && j < 63; i++, j++) dirname[j] = path[i];
        dirname[j] = '\0';
    }
    
    vfs_node_t *parent = vfs_namei(parent_path);
    if (!parent) return -ENOENT;
    
    return vfs_mkdir(parent, dirname, (uint32_t)mode);
}

static int sys_mknodat(int dirfd, const char *pathname, int mode) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    (void)mode;
    return -ENOSYS;
}

static int sys_fchownat(int dirfd, const char *pathname, int owner) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    
    (void)owner;
    return 0;
}

static int sys_unlinkat(int dirfd, const char *pathname, int flags) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    char parent_path[256];
    char filename[64];
    
    int len = 0;
    while (path[len]) len++;
    
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash < 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        for (int i = 0; i < len && i < 63; i++) filename[i] = path[i];
        filename[len < 63 ? len : 63] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        int j = 0;
        for (int i = 1; i < len && j < 63; i++, j++) filename[j] = path[i];
        filename[j] = '\0';
    } else {
        for (int i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
        parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        int j = 0;
        for (int i = last_slash + 1; i < len && j < 63; i++, j++) filename[j] = path[i];
        filename[j] = '\0';
    }
    
    vfs_node_t *parent = vfs_namei(parent_path);
    if (!parent) return -ENOENT;
    
    (void)flags;
    return vfs_unlink(parent, filename);
}

static int sys_renameat(int olddirfd, const char *oldpath, int newdirfd) {
    (void)olddirfd; (void)oldpath; (void)newdirfd;
    return -ENOSYS;
}

static int sys_linkat(int olddirfd, const char *oldpath, int newdirfd) {
    (void)olddirfd; (void)oldpath; (void)newdirfd;
    return -ENOSYS;
}

static int sys_symlinkat(int target_ptr, const char *newdirfd_ptr, int linkpath) {
    (void)target_ptr; (void)newdirfd_ptr; (void)linkpath;
    return -ENOSYS;
}

static int sys_readlinkat(int dirfd, const char *pathname, int buf_ptr) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;

    uint32_t buf_addr = (uint32_t)buf_ptr;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;

    vfs_node_t *node = vfs_namei_nofollow(path);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) return -EINVAL;

    char target[VFS_MAX_PATH];
    uint32_t n = vfs_read(node, 0, sizeof(target) - 1, (uint8_t *)target);
    if (n >= sizeof(target)) n = sizeof(target) - 1;
    target[n] = '\0';

    for (uint32_t i = 0; i < n; i++) {
        ((char *)buf_addr)[i] = target[i];
    }
    return (int)n;
}

static int sys_fchmodat(int dirfd, const char *pathname, int mode) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    
    node->mask = (uint32_t)mode & 0777;
    return 0;
}

static int sys_faccessat(int dirfd, const char *pathname, int mode) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    
    (void)mode;
    return 0;
}

static int sys_fstatat(int dirfd, const char *pathname, int statbuf) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    uint32_t buf_addr = (uint32_t)statbuf;
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    
    struct kernel_stat *st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    
    st->st_dev = 1;
    st->st_ino = node->inode ? node->inode : 1;
    st->__st_ino_truncated = (long)st->st_ino;

    uint32_t perms = 0;
    if (node->mask) {
        if ((node->mask & ~0x07u) == 0) {
            if (node->mask & VFS_PERM_READ) perms |= 0444;
            if (node->mask & VFS_PERM_WRITE) perms |= 0222;
            if (node->mask & VFS_PERM_EXEC) perms |= 0111;
        } else {
            perms = node->mask & 07777u;
        }
    }

    uint32_t mode;
    switch (VFS_GET_TYPE(node->flags)) {
        case VFS_DIRECTORY:   mode = S_IFDIR | (perms ? perms : 0755); break;
        case VFS_SYMLINK:     mode = S_IFLNK | (perms ? perms : 0777); break;
        case VFS_CHARDEVICE:  mode = S_IFCHR | (perms ? perms : 0660); break;
        case VFS_BLOCKDEVICE: mode = S_IFBLK | (perms ? perms : 0660); break;
        case VFS_PIPE:        mode = S_IFIFO | (perms ? perms : 0644); break;
        default:              mode = S_IFREG | (perms ? perms : 0644); break;
    }
    
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_rdev = 0;
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

static int sys_utimensat(int dirfd, const char *pathname, int times_ptr) {
    (void)dirfd; (void)pathname; (void)times_ptr;
    return 0;
}

static int sys_renameat2(int olddirfd, const char *oldpath, int newdirfd) {
    return sys_renameat(olddirfd, oldpath, newdirfd);
}

void syscalls_at_init(void) {
    syscall_table[SYSCALL_OPENAT] = sys_openat;
    syscall_table[SYSCALL_MKDIRAT] = sys_mkdirat;
    syscall_table[SYSCALL_MKNODAT] = sys_mknodat;
    syscall_table[SYSCALL_FCHOWNAT] = sys_fchownat;
    syscall_table[SYSCALL_UNLINKAT] = sys_unlinkat;
    syscall_table[SYSCALL_RENAMEAT] = sys_renameat;
    syscall_table[SYSCALL_LINKAT] = sys_linkat;
    syscall_table[SYSCALL_SYMLINKAT] = sys_symlinkat;
    syscall_table[SYSCALL_READLINKAT] = sys_readlinkat;
    syscall_table[SYSCALL_FCHMODAT] = sys_fchmodat;
    syscall_table[SYSCALL_FACCESSAT] = sys_faccessat;
    syscall_table[SYSCALL_FSTATAT] = sys_fstatat;
    syscall_table[SYSCALL_UTIMENSAT] = sys_utimensat;
    syscall_table[SYSCALL_RENAMEAT2] = sys_renameat2;
}

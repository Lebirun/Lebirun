#include "syscall_defs.h"
#include <lebirun/ramfs.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/timekeeping.h>

#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200
#define AT_EMPTY_PATH       0x1000
#define RENAME_NOREPLACE    1
#define RENAME_EXCHANGE     2
#define UTIME_NOW  1073741823L
#define UTIME_OMIT 1073741822L

typedef struct {
    long tv_sec;
    long tv_nsec;
} at_timespec_t;

static int task_fd_alloc_from(int start) {
    int i;
    int ret;

    if (!current_task || !current_task->fds) return -ESRCH;
    if (start < 0) start = 0;
    for (i = start; i < current_task->fds_capacity; i++) {
        if (!current_task->fds[i].in_use) {
            memset(&current_task->fds[i], 0, sizeof(task_fd_t));
            current_task->fds[i].in_use = 1;
            current_task->fds[i].ref_count = 1;
            current_task->fds[i].type = FD_TYPE_FILE;
            return i;
        }
    }
    ret = task_fd_ensure_capacity(
        current_task, start >= current_task->fds_capacity ?
        start : current_task->fds_capacity);
    if (ret != 0) return -EMFILE;
    for (i = start; i < current_task->fds_capacity; i++) {
        if (!current_task->fds[i].in_use) break;
    }
    if (i >= current_task->fds_capacity) return -EMFILE;
    memset(&current_task->fds[i], 0, sizeof(task_fd_t));
    current_task->fds[i].in_use = 1;
    current_task->fds[i].ref_count = 1;
    current_task->fds[i].type = FD_TYPE_FILE;
    return i;
}

static char *resolve_at_path_alloc(int dirfd, const char *pathname) {
    char *input;
    char *base_alloc;
    char *result;
    const char *base;
    task_fd_t *tfd;
    vfs_node_t *dir_node;
    size_t base_length;
    size_t input_length;
    size_t position;

    input = copy_string_from_user_alloc(pathname);
    if (!input) return NULL;
    if (input[0] == '/') return input;
    base_alloc = NULL;
    if (dirfd == AT_FDCWD) {
        base = current_task && current_task->cwd && current_task->cwd[0] ?
               current_task->cwd : "/";
    } else {
        if (!current_task || dirfd < 0 ||
            dirfd >= current_task->fds_capacity ||
            !current_task->fds[dirfd].in_use) {
            kfree(input);
            return NULL;
        }
        tfd = &current_task->fds[dirfd];
        if (!tfd->node) {
            kfree(input);
            return NULL;
        }
        dir_node = (vfs_node_t *)tfd->node;
        base_alloc = vfs_get_path_alloc(dir_node);
        if (!base_alloc) {
            kfree(input);
            return NULL;
        }
        base = base_alloc;
    }
    base_length = strlen(base);
    input_length = strlen(input);
    if (input_length > SIZE_MAX - 2 ||
        base_length > SIZE_MAX - input_length - 2) {
        if (base_alloc) kfree(base_alloc);
        kfree(input);
        return NULL;
    }
    result = (char *)kmalloc(base_length + input_length + 2);
    if (!result) {
        if (base_alloc) kfree(base_alloc);
        kfree(input);
        return NULL;
    }
    memcpy(result, base, base_length);
    position = base_length;
    if (position == 0 || result[position - 1] != '/') result[position++] = '/';
    memcpy(result + position, input, input_length + 1);
    if (base_alloc) kfree(base_alloc);
    kfree(input);
    return result;
}

static int split_at_path_alloc(const char *path, char **parent_out,
                               char **name_out) {
    const char *slash;
    const char *name_start;
    size_t parent_length;
    size_t name_length;
    char *parent;
    char *name;

    if (!path || !parent_out || !name_out) return -EINVAL;
    *parent_out = NULL;
    *name_out = NULL;
    slash = strrchr(path, '/');
    if (!slash || slash == path) {
        parent_length = 1;
        name_start = slash ? slash + 1 : path;
    } else {
        parent_length = (size_t)(slash - path);
        name_start = slash + 1;
    }
    name_length = strlen(name_start);
    if (name_length == 0 || parent_length > SIZE_MAX - 1 ||
        name_length > SIZE_MAX - 1)
        return -EINVAL;
    parent = (char *)kmalloc(parent_length + 1);
    if (!parent) return -ENOMEM;
    name = (char *)kmalloc(name_length + 1);
    if (!name) {
        kfree(parent);
        return -ENOMEM;
    }
    if (!slash || slash == path) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        memcpy(parent, path, parent_length);
        parent[parent_length] = '\0';
    }
    memcpy(name, name_start, name_length + 1);
    *parent_out = parent;
    *name_out = name;
    return 0;
}

static int sys_openat(int dirfd, const char *pathname, int flags, int mode) {
    char *path;
    vfs_node_t *node;
    int fd;
    uint64_t create_mode;
    char *parent_path;
    char *filename;
    int ret;
    int want;
    int perm_ret;
    int pipe_type;
    uint64_t pipe_flags;
    uint64_t mount_flags;
    pipe_t *pipe;
    vfs_node_t *parent;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;

    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }

    if (pty_path_supported(path)) {
        fd = pty_open_path(path, flags);
        kfree(path);
        return fd < 0 ? -ENODEV : fd;
    }

    node = vfs_namei(path);

    if (node && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
        vfs_release(node);
        kfree(path);
        return -EEXIST;
    }

    if (!node && (flags & VFS_O_CREAT)) {
        create_mode = (uint64_t)(mode & 0777);
        create_mode &= ~current_task->creation_mask;
        if (flags & VFS_O_EXCL) create_mode |= VFS_O_EXCL;

        ret = split_at_path_alloc(path, &parent_path, &filename);
        if (ret != 0) {
            kfree(path);
            return ret;
        }

        parent = vfs_namei(parent_path);
        kfree(parent_path);
        if (!parent) {
            kfree(filename);
            kfree(path);
            return -ENOENT;
        }

        if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
            vfs_release(parent);
            kfree(filename);
            kfree(path);
            return -EROFS;
        }

        ret = vfs_create(parent, filename, create_mode);
        vfs_release(parent);
        kfree(filename);
        if (ret < 0 && !(flags & VFS_O_EXCL)) {
            node = vfs_namei(path);
        } else if (ret == 0) {
            node = vfs_namei(path);
        }
    }

    kfree(path);
    if (!node) return -ENOENT;

    mount_flags = vfs_get_mount_flags_for_node(node);
    if (((flags & VFS_O_WRONLY) || (flags & VFS_O_RDWR) ||
         (flags & VFS_O_TRUNC)) &&
        (mount_flags & VFS_MS_RDONLY)) {
        vfs_release(node);
        return -EROFS;
    }
    if ((VFS_GET_TYPE(node->flags) == VFS_CHARDEVICE ||
         VFS_GET_TYPE(node->flags) == VFS_BLOCKDEVICE) &&
        (mount_flags & VFS_MS_NODEV)) {
        vfs_release(node);
        return -EACCES;
    }
    want = VFS_PERM_READ;
    if ((flags & VFS_O_WRONLY) || (flags & VFS_O_RDWR) ||
        (flags & VFS_O_TRUNC)) want |= VFS_PERM_WRITE;
    perm_ret = vfs_check_perm(node, want);
    if (perm_ret < 0) {
        vfs_release(node);
        return perm_ret;
    }

    if (VFS_GET_TYPE(node->flags) == VFS_PIPE) {
        if ((flags & 0x3) == VFS_O_WRONLY) pipe_type = FD_TYPE_PIPE_W;
        else if ((flags & 0x3) == VFS_O_RDWR) pipe_type = FD_TYPE_PIPE_RW;
        else pipe_type = FD_TYPE_PIPE_R;
        pipe = pipe_named_open(node, pipe_type);
        if (!pipe) {
            vfs_release(node);
            return -ENOMEM;
        }
        pipe_flags = pipe_lock_irqsave(pipe);
        if (pipe_type == FD_TYPE_PIPE_W && pipe->readers == 0 &&
            (flags & VFS_O_NONBLOCK)) {
            pipe_unlock_irqrestore(pipe, pipe_flags);
            pipe_release_reference(pipe, pipe_type);
            pipe_destroy_if_unused(pipe);
            vfs_release(node);
            return -ENXIO;
        }
        while (!(flags & VFS_O_NONBLOCK) &&
               ((pipe_type == FD_TYPE_PIPE_R && pipe->writers == 0) ||
                (pipe_type == FD_TYPE_PIPE_W && pipe->readers == 0))) {
            if (pipe_type == FD_TYPE_PIPE_R)
                waitq_add(&pipe->read_waitq, current_task);
            else
                waitq_add(&pipe->write_waitq, current_task);
            current_task->state = TASK_BLOCKED;
            pipe_unlock_irqrestore(pipe, pipe_flags);
            schedule();
            if (task_has_pending_signals()) {
                pipe_release_reference(pipe, pipe_type);
                pipe_destroy_if_unused(pipe);
                vfs_release(node);
                return -EINTR;
            }
            pipe_flags = pipe_lock_irqsave(pipe);
        }
        pipe_unlock_irqrestore(pipe, pipe_flags);
        fd = task_fd_alloc_from(0);
        if (fd < 0) {
            pipe_release_reference(pipe, pipe_type);
            pipe_destroy_if_unused(pipe);
            vfs_release(node);
            return fd;
        }
        current_task->fds[fd].type = pipe_type;
        current_task->fds[fd].node = node;
        current_task->fds[fd].private_data = pipe;
        current_task->fds[fd].flags = (uint64_t)flags;
        vfs_release(node);
        return fd;
    }

    if ((flags & VFS_O_TRUNC) && node->truncate) {
        node->truncate(node, 0);
    }

    if ((flags & 0200000) && VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        vfs_release(node);
        return -ENOTDIR;
    }

    fd = task_fd_alloc_from(0);
    if (fd < 0) { vfs_release(node); return fd; }

    vfs_open(node, (uint64_t)flags);
    current_task->fds[fd].type = FD_TYPE_FILE;
    current_task->fds[fd].node = node;
    current_task->fds[fd].offset = (flags & VFS_O_APPEND) ? node->length : 0;
    current_task->fds[fd].flags = (uint64_t)flags;
    return fd;
}

static int sys_mkdirat(int dirfd, const char *pathname, int mode) {
    char *path;
    char *parent_path;
    char *dirname;
    vfs_node_t *parent;
    int result;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }
    result = split_at_path_alloc(path, &parent_path, &dirname);
    kfree(path);
    if (result != 0) return result;
    parent = vfs_namei(parent_path);
    kfree(parent_path);
    if (!parent) {
        kfree(dirname);
        return -ENOENT;
    }
    result = vfs_mkdir(parent, dirname,
                       (uint64_t)mode & ~current_task->creation_mask);
    vfs_release(parent);
    kfree(dirname);
    return result;
}

static int sys_mknodat(int dirfd, const char *pathname, int mode,
                       uint64_t device) {
    char *parent_path;
    char *name;
    char *path;
    vfs_node_t *parent;
    vfs_node_t *existing;
    int type;
    int result;

    (void)device;
    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    existing = vfs_namei(path);
    if (existing) {
        vfs_release(existing);
        kfree(path);
        return -EEXIST;
    }
    result = split_at_path_alloc(path, &parent_path, &name);
    kfree(path);
    if (result != 0) return result;
    parent = vfs_namei(parent_path);
    kfree(parent_path);
    if (!parent) {
        kfree(name);
        return -ENOENT;
    }
    if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
        vfs_release(parent);
        kfree(name);
        return -EROFS;
    }
    type = mode & S_IFMT;
    if (type == 0 || type == S_IFREG) {
        result = vfs_create(parent, name,
                            ((uint64_t)mode & 07777) &
                            ~current_task->creation_mask);
    } else if (type == S_IFIFO) {
        result = vfs_mknod(parent, name,
                           S_IFIFO | (((uint64_t)mode & 07777) &
                           ~current_task->creation_mask));
    } else if (type == S_IFCHR || type == S_IFBLK) {
        vfs_release(parent);
        kfree(name);
        return -EPERM;
    } else {
        vfs_release(parent);
        kfree(name);
        return -EINVAL;
    }
    vfs_release(parent);
    kfree(name);
    return result == 0 ? 0 : (result < -11 ? result : -EIO);
}

static int sys_fchownat(int dirfd, const char *pathname, int owner) {
    char *path;
    vfs_node_t *node;
    int result;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }

    if (current_task->euid != 0) {
        kfree(path);
        return -EPERM;
    }

    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) {
        vfs_release(node);
        return -EROFS;
    }

    if (node->chown) {
        result = node->chown(node, (uint64_t)owner, node->gid);
        vfs_release(node);
        return result;
    }
    if (owner != -1) node->uid = (uint64_t)owner;
    vfs_release(node);
    return 0;
}

static int sys_unlinkat(int dirfd, const char *pathname, int flags) {
    char *path;
    char *parent_path;
    char *filename;
    int r;
    uint64_t pmode;
    int pshift;
    int pallowed;
    vfs_node_t *parent;

    if (flags & ~AT_REMOVEDIR) return -EINVAL;
    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }
    r = split_at_path_alloc(path, &parent_path, &filename);
    kfree(path);
    if (r != 0) return r;

    parent = vfs_namei(parent_path);
    kfree(parent_path);
    if (!parent) {
        kfree(filename);
        return -ENOENT;
    }

    if (current_task->euid != 0) {
        pmode = parent->mask;
        if (current_task->euid == parent->uid)
            pshift = 6;
        else if (current_task->egid == parent->gid)
            pshift = 3;
        else
            pshift = 0;
        pallowed = (int)((pmode >> pshift) & 7);
        if (!(pallowed & VFS_PERM_WRITE)) {
            vfs_release(parent);
            kfree(filename);
            return -EACCES;
        }
    }

    r = vfs_unlink_checked(parent, filename, (flags & AT_REMOVEDIR) != 0);
    vfs_release(parent);
    kfree(filename);
    return r;
}

static int sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath_arg) {
    char *old_path;
    char *new_path;
    char *old_parent_path;
    char *new_parent_path;
    char *old_name;
    char *new_name;
    vfs_node_t *old_node;
    vfs_node_t *old_parent;
    vfs_node_t *new_parent;
    int r;

    if (!oldpath || !newpath_arg) return -EFAULT;

    old_path = resolve_at_path_alloc(olddirfd, oldpath);
    if (!old_path) return -EFAULT;

    new_path = resolve_at_path_alloc(newdirfd, newpath_arg);
    if (!new_path) {
        kfree(old_path);
        return -EFAULT;
    }
    if (strcmp(old_path, new_path) == 0) {
        kfree(old_path);
        kfree(new_path);
        return 0;
    }

    old_node = vfs_namei(old_path);
    if (!old_node) {
        kfree(old_path);
        kfree(new_path);
        return -ENOENT;
    }

    old_parent = old_node->parent;
    if (!old_parent || !old_parent->rename) {
        r = ramfs_rename(old_path, new_path) ? -ENOENT : 0;
        vfs_release(old_node);
        kfree(old_path);
        kfree(new_path);
        return r;
    }

    r = split_at_path_alloc(old_path, &old_parent_path, &old_name);
    if (r != 0) {
        vfs_release(old_node);
        kfree(old_path);
        kfree(new_path);
        return r;
    }
    kfree(old_parent_path);
    r = split_at_path_alloc(new_path, &new_parent_path, &new_name);
    if (r != 0) {
        vfs_release(old_node);
        kfree(old_name);
        kfree(old_path);
        kfree(new_path);
        return r;
    }

    new_parent = vfs_namei(new_parent_path);
    kfree(new_parent_path);
    if (!new_parent) {
        vfs_release(old_node);
        kfree(old_name);
        kfree(new_name);
        kfree(old_path);
        kfree(new_path);
        return -ENOENT;
    }
    if ((vfs_get_mount_flags_for_node(old_parent) & VFS_MS_RDONLY) ||
        (vfs_get_mount_flags_for_node(new_parent) & VFS_MS_RDONLY)) {
        vfs_release(new_parent);
        vfs_release(old_node);
        kfree(old_name);
        kfree(new_name);
        kfree(old_path);
        kfree(new_path);
        return -EROFS;
    }

    r = old_parent->rename(old_parent, old_name, new_parent, new_name);
    vfs_release(old_node);
    vfs_release(new_parent);
    kfree(old_name);
    kfree(new_name);
    kfree(old_path);
    kfree(new_path);
    return r;
}

static int sys_linkat(int olddirfd, const char *oldpath, int newdirfd,
                      const char *newpath, int flags) {
    char *parent_path;
    char *name;
    char *old_path;
    char *new_path;
    vfs_node_t *old_node;
    vfs_node_t *parent;
    int result;

    if (!oldpath || !newpath) return -EFAULT;
    if (flags & ~AT_SYMLINK_FOLLOW) return -EINVAL;
    old_path = resolve_at_path_alloc(olddirfd, oldpath);
    if (!old_path) return -EFAULT;
    new_path = resolve_at_path_alloc(newdirfd, newpath);
    if (!new_path) {
        kfree(old_path);
        return -EFAULT;
    }
    result = split_at_path_alloc(new_path, &parent_path, &name);
    if (result != 0) {
        kfree(old_path);
        kfree(new_path);
        return result;
    }
    parent = vfs_namei(parent_path);
    kfree(parent_path);
    if (!parent) {
        kfree(name);
        kfree(old_path);
        kfree(new_path);
        return -ENOENT;
    }
    if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
        vfs_release(parent);
        kfree(name);
        kfree(old_path);
        kfree(new_path);
        return -EROFS;
    }
    old_node = vfs_namei(old_path);
    if (!old_node) {
        vfs_release(parent);
        kfree(name);
        kfree(old_path);
        kfree(new_path);
        return -ENOENT;
    }
    result = ramfs_link_node(old_node, parent, name);
    vfs_release(old_node);
    vfs_release(parent);
    if (result == RAMFS_ERR_OK) result = 0;
    else if (result == RAMFS_ERR_EXIST) result = -EEXIST;
    else if (result == RAMFS_ERR_NOSPC) result = -ENOSPC;
    else if (result == RAMFS_ERR_NOMEM) result = -ENOMEM;
    else if (ext4_vfs_link_node(old_path, new_path) != 0) result = -EIO;
    else result = 0;
    kfree(name);
    kfree(old_path);
    kfree(new_path);
    return result;
}

static int sys_symlinkat(uint64_t target_ptr, const char *newdirfd_ptr,
                         uint64_t linkpath) {
    char *parent_path;
    char *name;
    char *link_path;
    char *target;
    vfs_node_t *parent;
    int newdirfd;
    int ret;

    newdirfd = (int)(uintptr_t)newdirfd_ptr;

    target = copy_string_from_user_alloc((const char *)(uintptr_t)target_ptr);
    if (!target) return -EFAULT;

    link_path = resolve_at_path_alloc(newdirfd,
                                      (const char *)(uintptr_t)linkpath);
    if (!link_path) {
        kfree(target);
        return -EFAULT;
    }
    ret = split_at_path_alloc(link_path, &parent_path, &name);
    if (ret != 0) {
        kfree(link_path);
        kfree(target);
        return ret;
    }
    parent = vfs_namei(parent_path);
    kfree(parent_path);
    if (!parent) {
        kfree(name);
        kfree(link_path);
        kfree(target);
        return -ENOENT;
    }
    if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
        vfs_release(parent);
        kfree(name);
        kfree(link_path);
        kfree(target);
        return -EROFS;
    }
    ret = ext4_vfs_symlink_node(target, link_path, 0);
    if (ret == 0) {
        vfs_release(parent);
        kfree(name);
        kfree(link_path);
        kfree(target);
        return 0;
    }

    ret = ramfs_create_symlink_node(parent, name, target, 0777);
    vfs_release(parent);
    kfree(name);
    kfree(link_path);
    kfree(target);
    if (ret == 0) return 0;
    if (ret == RAMFS_ERR_EXIST) return -EEXIST;
    if (ret == RAMFS_ERR_NOENT) return -ENOENT;
    if (ret == RAMFS_ERR_NOSPC) return -ENOSPC;
    if (ret == RAMFS_ERR_NOMEM) return -ENOMEM;
    return -EIO;
}

static int sys_readlinkat(int dirfd, const char *pathname, uint64_t buf_ptr,
                          uint64_t buf_size) {
    char *path;
    uint64_t buf_addr;
    uint64_t n;
    vfs_node_t *node;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    buf_addr = (uint64_t)buf_ptr;
    if (buf_size == 0) {
        kfree(path);
        return 0;
    }
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000 ||
        !syscall_user_range_mapped(buf_addr, buf_size, 0)) {
        kfree(path);
        return -EFAULT;
    }
    node = vfs_namei_nofollow(path);
    kfree(path);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) {
        vfs_release(node);
        return -EINVAL;
    }

    n = node->length;
    if (n > buf_size) n = buf_size;
    n = vfs_read(node, 0, n, (uint8_t *)(uintptr_t)buf_addr);
    vfs_release(node);
    return (int)n;
}

static int sys_fchmodat(int dirfd, const char *pathname, int mode) {
    char *path;
    vfs_node_t *node;
    int result;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }

    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) {
        vfs_release(node);
        return -EROFS;
    }

    if (current_task->euid != 0 && current_task->euid != node->uid) {
        vfs_release(node);
        return -EPERM;
    }

    if (node->chmod) {
        result = node->chmod(node, (uint64_t)mode & 07777);
        vfs_release(node);
        return result;
    }
    node->mask = (uint64_t)mode & 07777;
    vfs_release(node);
    return 0;
}

static int sys_faccessat(int dirfd, const char *pathname, int mode) {
    char *path;
    vfs_node_t *node;
    uint64_t uid;
    uint64_t gid;
    uint64_t fmode;
    int shift;
    int allowed;
    int want;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;
    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }

    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;

    if (mode == 0) { vfs_release(node); return 0; }

    uid = current_task->euid;
    gid = current_task->egid;
    if (uid == 0) { vfs_release(node); return 0; }

    fmode = node->mask;
    if (uid == node->uid)
        shift = 6;
    else if (gid == node->gid)
        shift = 3;
    else
        shift = 0;

    allowed = (int)((fmode >> shift) & 7);
    want = 0;
    if (mode & 4) want |= VFS_PERM_READ;
    if (mode & 2) want |= VFS_PERM_WRITE;
    if (mode & 1) want |= VFS_PERM_EXEC;
    vfs_release(node);
    if ((allowed & want) == want) return 0;
    return -EACCES;
}

static int sys_fstatat(int dirfd, const char *pathname, uint64_t statbuf) {
    char *path;
    uint64_t buf_addr;
    vfs_node_t *node;
    struct kernel_stat *st;
    uint64_t perms;
    uint64_t mode;

    path = resolve_at_path_alloc(dirfd, pathname);
    if (!path) return -EFAULT;

    buf_addr = (uint64_t)statbuf;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) {
        kfree(path);
        return -EFAULT;
    }

    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;

    st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    
    st->st_dev = 1;
    st->st_ino = node->inode ? node->inode : 1;

    perms = 0;
    if (node->mask) {
        if ((node->mask & ~0x07u) == 0) {
            if (node->mask & VFS_PERM_READ) perms |= 0444;
            if (node->mask & VFS_PERM_WRITE) perms |= 0222;
            if (node->mask & VFS_PERM_EXEC) perms |= 0111;
        } else {
            perms = node->mask & 07777u;
        }
    }

    switch (VFS_GET_TYPE(node->flags)) {
        case VFS_DIRECTORY:   mode = S_IFDIR | (perms ? perms : 0755); break;
        case VFS_SYMLINK:     mode = S_IFLNK | (perms ? perms : 0777); break;
        case VFS_CHARDEVICE:  mode = S_IFCHR | (perms ? perms : 0660); break;
        case VFS_BLOCKDEVICE: mode = S_IFBLK | (perms ? perms : 0660); break;
        case VFS_PIPE:        mode = S_IFIFO | (perms ? perms : 0644); break;
        case VFS_SOCKET:      mode = S_IFSOCK | (perms ? perms : 0777); break;
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
    
    vfs_release(node);
    return 0;
}

static int sys_utimensat(int dirfd, const char *pathname,
                         const at_timespec_t *times, int flags) {
    char *path;
    vfs_node_t *node;
    task_fd_t *fd;
    at_timespec_t requested[2];
    uint64_t now_ns;
    uint64_t now;
    uint64_t atime;
    uint64_t mtime;
    int explicit_time;
    int have_requested;

    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (!pathname ||
        !syscall_user_range_mapped((uint64_t)(uintptr_t)pathname, 1, 0))
        return -EFAULT;
    node = NULL;
    if (pathname && pathname[0] == '\0' && (flags & AT_EMPTY_PATH)) {
        fd = task_fd_get(current_task, dirfd);
        if (!fd || !fd->in_use || !fd->node) return -EBADF;
        node = (vfs_node_t *)fd->node;
        vfs_open(node, 0);
    } else {
        path = resolve_at_path_alloc(dirfd, pathname);
        if (!path) return -EFAULT;
        if (flags & AT_SYMLINK_NOFOLLOW)
            node = vfs_namei_nofollow(path);
        else
            node = vfs_namei(path);
        kfree(path);
        if (!node) return -ENOENT;
    }
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) {
        vfs_release(node);
        return -EROFS;
    }
    have_requested = times != NULL;
    explicit_time = 0;
    if (have_requested) {
        if (copy_from_user(requested, times, sizeof(requested)) < 0) {
            vfs_release(node);
            return -EFAULT;
        }
        if ((requested[0].tv_nsec < 0 ||
             requested[0].tv_nsec >= 1000000000L) &&
            requested[0].tv_nsec != UTIME_NOW &&
            requested[0].tv_nsec != UTIME_OMIT) {
            vfs_release(node);
            return -EINVAL;
        }
        if ((requested[1].tv_nsec < 0 ||
             requested[1].tv_nsec >= 1000000000L) &&
            requested[1].tv_nsec != UTIME_NOW &&
            requested[1].tv_nsec != UTIME_OMIT) {
            vfs_release(node);
            return -EINVAL;
        }
        if ((requested[0].tv_nsec != UTIME_NOW &&
             requested[0].tv_nsec != UTIME_OMIT) ||
            (requested[1].tv_nsec != UTIME_NOW &&
             requested[1].tv_nsec != UTIME_OMIT))
            explicit_time = 1;
        if ((requested[0].tv_nsec != UTIME_NOW &&
             requested[0].tv_nsec != UTIME_OMIT &&
             requested[0].tv_sec < 0) ||
            (requested[1].tv_nsec != UTIME_NOW &&
             requested[1].tv_nsec != UTIME_OMIT &&
             requested[1].tv_sec < 0)) {
            vfs_release(node);
            return -EOVERFLOW;
        }
    }
    if (current_task && current_task->euid != 0 &&
        current_task->euid != node->uid) {
        if (explicit_time || vfs_check_perm(node, VFS_PERM_WRITE) != 0) {
            vfs_release(node);
            return explicit_time ? -EPERM : -EACCES;
        }
    }
    if (timekeeping_get_ns(0, &now_ns) != 0) now_ns = 0;
    now = now_ns / 1000000000ULL;
    atime = node->atime;
    mtime = node->mtime;
    if (!have_requested) {
        atime = now;
        mtime = now;
    } else {
        if (requested[0].tv_nsec == UTIME_NOW) atime = now;
        else if (requested[0].tv_nsec != UTIME_OMIT)
            atime = (uint64_t)requested[0].tv_sec;
        if (requested[1].tv_nsec == UTIME_NOW) mtime = now;
        else if (requested[1].tv_nsec != UTIME_OMIT)
            mtime = (uint64_t)requested[1].tv_sec;
    }
    if (vfs_set_times(node, atime, mtime, now) != 0) {
        vfs_release(node);
        return -EIO;
    }
    vfs_release(node);
    return 0;
}

static int sys_renameat2(int olddirfd, const char *oldpath, int newdirfd,
                         const char *newpath, int flags) {
    char *old_parent_path;
    char *new_parent_path;
    char *old_name;
    char *new_name;
    char *old_path;
    char *path;
    vfs_node_t *target;
    vfs_node_t *old_parent;
    vfs_node_t *new_parent;
    int result;

    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE))
        return -EINVAL;
    if (flags & RENAME_EXCHANGE) {
        old_path = resolve_at_path_alloc(olddirfd, oldpath);
        if (!old_path) return -EFAULT;
        path = resolve_at_path_alloc(newdirfd, newpath);
        if (!path) {
            kfree(old_path);
            return -EFAULT;
        }
        result = split_at_path_alloc(old_path, &old_parent_path, &old_name);
        if (result != 0) {
            kfree(old_path);
            kfree(path);
            return result;
        }
        result = split_at_path_alloc(path, &new_parent_path, &new_name);
        if (result != 0) {
            kfree(old_parent_path);
            kfree(old_name);
            kfree(old_path);
            kfree(path);
            return result;
        }
        target = vfs_namei(old_path);
        if (!target) {
            result = -ENOENT;
            goto exchange_free_names;
        }
        vfs_release(target);
        target = vfs_namei(path);
        if (!target) {
            result = -ENOENT;
            goto exchange_free_names;
        }
        vfs_release(target);
        old_parent = vfs_namei(old_parent_path);
        new_parent = vfs_namei(new_parent_path);
        if (!old_parent || !new_parent) {
            if (old_parent) vfs_release(old_parent);
            if (new_parent) vfs_release(new_parent);
            result = -ENOENT;
            goto exchange_free_names;
        }
        result = vfs_exchange(old_parent, old_name, new_parent, new_name);
        vfs_release(old_parent);
        vfs_release(new_parent);
        if (result == -1) result = -EXDEV;
        else if (result != 0) result = -EINVAL;
        else result = 0;
exchange_free_names:
        kfree(old_parent_path);
        kfree(new_parent_path);
        kfree(old_name);
        kfree(new_name);
        kfree(old_path);
        kfree(path);
        return result;
    }
    if (flags & RENAME_NOREPLACE) {
        path = resolve_at_path_alloc(newdirfd, newpath);
        if (!path) return -EFAULT;
        target = vfs_namei(path);
        kfree(path);
        if (target) {
            vfs_release(target);
            return -EEXIST;
        }
    }
    return sys_renameat(olddirfd, oldpath, newdirfd, newpath);
}

void syscalls_at_init(void) {
    syscall_table_set(SYSCALL_OPENAT, (void *)(sys_openat));
    syscall_table_set(SYSCALL_MKDIRAT, (void *)(sys_mkdirat));
    syscall_table_set(SYSCALL_MKNODAT, (void *)(sys_mknodat));
    syscall_table_set(SYSCALL_FCHOWNAT, (void *)(sys_fchownat));
    syscall_table_set(SYSCALL_UNLINKAT, (void *)(sys_unlinkat));
    syscall_table_set(SYSCALL_RENAMEAT, (void *)(sys_renameat));
    syscall_table_set(SYSCALL_LINKAT, (void *)(sys_linkat));
    syscall_table_set(SYSCALL_SYMLINKAT, (void *)(sys_symlinkat));
    syscall_table_set(SYSCALL_READLINKAT, (void *)(sys_readlinkat));
    syscall_table_set(SYSCALL_FCHMODAT, (void *)(sys_fchmodat));
    syscall_table_set(SYSCALL_FACCESSAT, (void *)(sys_faccessat));
    syscall_table_set(SYSCALL_FSTATAT, (void *)(sys_fstatat));
    syscall_table_set(SYSCALL_UTIMENSAT, (void *)(sys_utimensat));
    syscall_table_set(SYSCALL_RENAMEAT2, (void *)(sys_renameat2));
}

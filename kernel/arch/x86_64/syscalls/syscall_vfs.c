#include "syscall_defs.h"
#include <lebirun/creds.h>
#include <lebirun/ramfs.h>
#include <lebirun/squashfs.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <lebirun/pty.h>

extern int is_socket_fd(int fd);
extern int socket_close_fd(int fd);
extern void socket_close_range(unsigned int first, unsigned int last,
                               int cloexec);
extern int is_epoll_special_fd(int fd);
extern int epoll_close_fd(int fd);
extern void event_descriptors_close_range(unsigned int first,
                                          unsigned int last, int cloexec);
extern void file_locks_release_process_node(pid_t owner, vfs_node_t *node,
                                            int release_flock);

#define VFS_BLOCK_IO_CHUNK 65536

static pipe_t *named_pipe_head;
static spinlock_t named_pipe_lock;

pipe_t *pipe_named_open(vfs_node_t *node, int type) {
    pipe_t *pipe;
    pipe_t *created;

    if (!node || VFS_GET_TYPE(node->flags) != VFS_PIPE) return NULL;
    spin_lock(&named_pipe_lock);
    pipe = named_pipe_head;
    while (pipe) {
        if (pipe->named_node == node) {
            pipe_retain_reference(pipe, type);
            waitq_wake_all(&pipe->read_waitq);
            waitq_wake_all(&pipe->write_waitq);
            spin_unlock(&named_pipe_lock);
            return pipe;
        }
        pipe = pipe->named_next;
    }
    spin_unlock(&named_pipe_lock);
    created = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!created) return NULL;
    memset(created, 0, sizeof(*created));
    waitq_init(&created->read_waitq);
    waitq_init(&created->write_waitq);
    spinlock_init(&created->lock);
    created->named_node = node;
    vfs_open(node, 0);
    spin_lock(&named_pipe_lock);
    pipe = named_pipe_head;
    while (pipe) {
        if (pipe->named_node == node) {
            pipe_retain_reference(pipe, type);
            waitq_wake_all(&pipe->read_waitq);
            waitq_wake_all(&pipe->write_waitq);
            spin_unlock(&named_pipe_lock);
            vfs_close(node);
            kfree(created);
            return pipe;
        }
        pipe = pipe->named_next;
    }
    created->named_next = named_pipe_head;
    named_pipe_head = created;
    pipe_retain_reference(created, type);
    waitq_wake_all(&created->read_waitq);
    waitq_wake_all(&created->write_waitq);
    spin_unlock(&named_pipe_lock);
    return created;
}

void pipe_destroy_if_unused(pipe_t *pipe) {
    pipe_t **link;
    vfs_node_t *node;
    uint64_t flags;
    int unused;

    if (!pipe) return;
    spin_lock(&named_pipe_lock);
    flags = pipe_lock_irqsave(pipe);
    unused = pipe->readers <= 0 && pipe->writers <= 0;
    pipe_unlock_irqrestore(pipe, flags);
    if (!unused) {
        spin_unlock(&named_pipe_lock);
        return;
    }
    node = pipe->named_node;
    if (node) {
        link = &named_pipe_head;
        while (*link && *link != pipe) link = &(*link)->named_next;
        if (*link == pipe) *link = pipe->named_next;
    }
    spin_unlock(&named_pipe_lock);
    if (pipe->buffer) kfree(pipe->buffer);
    if (node) vfs_close(node);
    kfree(pipe);
}

int vfs_check_perm(vfs_node_t *node, int want) {
    uint64_t mode;
    uint64_t uid;
    uint64_t gid;
    int shift;
    int allowed;
    int i;

    if (!current_task) return -ESRCH;
    if (!node) return -ENOENT;

    uid = current_task->euid;
    gid = current_task->egid;

    if (uid == 0) return 0;

    mode = node->mask;

    if (uid == node->uid) {
        shift = 6;
    } else {
        shift = gid == node->gid ? 3 : 0;
        for (i = 0; current_task->groups && shift == 0 &&
             i < current_task->ngroups; i++) {
            if (current_task->groups[i] == node->gid) shift = 3;
        }
    }

    allowed = (int)((mode >> shift) & 7);
    if ((allowed & want) == want) return 0;
    return -EACCES;
}

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

#if 0
static const char *resolve_cwd_path(const char *pathname, char *resolved, size_t size) {
    uint64_t path_addr;
    const char *cwd;
    size_t cwd_len;
    size_t path_len;
    size_t pos;
    size_t i;

    if (!pathname || !resolved || size == 0) return NULL;

    path_addr = (uint64_t)(uintptr_t)pathname;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return NULL;
    if (!vfs_user_string_mapped(pathname, size)) return NULL;

    if (pathname[0] == '/') return pathname;

    cwd = "/";
    if (current_task && current_task->cwd && current_task->cwd[0]) cwd = current_task->cwd;

    cwd_len = 0;
    while (cwd[cwd_len]) cwd_len++;

    path_len = 0;
    while (pathname[path_len]) path_len++;

    if (cwd_len + 1 + path_len + 1 > size) return NULL;

    pos = 0;
    for (i = 0; i < cwd_len && pos < size - 1; i++) resolved[pos++] = cwd[i];
    if (pos > 0 && resolved[pos - 1] != '/' && pos < size - 1) resolved[pos++] = '/';
    for (i = 0; i < path_len && pos < size - 1; i++) resolved[pos++] = pathname[i];
    resolved[pos] = '\0';
    return resolved;
}

static int split_parent_child_path(const char *path, char *parent_path, size_t parent_size, char *child, size_t child_size) {
    int len;
    int last_slash;
    int i;
    int j;

    if (!path || !parent_path || !child || parent_size == 0 || child_size == 0) return -EFAULT;

    len = 0;
    while (path[len]) len++;

    last_slash = -1;
    for (i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }

    if (last_slash < 0) {
        parent_path[0] = '/';
        if (parent_size > 1) parent_path[1] = '\0';
        for (i = 0; i < len && (size_t)i < child_size - 1; i++) child[i] = path[i];
        child[i] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/';
        if (parent_size > 1) parent_path[1] = '\0';
        j = 0;
        for (i = 1; i < len && (size_t)j < child_size - 1; i++, j++) child[j] = path[i];
        child[j] = '\0';
    } else {
        for (i = 0; i < last_slash && (size_t)i < parent_size - 1; i++) parent_path[i] = path[i];
        parent_path[i] = '\0';
        j = 0;
        for (i = last_slash + 1; i < len && (size_t)j < child_size - 1; i++, j++) child[j] = path[i];
        child[j] = '\0';
    }

    if (child[0] == '\0') return -EINVAL;
    return 0;
}
#endif

static char *resolve_cwd_path_alloc(const char *pathname) {
    size_t path_len;
    size_t cwd_len;
    size_t position;
    const char *cwd;
    char *path;
    char *input;

    input = copy_string_from_user_alloc(pathname);
    if (!input) return NULL;
    path_len = strlen(input);
    if (input[0] == '/') return input;
    cwd = current_task && current_task->cwd && current_task->cwd[0] ?
          current_task->cwd : "/";
    cwd_len = strlen(cwd);
    if (path_len > SIZE_MAX - 2 || cwd_len > SIZE_MAX - path_len - 2) {
        kfree(input);
        return NULL;
    }
    path = (char *)kmalloc(cwd_len + path_len + 2);
    if (!path) {
        kfree(input);
        return NULL;
    }
    memcpy(path, cwd, cwd_len);
    position = cwd_len;
    if (position == 0 || path[position - 1] != '/') path[position++] = '/';
    memcpy(path + position, input, path_len + 1);
    kfree(input);
    return path;
}

static int split_parent_child_inplace(char *path, char **parent,
                                      char **child) {
    char *slash;

    if (!path || !parent || !child) return -EINVAL;
    slash = strrchr(path, '/');
    if (!slash) {
        *parent = "/";
        *child = path;
    } else if (slash == path) {
        *parent = "/";
        *child = slash + 1;
    } else {
        *slash = '\0';
        *parent = path;
        *child = slash + 1;
    }
    return (*child)[0] ? 0 : -EINVAL;
}

int syscall_vfs_open_resolved(char *path, int flags, int mode) {
    uint64_t create_mode;
    char *split_path;
    char *parent_path;
    char *filename;
    int fd;
    int ret;
    vfs_node_t *node;
    vfs_node_t *parent;
    pipe_t *pipe;
    int pipe_type;
    int want;
    int perm_ret;
    uint64_t pipe_flags;
    uint64_t mount_flags;

    if (!path) return -EFAULT;
    if (!current_task) {
        kfree(path);
        return -ESRCH;
    }
    if (pty_path_supported(path)) {
        fd = pty_open_path(path, flags);
        if (fd != PTY_OPEN_FALLBACK) {
            kfree(path);
            if (fd == PTY_OPEN_NOCTTY) return -ENXIO;
            return fd < 0 ? -ENODEV : fd;
        }
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

        split_path = (char *)kmalloc(strlen(path) + 1);
        if (!split_path) {
            kfree(path);
            return -ENOMEM;
        }
        strcpy(split_path, path);
        ret = split_parent_child_inplace(split_path, &parent_path, &filename);
        if (ret < 0) {
            kfree(split_path);
            kfree(path);
            return ret;
        }

        parent = vfs_namei(parent_path);
        if (!parent) {
            kfree(split_path);
            kfree(path);
            return -ENOENT;
        }
        if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
            vfs_release(parent);
            kfree(split_path);
            kfree(path);
            return -EROFS;
        }

        ret = vfs_create(parent, filename, create_mode);
        vfs_release(parent);
        kfree(split_path);
        if (ret < 0 && !(flags & VFS_O_EXCL)) {
            node = vfs_namei(path);
        } else if (ret == 0) {
            node = vfs_namei(path);
        }
    }

    kfree(path);

    if (!node) {
        return -ENOENT;
    }

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
    if ((flags & VFS_O_WRONLY) || (flags & VFS_O_RDWR))
        want |= VFS_PERM_WRITE;
    if (flags & VFS_O_TRUNC)
        want |= VFS_PERM_WRITE;
    perm_ret = vfs_check_perm(node, want);
    if (perm_ret < 0) {
        vfs_release(node);
        return perm_ret;
    }

    if ((flags & VFS_O_TRUNC) && node->ops && node->ops->truncate) {
        node->ops->truncate(node, 0);
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

    fd = task_fd_alloc_from(0);
    if (fd < 0) {
        vfs_release(node);
        return fd;
    }

    vfs_open(node, flags);

    current_task->fds[fd].type = FD_TYPE_FILE;
    current_task->fds[fd].node = node;
    current_task->fds[fd].offset = (flags & VFS_O_APPEND) ? node->length : 0;
    current_task->fds[fd].flags = (uint64_t)flags;
    return fd;
}

static int sys_vfs_open(uint64_t path_ptr, uint64_t flags_arg,
                        uint64_t mode_arg) {
    uint64_t path_addr;
    char *path;

    path_addr = path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path_alloc((const char *)path_addr);
    if (!path) return -EFAULT;
    return syscall_vfs_open_resolved(path, (int)flags_arg, (int)mode_arg);
}

static int sys_vfs_close(int fd, const char *unused1, int unused2) {
    task_fd_t *tfd;
    pipe_t *p;
    vfs_node_t *node;
    int release_flock;
    int i;
    int lock_group;
    int endpoint;
    int pty_type;

    (void)unused1; (void)unused2;
    if (is_socket_fd(fd)) return socket_close_fd(fd);
    if (is_epoll_special_fd(fd)) return epoll_close_fd(fd);
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    p = NULL;
    node = NULL;
    release_flock = 1;
    if (FD_TYPE_IS_PIPE(tfd->type)) {
        p = (pipe_t *)tfd->private_data;
        if (p) {
            if (pipe_release_reference(p, tfd->type)) {
                pipe_destroy_if_unused(p);
            }
        }
        memset(tfd, 0, sizeof(*tfd));
        task_fd_reclaim_unused(current_task);
        return 0;
    }

    if (FD_TYPE_IS_PTY(tfd->type)) {
        endpoint = (int)(uintptr_t)tfd->private_data;
        pty_type = tfd->type;
        memset(tfd, 0, sizeof(*tfd));
        if (pty_type == FD_TYPE_PTY_MASTER)
            pty_close_master(endpoint);
        else
            pty_close_slave(endpoint);
        task_fd_reclaim_unused(current_task);
        return 0;
    }

    if (tfd->type == FD_TYPE_FILE && tfd->node) {
        node = (vfs_node_t *)tfd->node;
        lock_group = tfd->ref_count;
        if (lock_group > 1) {
            for (i = 0; i < current_task->fds_capacity; i++) {
                if (i == fd || !current_task->fds[i].in_use) continue;
                if (current_task->fds[i].type == FD_TYPE_FILE &&
                    current_task->fds[i].node == node &&
                    current_task->fds[i].ref_count == lock_group) {
                    release_flock = 0;
                    break;
                }
            }
        }
        file_locks_release_process_node(current_task->pid, node,
                                        release_flock);
        memset(tfd, 0, sizeof(*tfd));
        vfs_close(node);
        task_fd_reclaim_unused(current_task);
        return 0;
    }

    memset(tfd, 0, sizeof(*tfd));
    task_fd_reclaim_unused(current_task);
    return 0;
}

static int sys_close_range(unsigned int first, const char *last_ptr,
                           unsigned int flags) {
    unsigned int last;
    int fd;

    if (!current_task) return -ESRCH;
    if (flags & ~4u) return -EINVAL;
    last = (unsigned int)(uintptr_t)last_ptr;
    if (first > last) return -EINVAL;
    socket_close_range(first, last, (flags & 4u) != 0);
    event_descriptors_close_range(first, last, (flags & 4u) != 0);
    if (first >= (unsigned int)current_task->fds_capacity) return 0;
    if (last >= (unsigned int)current_task->fds_capacity)
        last = (unsigned int)current_task->fds_capacity - 1;
    if (flags & 4u) {
        for (fd = (int)first; fd <= (int)last; fd++) {
            if (current_task->fds[fd].in_use)
                current_task->fds[fd].flags |= 1u;
        }
        return 0;
    }
    for (fd = (int)last; fd >= (int)first; fd--) {
        if (current_task->fds[fd].in_use)
            sys_vfs_close(fd, NULL, 0);
    }
    return 0;
}

int sys_vfs_readdir(registers_t *regs) {
    int fd;
    uint64_t name_addr;
    uint64_t type_addr;
    uint64_t index;
    int i;
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t local_copy;
    const char *entry_name;

    fd = (int)regs->rbx;
    name_addr = regs->rcx;
    type_addr = regs->rdx;
    index = regs->rsi;

    if (name_addr && !syscall_user_range_mapped(name_addr, 64, 1))
        return -EFAULT;
    if (type_addr &&
        !syscall_user_range_mapped(type_addr, sizeof(uint32_t), 1))
        return -EFAULT;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;

    if (vfs_readdir_copy(node, index, &local_copy) != 0) return -ENOENT;
    entry_name = vfs_dirent_name(&local_copy);

    if (name_addr) {
        i = 0;
        for (; i < 63 && entry_name[i]; i++) {
            ((char *)name_addr)[i] = entry_name[i];
        }
        ((char *)name_addr)[i] = '\0';
    }

    if (type_addr) *(uint32_t *)type_addr = (uint32_t)local_copy.type;
    vfs_dirent_release_name(&local_copy);
    return 0;
}

static int sys_vfs_readdir2(int fd, uint64_t name_addr, uint64_t capacity,
                            uint64_t type_addr, uint64_t index,
                            uint64_t required_addr) {
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t local_copy;
    const char *entry_name;
    size_t required;
    uint32_t type;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;
    if (required_addr &&
        !syscall_user_range_mapped(required_addr, sizeof(size_t), 1)) return -EFAULT;
    if (type_addr &&
        !syscall_user_range_mapped(type_addr, sizeof(uint32_t), 1)) return -EFAULT;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;
    if (vfs_readdir_copy(node, index, &local_copy) != 0) return -ENOENT;

    entry_name = vfs_dirent_name(&local_copy);
    required = strlen(entry_name) + 1;
    if (required_addr) *(size_t *)(uintptr_t)required_addr = required;
    if (type_addr) {
        type = (uint32_t)local_copy.type;
        *(uint32_t *)(uintptr_t)type_addr = type;
    }
    if (!name_addr) {
        vfs_dirent_release_name(&local_copy);
        return capacity == 0 ? 0 : -EFAULT;
    }
    if (capacity < required) {
        vfs_dirent_release_name(&local_copy);
        return -ERANGE;
    }
    if (!syscall_user_range_mapped(name_addr, required, 1)) {
        vfs_dirent_release_name(&local_copy);
        return -EFAULT;
    }
    memcpy((void *)(uintptr_t)name_addr, entry_name, required);
    vfs_dirent_release_name(&local_copy);
    return 0;
}

static int sys_vfs_stat(int fd, const char *size_ptr, uint64_t type_ptr) {
    uint64_t size_addr;
    uint64_t type_addr;
    uint64_t size;
    uint64_t flags;
    task_fd_t *tfd;
    vfs_node_t *node;

    size_addr = (uint64_t)size_ptr;
    type_addr = (uint64_t)type_ptr;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    size = node->length;
    flags = node->flags;
    
    if (size_addr && size_addr < KERNEL_VMA && size_addr >= 0x1000) {
        *(uint64_t*)size_addr = size;
    }
    if (type_addr && type_addr < KERNEL_VMA && type_addr >= 0x1000) {
        *(uint64_t*)type_addr = flags;
    }
    return 0;
}

static int sys_vfs_mounts(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    vfs_list_mounts();
    return vfs_get_mount_count();
}

static int sys_vfs_create(uint64_t path_ptr, const char *perms_ptr, int unused) {
    uint64_t path_addr;
    char *path;
    char *parent_path;
    char *filename;
    uint64_t perms;
    int ret;
    vfs_node_t *parent;
    int perm_ret;
    int r;

    (void)unused;
    path_addr = (uint64_t)path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path_alloc((const char *)path_addr);
    if (!path) return -EFAULT;
    perms = (uint64_t)(uintptr_t)perms_ptr;
    if (current_task) perms &= ~current_task->creation_mask;

    ret = split_parent_child_inplace(path, &parent_path, &filename);
    if (ret < 0) { kfree(path); return ret; }

    parent = vfs_namei(parent_path);
    if (!parent) { kfree(path); return -ENOENT; }
    if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
        vfs_release(parent);
        kfree(path);
        return -EROFS;
    }
    perm_ret = vfs_check_perm(parent, VFS_PERM_WRITE);
    if (perm_ret < 0) { vfs_release(parent); kfree(path); return perm_ret; }
    r = vfs_create(parent, filename, perms);
    vfs_release(parent);
    kfree(path);
    return r;
}

static int sys_vfs_mkdir(uint64_t path_ptr, const char *perms_ptr, int unused) {
    uint64_t path_addr;
    char *path;
    char *parent_path;
    char *dirname;
    uint64_t perms;
    int ret;
    vfs_node_t *parent;
    int perm_ret;
    int r;

    (void)unused;
    path_addr = (uint64_t)path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path_alloc((const char *)path_addr);
    if (!path) return -EFAULT;
    perms = (uint64_t)(uintptr_t)perms_ptr;
    if (current_task) perms &= ~current_task->creation_mask;

    ret = split_parent_child_inplace(path, &parent_path, &dirname);
    if (ret < 0) { kfree(path); return ret; }

    parent = vfs_namei(parent_path);
    if (!parent) { kfree(path); return -ENOENT; }
    if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
        vfs_release(parent);
        kfree(path);
        return -EROFS;
    }
    perm_ret = vfs_check_perm(parent, VFS_PERM_WRITE);
    if (perm_ret < 0) { vfs_release(parent); kfree(path); return perm_ret; }
    r = vfs_mkdir(parent, dirname, perms);
    vfs_release(parent);
    kfree(path);
    return r;
}

static int sys_vfs_unlink(uint64_t path_ptr, const char *unused1, int unused2) {
    uint64_t path_addr;
    char *path;
    char *parent_path;
    char *filename;
    int ret;
    vfs_node_t *parent;
    int perm_ret;
    int r;

    (void)unused1; (void)unused2;
    path_addr = (uint64_t)path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path_alloc((const char *)path_addr);
    if (!path) return -EFAULT;

    ret = split_parent_child_inplace(path, &parent_path, &filename);
    if (ret < 0) { kfree(path); return ret; }

    parent = vfs_namei(parent_path);
    if (!parent) { kfree(path); return -ENOENT; }
    if (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY) {
        vfs_release(parent);
        kfree(path);
        return -EROFS;
    }
    perm_ret = vfs_check_perm(parent, VFS_PERM_WRITE);
    if (perm_ret < 0) { vfs_release(parent); kfree(path); return perm_ret; }
    r = vfs_unlink_checked(parent, filename, 0);
    vfs_release(parent);
    kfree(path);
    return r;
}

struct statfs_kernel {
    unsigned long f_type;
    unsigned long f_bsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_files;
    unsigned long long f_ffree;
    int f_fsid[2];
    unsigned long f_namelen;
    unsigned long f_frsize;
    unsigned long f_flags;
    unsigned long f_spare[4];
};

#define RAMFS_MAGIC     0x858458F6
#define EXT4_MAGIC      0xEF53
#define PROCFS_MAGIC    0x9FA0
#define DEVFS_MAGIC     0x1373

static void fill_statfs_for_path(const char *path, struct statfs_kernel *buf) {
    int mount_count;
    int i;
    size_t best_len;
    size_t mlen;
    const char *fsname;
    extern int ramfs_get_stats(ramfs_stats_t *stats);
    vfs_mount_t *mount;
    vfs_mount_t *best;
    vfs_node_t *root;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint32_t bsize32;
    uint64_t bsize;
    uint64_t total_kb;
    uint64_t free_kb;
    ramfs_stats_t rs;
    uint64_t used;
    squashfs_context_t *sqctx;

    memset(buf, 0, sizeof(struct statfs_kernel));
    buf->f_namelen = 255;

    best = NULL;
    best_len = 0;
    mount_count = vfs_get_mount_count();
    for (i = 0; i < mount_count; i++) {
        mount = vfs_get_mount(i);
        if (!mount) continue;
        mlen = strlen(mount->path);
        if (strncmp(path, mount->path, mlen) == 0 &&
            (path[mlen] == '\0' || path[mlen] == '/' ||
             (mlen == 1 && mount->path[0] == '/'))) {
            if (mlen > best_len) {
                best = mount;
                best_len = mlen;
            }
        }
    }

    fsname = (best && best->fs_type && best->fs_type->name) ? best->fs_type->name : "";

    if (!fsname[0] && path && path[0] == '/') {
        root = vfs_get_root();
        if (root && root->private_data && ext4_get_stats(NULL, NULL, NULL) == 0) {
            fsname = "ext4";
        }
    }

    if (strcmp(fsname, "ext4") == 0) {
        if (ext4_get_stats(&total_blocks, &free_blocks, &bsize32) == 0) {
            bsize = bsize32;
            buf->f_type = EXT4_MAGIC;
            buf->f_bsize = bsize;
            buf->f_frsize = bsize;
            buf->f_blocks = total_blocks;
            buf->f_bfree = free_blocks;
            buf->f_bavail = free_blocks;
            buf->f_files = 1024;
            buf->f_ffree = 512;
            return;
        }
    }

    if (strcmp(fsname, "tmpfs") == 0) {
        total_kb = pfa_get_usable_ram_kb() / 2;
        free_kb = pfa_count_free() * 4;
        if (free_kb > total_kb) free_kb = total_kb;
        buf->f_type = RAMFS_MAGIC;
        buf->f_bsize = 4096;
        buf->f_frsize = 4096;
        buf->f_blocks = (uint64_t)total_kb * 1024 / 4096;
        buf->f_bfree = (uint64_t)free_kb * 1024 / 4096;
        buf->f_bavail = buf->f_bfree;
        buf->f_files = 1024;
        buf->f_ffree = 1024;
        return;
    }

    if (strcmp(fsname, "ramfs") == 0 || strcmp(fsname, "overlayfs") == 0) {
        if (ramfs_get_stats(&rs) == 0) {
            used = rs.used_size;
            sqctx = squashfs_get_context();
            if (sqctx && sqctx->base)
                used += sqctx->size;
            buf->f_type = RAMFS_MAGIC;
            buf->f_bsize = 4096;
            buf->f_frsize = 4096;
            buf->f_blocks = rs.total_size / 4096;
            buf->f_bfree = (rs.total_size - used) / 4096;
            buf->f_bavail = buf->f_bfree;
            buf->f_files = rs.file_count + rs.dir_count + 1000;
            buf->f_ffree = 1000;
            return;
        }
    }

    if (strcmp(fsname, "procfs") == 0) {
        total_kb = pfa_get_usable_ram_kb();
        buf->f_type = PROCFS_MAGIC;
        buf->f_bsize = 4096;
        buf->f_frsize = 4096;
        buf->f_blocks = (uint64_t)total_kb * 1024 / 4096;
        buf->f_bfree = buf->f_blocks;
        buf->f_bavail = buf->f_blocks;
        return;
    }

    if (strcmp(fsname, "devfs") == 0) {
        total_kb = pfa_get_usable_ram_kb();
        buf->f_type = DEVFS_MAGIC;
        buf->f_bsize = 4096;
        buf->f_frsize = 4096;
        buf->f_blocks = (uint64_t)total_kb * 1024 / 4096;
        buf->f_bfree = buf->f_blocks;
        buf->f_bavail = buf->f_blocks;
        return;
    }

    if (strcmp(fsname, "sysfs") == 0) {
        total_kb = pfa_get_usable_ram_kb();
        buf->f_type = 0x62656572;
        buf->f_bsize = 4096;
        buf->f_frsize = 4096;
        buf->f_blocks = (uint64_t)total_kb * 1024 / 4096;
        buf->f_bfree = buf->f_blocks;
        buf->f_bavail = buf->f_blocks;
        return;
    }

    buf->f_type = 0;
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 0;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
}

static int sys_statfs(uint64_t path_ptr, const char *size_ptr,
                      uint64_t buf_ptr_int) {
    uint64_t path_addr;
    int size;
    uint64_t buf_addr;
    uint64_t arg2_addr;
    char *path;
    struct statfs_kernel *buf;

    path_addr = (uint64_t)path_ptr;
    size = (int)(uintptr_t)size_ptr;
    buf_addr = (uint64_t)buf_ptr_int;
    arg2_addr = (uint64_t)(uintptr_t)size_ptr;
    if ((buf_addr < 0x1000 || buf_addr >= KERNEL_VMA) &&
        arg2_addr >= 0x1000 && arg2_addr < KERNEL_VMA) {
        buf_addr = arg2_addr;
        size = (int)sizeof(struct statfs_kernel);
    }
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) {
        return -EFAULT;
    }
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) {
        return -EFAULT;
    }

    if (size < (int)sizeof(struct statfs_kernel)) {
        return -EINVAL;
    }

    path = resolve_cwd_path_alloc((const char *)path_addr);
    if (!path) return -EFAULT;
    buf = (struct statfs_kernel *)buf_addr;

    fill_statfs_for_path(path, buf);
    kfree(path);

    return 0;
}

static int sys_fstatfs(int fd, const char *size_ptr, uint64_t buf_ptr_int) {
    int size;
    uint64_t buf_addr;
    uint64_t arg2_addr;
    struct statfs_kernel *buf;
    task_fd_t *tfd;
    vfs_node_t *node;
    vfs_mount_t *mount;

    size = (int)(uintptr_t)size_ptr;
    buf_addr = (uint64_t)buf_ptr_int;

    arg2_addr = (uint64_t)(uintptr_t)size_ptr;
    if ((buf_addr < 0x1000 || buf_addr >= KERNEL_VMA) &&
        arg2_addr >= 0x1000 && arg2_addr < KERNEL_VMA) {
        buf_addr = arg2_addr;
        size = (int)sizeof(struct statfs_kernel);
    }

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) {
        return -EBADF;
    }
    if (!current_task->fds[fd].in_use) {
        return -EBADF;
    }

    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) {
        return -EFAULT;
    }

    if (size < (int)sizeof(struct statfs_kernel)) {
        return -EINVAL;
    }

    tfd = &current_task->fds[fd];
    if ((tfd->type != FD_TYPE_FILE && !FD_TYPE_IS_PIPE(tfd->type)) ||
        !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    mount = vfs_get_mount_for_node(node);
    if (!mount || !mount->path) return -EINVAL;
    buf = (struct statfs_kernel *)buf_addr;
    fill_statfs_for_path(mount->path, buf);

    return 0;
}

static int sys_vfs_mount_user(uint64_t source_ptr, const char *target_ptr,
                              uint64_t fstype_ptr, int flags_val) {
    uint64_t src_addr;
    uint64_t tgt_addr;
    uint64_t fs_addr;
    uint64_t mnt_flags;
    const char *source;
    const char *target;
    const char *fstype;
    vfs_node_t *mp_check;

    src_addr = (uint64_t)source_ptr;
    tgt_addr = (uint64_t)(uintptr_t)target_ptr;
    fs_addr = (uint64_t)fstype_ptr;
    mnt_flags = (uint64_t)flags_val;

    if (tgt_addr >= KERNEL_VMA || tgt_addr < 0x1000) return -EFAULT;
    if (fs_addr >= KERNEL_VMA || fs_addr < 0x1000) return -EFAULT;

    if (!current_task) return -ESRCH;
    if (!creds_has_capability(current_task, 21)) return -EPERM;

    source = NULL;
    if (src_addr != 0 && src_addr < KERNEL_VMA && src_addr >= 0x1000)
        source = (const char *)src_addr;
    target = (const char *)tgt_addr;
    fstype = (const char *)fs_addr;

    mp_check = vfs_namei(target);
    if (!mp_check)
        return -ENOENT;
    vfs_release(mp_check);

    return vfs_mount_flags(source, target, fstype, mnt_flags);
}

static int sys_vfs_umount_user(uint64_t target_ptr, const char *unused1,
                               int unused2) {
    uint64_t tgt_addr;
    const char *target;

    (void)unused1;
    (void)unused2;
    tgt_addr = (uint64_t)target_ptr;
    if (tgt_addr >= KERNEL_VMA || tgt_addr < 0x1000) return -EFAULT;
    if (!current_task) return -ESRCH;
    if (!creds_has_capability(current_task, 21)) return -EPERM;

    target = (const char *)tgt_addr;
    return vfs_unmount(target);
}

void syscalls_vfs_init(void) {
    syscall_table_set(SYSCALL_OPEN, (void *)(sys_vfs_open));
    syscall_table_set(SYSCALL_CLOSE, (void *)(sys_vfs_close));
    syscall_table_set(SYSCALL_VFS_OPEN, (void *)(sys_vfs_open));
    syscall_table_set(SYSCALL_VFS_CLOSE, (void *)(sys_vfs_close));
    syscall_table_set(SYSCALL_VFS_READ, (void *)(syscall_fd_read));
    syscall_table_set(SYSCALL_VFS_READDIR, (void *)((void*)1));
    syscall_table_set(SYSCALL_VFS_READDIR2, (void *)(sys_vfs_readdir2));
    syscall_table_set(SYSCALL_VFS_STAT, (void *)(sys_vfs_stat));
    syscall_table_set(SYSCALL_VFS_MOUNTS, (void *)(sys_vfs_mounts));
    syscall_table_set(SYSCALL_VFS_WRITE, (void *)(syscall_fd_write));
    syscall_table_set(SYSCALL_VFS_CREATE, (void *)(sys_vfs_create));
    syscall_table_set(SYSCALL_VFS_MKDIR, (void *)(sys_vfs_mkdir));
    syscall_table_set(SYSCALL_VFS_UNLINK, (void *)(sys_vfs_unlink));
    syscall_table_set(SYSCALL_STATFS, (void *)(sys_statfs));
    syscall_table_set(SYSCALL_FSTATFS, (void *)(sys_fstatfs));
    syscall_table_set(SYSCALL_VFS_MOUNT, (void *)(sys_vfs_mount_user));
    syscall_table_set(SYSCALL_VFS_UMOUNT, (void *)(sys_vfs_umount_user));
    syscall_table_set(SYSCALL_CLOSE_RANGE, (void *)(sys_close_range));
}

#include "syscall_defs.h"
#include <lebirun/pit.h>
#include <lebirun/pty.h>
#include <lebirun/common.h>
#include <lebirun/ramfs.h>
#include <lebirun/spinlock.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/timekeeping.h>
#include <lebirun/creds.h>

extern int is_socket_fd(int fd);
extern int socket_fcntl(int fd, int cmd, int arg);
extern int syscall_core_read_for_readv(int fd, char *buf, int len);
void file_locks_release_process_node(pid_t owner, vfs_node_t *node,
                                     int release_flock);

#define fd_table (current_task->fds)

#define POSIX_EXEC_PATH_MAX 256

static uint64_t posix_user_pd(void) {
    if (!current_task) return 0;
    if (current_task->cr3) return current_task->cr3;
    return current_task->pml4_phys;
}

static int posix_user_range_mapped(uint64_t addr, uint64_t size) {
    uint64_t pd;
    uint64_t start;
    uint64_t end;
    uint64_t p;

    if (size == 0) return 0;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return 0;
    if (addr + size < addr || addr + size > KERNEL_VMA) return 0;
    pd = posix_user_pd();
    if (!pd) return 0;
    start = addr & ~0xFFFu;
    end = (addr + size - 1) & ~0xFFFu;
    p = start;
    for (;;) {
        if (vmm_get_phys_in_pml4(pd, p) == 0) return 0;
        if (p == end) break;
        if (p > end) return 0;
        p += 0x1000;
    }
    return 1;
}

static int posix_user_read_u64(uint64_t addr, uint64_t *out) {
    if (!out) return -EFAULT;
    if (!posix_user_range_mapped(addr, sizeof(uint64_t))) return -EFAULT;
    *out = *(uint64_t *)addr;
    return 0;
}

static int posix_copy_user_string(char **out, const char *src_user, uint64_t max_len) {
    char *dst;
    uint64_t addr;
    uint64_t cur;
    uint64_t chunk;
    uint64_t page_remaining;
    uint64_t i;
    uint64_t j;
    uint64_t alloc_len;
    char c;

    if (!out || !src_user || max_len == 0) return -EFAULT;
    *out = NULL;
    addr = (uint64_t)src_user;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return -EFAULT;
    i = 0;
    while (i < max_len) {
        cur = addr + i;
        if (cur < addr) {
            return -EFAULT;
        }
        page_remaining = 0x1000 - (cur & 0xFFF);
        chunk = max_len - i;
        if (chunk > page_remaining) chunk = page_remaining;
        if (!posix_user_range_mapped(cur, chunk)) return -EFAULT;
        for (j = 0; j < chunk; j++) {
            c = *(const char *)(cur + j);
            if (c == '\0') {
                alloc_len = i + j + 1;
                dst = (char *)kmalloc(alloc_len);
                if (!dst) return -ENOMEM;
                memcpy(dst, (const void *)addr, alloc_len);
                *out = dst;
                return 0;
            }
        }
        i += chunk;
    }
    return -ENAMETOOLONG;
}

static char *posix_kstrdup(const char *src) {
    char *dst;
    int len;

    if (!src) return NULL;
    len = strlen(src);
    dst = (char *)kmalloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len + 1);
    return dst;
}

static void posix_free_string_array(char **arr, int count) {
    int i;

    if (!arr) return;
    for (i = 0; i < count; i++) {
        if (arr[i]) kfree(arr[i]);
    }
    kfree(arr);
}

static int posix_copy_user_string_array(char ***out, int *out_count,
                                        uint64_t array_addr,
                                        uint64_t *remaining) {
    char **arr;
    char **new_arr;
    uint64_t str_addr;
    uint64_t slot_addr;
    int count;
    int cap;
    int new_cap;
    int ret;
    uint64_t string_len;
    uint64_t used;

    if (!out || !out_count || !remaining) return -EFAULT;
    *out = NULL;
    *out_count = 0;
    if (!array_addr) return 0;
    cap = 8;
    arr = (char **)kmalloc(cap * sizeof(char *));
    if (!arr) return -ENOMEM;
    memset(arr, 0, cap * sizeof(char *));
    count = 0;
    while (count < INT32_MAX) {
        if (count + 1 >= cap) {
            if (cap > INT32_MAX / 2) {
                posix_free_string_array(arr, count);
                return -E2BIG;
            }
            new_cap = cap * 2;
            new_arr = (char **)krealloc(arr, new_cap * sizeof(char *));
            if (!new_arr) {
                posix_free_string_array(arr, count);
                return -ENOMEM;
            }
            memset(new_arr + cap, 0, (new_cap - cap) * sizeof(char *));
            arr = new_arr;
            cap = new_cap;
        }
        slot_addr = array_addr + (uint64_t)count * sizeof(uint64_t);
        if (slot_addr < array_addr) {
            posix_free_string_array(arr, count);
            return -EFAULT;
        }
        ret = posix_user_read_u64(slot_addr, &str_addr);
        if (ret != 0) {
            posix_free_string_array(arr, count);
            return ret;
        }
        if (!str_addr) break;
        if (*remaining <= sizeof(uint64_t)) {
            posix_free_string_array(arr, count);
            return -E2BIG;
        }
        ret = posix_copy_user_string(&arr[count], (const char *)str_addr,
                                     *remaining - sizeof(uint64_t));
        if (ret != 0) {
            posix_free_string_array(arr, count);
            return ret;
        }
        string_len = strlen(arr[count]) + 1u;
        used = (string_len + 7u) & ~7u;
        if (used > *remaining - sizeof(uint64_t)) {
            kfree(arr[count]);
            posix_free_string_array(arr, count);
            return -E2BIG;
        }
        *remaining -= used + sizeof(uint64_t);
        count++;
    }
    if (count == INT32_MAX) {
        posix_free_string_array(arr, count);
        return -E2BIG;
    }
    arr[count] = NULL;
    *out = arr;
    *out_count = count;
    return 0;
}

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

static void fd_release_entry(task_fd_t *tfd) {
    int pipe_type;
    vfs_node_t *node_to_close;
    pipe_t *pipe_to_release;
    int release_flock;
    int i;

    if (!tfd || !tfd->in_use) return;

    node_to_close = NULL;
    pipe_to_release = NULL;
    pipe_type = 0;
    release_flock = 1;

    if (FD_TYPE_IS_PIPE(tfd->type)) {
        pipe_to_release = (pipe_t *)tfd->private_data;
        pipe_type = tfd->type;
    } else if (tfd->type == FD_TYPE_FILE && tfd->node) {
        node_to_close = (vfs_node_t *)tfd->node;
        if (current_task && tfd->ref_count > 1) {
            for (i = 0; i < current_task->fds_capacity; i++) {
                if (&current_task->fds[i] == tfd ||
                    !current_task->fds[i].in_use)
                    continue;
                if (current_task->fds[i].type == FD_TYPE_FILE &&
                    current_task->fds[i].node == node_to_close &&
                    current_task->fds[i].ref_count == tfd->ref_count) {
                    release_flock = 0;
                    break;
                }
            }
        }
        if (current_task)
            file_locks_release_process_node(current_task->pid, node_to_close,
                                            release_flock);
    }

    memset(tfd, 0, sizeof(*tfd));

    if (pipe_to_release) {
        if (pipe_release_reference(pipe_to_release, pipe_type)) {
            pipe_destroy_if_unused(pipe_to_release);
        }
    }

    if (node_to_close) {
        vfs_close(node_to_close);
    }
}

static void fd_retain_entry(task_fd_t *tfd) {
    pipe_t *p;
    vfs_node_t *node;

    if (!tfd || !tfd->in_use) return;
    if (tfd->type == FD_TYPE_FILE && tfd->node) {
        node = (vfs_node_t *)tfd->node;
        vfs_open(node, 0);
        return;
    }
    if (tfd->private_data && FD_TYPE_IS_PIPE(tfd->type)) {
        p = (pipe_t *)tfd->private_data;
        pipe_retain_reference(p, tfd->type);
    }
}

static int fd_alloc_from(int start) {
    int capacity;
    int i;
    int ret;

    if (!current_task) return -1;
    if (start < 0) start = 0;
    capacity = current_task->fds_capacity;
    for (i = start; i < capacity; i++) {
        if (!fd_table[i].in_use) {
            memset(&fd_table[i], 0, sizeof(task_fd_t));
            fd_table[i].in_use = 1;
            fd_table[i].ref_count = 1;
            return i;
        }
    }
    ret = task_fd_ensure_capacity(current_task, start >= capacity ? start : capacity);
    if (ret != 0) return -1;
    capacity = current_task->fds_capacity;
    for (i = start; i < capacity; i++) {
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
    int newfd;
    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !fd_table[oldfd].in_use) return -EBADF;
    newfd = fd_alloc();
    if (newfd < 0) return -EMFILE;
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(task_fd_t));
    fd_table[newfd].ref_count = 1;
    fd_table[newfd].flags &= ~1;
    fd_retain_entry(&fd_table[newfd]);
    task_fd_position_share(&fd_table[oldfd], &fd_table[newfd]);
    return newfd;
}

static int sys_dup2(int oldfd, const char *newfd_ptr, int unused) {
    int newfd;
    int ret;

    (void)unused;
    newfd = (int)(uintptr_t)newfd_ptr;
    if (!current_task) return -ESRCH;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !fd_table[oldfd].in_use) return -EBADF;
    if (newfd < 0) return -EBADF;
    if (newfd >= current_task->fds_capacity) {
        ret = task_fd_ensure_capacity(current_task, newfd);
        if (ret != 0) return -EMFILE;
    }
    if (oldfd == newfd) return newfd;
    if (fd_table[newfd].in_use) {
        fd_release_entry(&fd_table[newfd]);
    }
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(task_fd_t));
    fd_table[newfd].ref_count = 1;
    fd_table[newfd].flags &= ~1;
    fd_retain_entry(&fd_table[newfd]);
    task_fd_position_share(&fd_table[oldfd], &fd_table[newfd]);
    return newfd;
}

static int sys_pipe(int pipefd_ptr, const char *unused1, int unused2) {
    uint64_t addr;
    int *pipefd;
    int rfd;
    int wfd;
    pipe_t *p;

    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    addr = (uint64_t)pipefd_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    pipefd = (int *)addr;
    p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(pipe_t));
    waitq_init(&p->read_waitq);
    waitq_init(&p->write_waitq);
    spinlock_init(&p->lock);
    p->readers = 1;
    p->writers = 1;
    rfd = fd_alloc();
    if (rfd < 0) { kfree(p); return -EMFILE; }
    wfd = fd_alloc();
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
    uint64_t buf_addr;
    uint64_t size;
    const char *cwd;
    uint64_t len;
    (void)unused;
    buf_addr = (uint64_t)buf_ptr;
    size = (uint64_t)(uintptr_t)size_ptr;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (size == 0) return -EINVAL;
    cwd = current_task && current_task->cwd ? current_task->cwd : "/";
    if (!cwd[0]) cwd = "/";
    len = 0;
    while (cwd[len]) len++;
    if (len + 1 > size) return -ERANGE;
    if (!posix_user_range_mapped(buf_addr, len + 1)) return -EFAULT;
    memcpy((void *)buf_addr, cwd, len + 1);
    return buf_ptr;
}

static int sys_chdir(int path_ptr, const char *unused1, int unused2) {
    uint64_t addr;
    char *path;
    char *resolved;
    char resolved_path[VFS_MAX_PATH];
    vfs_node_t *node;
    int ret;

    (void)unused1; (void)unused2;
    addr = (uint64_t)path_ptr;
    ret = posix_copy_user_string(&path, (const char *)addr, 256);
    if (ret < 0) return ret;
    if (strncmp(path, "/ro", 3) == 0 && (path[3] == '\0' || path[3] == '/')) {
        kfree(path);
        return -EACCES;
    }
    node = vfs_namei(path);
    if (!node) { kfree(path); return -ENOENT; }
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) { vfs_release(node); kfree(path); return -ENOTDIR; }
    if (!current_task) { vfs_release(node); kfree(path); return -EFAULT; }
    resolved = vfs_get_path(node, resolved_path, sizeof(resolved_path));
    ret = task_set_cwd(current_task, resolved ? resolved : path);
    vfs_release(node);
    kfree(path);
    return ret == 0 ? 0 : -ENOMEM;
}

static int sys_chroot(const char *path_arg, int unused1, int unused2) {
    char *path;
    char resolved[VFS_MAX_PATH];
    char *canonical;
    vfs_node_t *node;
    int result;

    (void)unused1;
    (void)unused2;
    if (!current_task) return -ESRCH;
    if (!creds_has_capability(current_task, 18)) return -EPERM;
    result = posix_copy_user_string(&path, path_arg, VFS_MAX_PATH);
    if (result < 0) return result;
    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        vfs_release(node);
        return -ENOTDIR;
    }
    canonical = vfs_get_path(node, resolved, sizeof(resolved));
    if (!canonical) {
        vfs_release(node);
        return -EINVAL;
    }
    result = task_set_root(current_task, canonical);
    if (result == 0) result = task_set_cwd(current_task, "/");
    vfs_release(node);
    return result == 0 ? 0 : -ENOMEM;
}

static int sys_pivot_root(const char *new_root_arg, const char *put_old_arg,
                          int unused) {
    char *new_root;
    char *put_old;
    char new_path[VFS_MAX_PATH];
    char old_path[VFS_MAX_PATH];
    char *new_canonical;
    char *old_canonical;
    vfs_node_t *new_node;
    vfs_node_t *old_node;
    vfs_mount_t *new_mount;
    size_t new_length;
    int result;

    (void)unused;
    if (!current_task) return -ESRCH;
    if (!creds_has_capability(current_task, 21)) return -EPERM;
    result = posix_copy_user_string(&new_root, new_root_arg, VFS_MAX_PATH);
    if (result < 0) return result;
    result = posix_copy_user_string(&put_old, put_old_arg, VFS_MAX_PATH);
    if (result < 0) {
        kfree(new_root);
        return result;
    }
    new_node = vfs_namei(new_root);
    old_node = vfs_namei(put_old);
    kfree(new_root);
    kfree(put_old);
    if (!new_node || !old_node) {
        if (new_node) vfs_release(new_node);
        if (old_node) vfs_release(old_node);
        return -ENOENT;
    }
    if (VFS_GET_TYPE(new_node->flags) != VFS_DIRECTORY ||
        VFS_GET_TYPE(old_node->flags) != VFS_DIRECTORY) {
        vfs_release(new_node);
        vfs_release(old_node);
        return -ENOTDIR;
    }
    new_mount = vfs_get_mount_for_node(new_node);
    if (!new_mount || new_mount->root != new_node || new_node == old_node) {
        vfs_release(new_node);
        vfs_release(old_node);
        return -EINVAL;
    }
    new_canonical = vfs_get_path(new_node, new_path, sizeof(new_path));
    old_canonical = vfs_get_path(old_node, old_path, sizeof(old_path));
    if (!new_canonical || !old_canonical) {
        vfs_release(new_node);
        vfs_release(old_node);
        return -EINVAL;
    }
    new_length = strlen(new_canonical);
    if (strncmp(old_canonical, new_canonical, new_length) != 0 ||
        old_canonical[new_length] != '/') {
        vfs_release(new_node);
        vfs_release(old_node);
        return -EINVAL;
    }
    result = task_set_root(current_task, new_canonical);
    if (result == 0) result = task_set_cwd(current_task, "/");
    vfs_release(new_node);
    vfs_release(old_node);
    return result == 0 ? 0 : -ENOMEM;
}

static inline uint64_t vfs_mask_to_unix_perms(uint64_t mask);

static int sys_access(int path_ptr, const char *mode_ptr, int unused) {
    uint64_t addr;
    int mode;
    char *path;
    vfs_node_t *node;
    uint64_t perms;
    int ret;

    (void)unused;
    addr = (uint64_t)path_ptr;
    mode = (int)(uintptr_t)mode_ptr;
    if (mode & ~7) return -EINVAL;
    ret = posix_copy_user_string(&path, (const char *)addr, 256);
    if (ret < 0) return ret;
    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;
    if (mode == 0) { vfs_release(node); return 0; }
    if (current_task && current_task->uid == 0) { vfs_release(node); return 0; }
    perms = vfs_mask_to_unix_perms(node->mask);
    if (!perms) {
        if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY)
            perms = 0755;
        else
            perms = 0644;
    }
    if ((mode & 4) && !(perms & 0444)) { vfs_release(node); return -EACCES; }
    if ((mode & 2) && !(perms & 0222)) { vfs_release(node); return -EACCES; }
    if ((mode & 1) && !(perms & 0111)) { vfs_release(node); return -EACCES; }
    vfs_release(node);
    return 0;
}

static inline uint64_t vfs_mask_to_unix_perms(uint64_t mask) {
    if (mask != 0 && (mask & ~0x07u) == 0) {
        uint64_t perms = 0;
        if (mask & VFS_PERM_READ) perms |= 0444;
        if (mask & VFS_PERM_WRITE) perms |= 0222;
        if (mask & VFS_PERM_EXEC) perms |= 0111;
        return perms;
    }
    return mask & 07777u;
}

static inline uint64_t vfs_node_to_unix_mode(const vfs_node_t *node) {
    uint64_t perms = 0;
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
    uint64_t path_addr;
    uint64_t buf_addr;
    char *path;
    struct kernel_stat *st;
    vfs_node_t *node;
    int ret;

    (void)unused;
    path_addr = (uint64_t)path_ptr;
    buf_addr = (uint64_t)(uintptr_t)buf_ptr;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    ret = posix_copy_user_string(&path, (const char *)path_addr, 256);
    if (ret < 0) return ret;
    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;
    st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    st->st_dev = 1;
    st->st_ino = node->inode ? node->inode : 1;

    st->st_mode = vfs_node_to_unix_mode(node);
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

static int sys_fstat(int fd, const char *buf_ptr, int unused) {
    uint64_t buf_addr;
    struct kernel_stat *st;
    int pty_fd;
    uint64_t size;
    uint64_t flags;
    vfs_node_t *node;
    uint64_t node_address;
    int ret;

    (void)unused;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    buf_addr = (uint64_t)(uintptr_t)buf_ptr;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
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
        if (FD_TYPE_IS_PIPE(fd_table[fd].type)) {
            node = (vfs_node_t *)fd_table[fd].node;
            if (node && VFS_GET_TYPE(node->flags) == VFS_PIPE) {
                st->st_dev = 1;
                st->st_ino = node->inode ? node->inode : 1;
                st->st_mode = vfs_node_to_unix_mode(node);
                st->st_uid = node->uid;
                st->st_gid = node->gid;
                st->st_atim.tv_sec = node->atime;
                st->st_mtim.tv_sec = node->mtime;
                st->st_ctim.tv_sec = node->ctime;
            } else {
                st->st_mode = S_IFIFO | 0600;
            }
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
        
        node = (vfs_node_t *)fd_table[fd].node;
        if (node) {
            node_address = (uint64_t)node;
            if ((node_address & 0xFFFF0000u) == 0xFEFE0000u) {
                node = NULL;
            }
        }
        if (node) {
            st->st_dev = 1;
            st->st_ino = node->inode ? node->inode : 1;

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
            return 0;
        }
    }
    
    size = 0;
    flags = 0;
    ret = vfs_stat_fd(fd, &size, &flags);
    if (ret < 0) return -EBADF;
    st->st_dev = 1;
    st->st_ino = 1;
    if (VFS_GET_TYPE(flags) == VFS_DIRECTORY) st->st_mode = S_IFDIR | 0755;
    else st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511) / 512;
    return 0;
}

static int64_t sys_lseek_new(int fd, const char *offset_ptr, int whence) {
    int64_t offset;
    int64_t base;
    int64_t new_offset;
    vfs_node_t *node;
    task_fd_t *tfd;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (FD_TYPE_IS_PIPE(tfd->type)) return -ESPIPE;
    if (tfd->type == FD_TYPE_STDIN || tfd->type == FD_TYPE_STDOUT || tfd->type == FD_TYPE_STDERR) return -ESPIPE;
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;

    node = (vfs_node_t *)tfd->node;

    offset = (int64_t)(intptr_t)offset_ptr;
    switch (whence) {
        case VFS_SEEK_SET:
            base = 0;
            break;
        case VFS_SEEK_CUR:
            if (task_fd_position_get(tfd) > INT64_MAX) return -EOVERFLOW;
            base = (int64_t)task_fd_position_get(tfd);
            break;
        case VFS_SEEK_END:
            if (node->length > INT64_MAX) return -EOVERFLOW;
            base = (int64_t)node->length;
            break;
        default:
            return -EINVAL;
    }

    if (offset < -base) return -EINVAL;
    if (offset > INT64_MAX - base) return -EOVERFLOW;
    new_offset = base + offset;
    task_fd_position_set(tfd, (uint64_t)new_offset);
    return new_offset;
}

extern volatile uint64_t tick_count;
#define pit_ticks tick_count

static int sys_clock_gettime(int clock_id, const char *tp_ptr, int unused) {
    uint64_t tp_addr;
    struct kernel_timespec *ts;
    uint64_t nanoseconds;
    struct kernel_timespec value;
    
    (void)unused;
    tp_addr = (uint64_t)(uintptr_t)tp_ptr;
    if (!tp_addr || tp_addr >= KERNEL_VMA || tp_addr < 0x1000)
        return -EFAULT;
    if (timekeeping_get_ns(clock_id, &nanoseconds) != 0) return -EINVAL;
    ts = (struct kernel_timespec *)tp_addr;
    value.tv_sec = (long)(nanoseconds / 1000000000ULL);
    value.tv_nsec = (long)(nanoseconds % 1000000000ULL);
    if (copy_to_user(ts, &value, sizeof(value)) != 0) return -EFAULT;
    return 0;
}

static int sys_gettimeofday(int tv_ptr, const char *tz_ptr, int unused) {
    uint64_t tv_addr;
    struct kernel_timeval *tv;
    uint64_t nanoseconds;
    struct kernel_timeval value;
    
    (void)tz_ptr; (void)unused;
    tv_addr = (uint64_t)tv_ptr;
    if (!tv_addr || tv_addr >= KERNEL_VMA || tv_addr < 0x1000)
        return -EFAULT;
    tv = (struct kernel_timeval *)tv_addr;
    nanoseconds = timekeeping_realtime_ns();
    value.tv_sec = (long)(nanoseconds / 1000000000ULL);
    value.tv_usec = (long)((nanoseconds % 1000000000ULL) / 1000);
    if (copy_to_user(tv, &value, sizeof(value)) != 0) return -EFAULT;
    return 0;
}

static int timespec_to_ns(const struct kernel_timespec *value,
                          uint64_t *nanoseconds) {
    uint64_t seconds;

    if (!value || !nanoseconds) return -EFAULT;
    if (value->tv_sec < 0 || value->tv_nsec < 0 ||
        value->tv_nsec >= 1000000000L) return -EINVAL;
    seconds = (uint64_t)value->tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)value->tv_nsec) /
        1000000000ULL) return -EOVERFLOW;
    *nanoseconds = seconds * 1000000000ULL +
                   (uint64_t)value->tv_nsec;
    return 0;
}

static int sys_clock_settime(int clock_id, const char *tp_ptr, int unused) {
    struct kernel_timespec value;
    uint64_t nanoseconds;
    int result;

    (void)unused;
    if (clock_id != TIMEKEEPING_CLOCK_REALTIME) return -EINVAL;
    if (!current_task || !creds_has_capability(current_task, 25))
        return -EPERM;
    if (!tp_ptr) return -EFAULT;
    if (copy_from_user(&value, tp_ptr, sizeof(value)) != 0)
        return -EFAULT;
    result = timespec_to_ns(&value, &nanoseconds);
    if (result != 0) return result;
    if (timekeeping_set_realtime_ns(nanoseconds) != 0) return -EINVAL;
    return 0;
}

static int sys_settimeofday(int tv_ptr, const char *tz_ptr, int unused) {
    struct kernel_timeval value;
    uint64_t seconds;
    uint64_t nanoseconds;

    (void)unused;
    if (!current_task || !creds_has_capability(current_task, 25))
        return -EPERM;
    if (tz_ptr) return -EINVAL;
    if (!tv_ptr) return -EFAULT;
    if (copy_from_user(&value, (const void *)(uintptr_t)tv_ptr,
                       sizeof(value)) != 0) return -EFAULT;
    if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000L)
        return -EINVAL;
    seconds = (uint64_t)value.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)value.tv_usec * 1000ULL) /
        1000000000ULL) return -EOVERFLOW;
    nanoseconds = seconds * 1000000000ULL +
                  (uint64_t)value.tv_usec * 1000ULL;
    if (timekeeping_set_realtime_ns(nanoseconds) != 0) return -EINVAL;
    return 0;
}

static int sys_clock_getres(int clock_id, const char *tp_ptr, int unused) {
    struct kernel_timespec value;
    uint64_t resolution;

    (void)unused;
    if (timekeeping_get_ns(clock_id, &resolution) != 0) return -EINVAL;
    if (!tp_ptr) return 0;
    resolution = pit_freq ? (1000000000ULL + pit_freq - 1) / pit_freq :
                 1000000ULL;
    value.tv_sec = (long)(resolution / 1000000000ULL);
    value.tv_nsec = (long)(resolution % 1000000000ULL);
    if (copy_to_user((void *)tp_ptr, &value, sizeof(value)) != 0)
        return -EFAULT;
    return 0;
}

static int sys_clock_nanosleep(int clock_id, const char *flags_ptr,
                               int request_ptr, int remain_ptr) {
    struct kernel_timespec request;
    struct kernel_timespec remain;
    uint64_t requested_ns;
    uint64_t now_ns;
    uint64_t sleep_ns;
    uint64_t frequency;
    uint64_t ticks;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    int flags;
    int result;
    extern int task_has_pending_signals(void);

    flags = (int)(uintptr_t)flags_ptr;
    if (flags & ~1) return -EINVAL;
    if (timekeeping_get_ns(clock_id, &now_ns) != 0) return -EINVAL;
    if (!request_ptr) return -EFAULT;
    if (copy_from_user(&request, (const void *)(uintptr_t)request_ptr,
                       sizeof(request)) != 0) return -EFAULT;
    result = timespec_to_ns(&request, &requested_ns);
    if (result != 0) return result;
    if (flags & 1) {
        sleep_ns = requested_ns > now_ns ? requested_ns - now_ns : 0;
    } else {
        sleep_ns = requested_ns;
    }
    frequency = pit_freq ? pit_freq : 1000;
    if (sleep_ns > UINT64_MAX / frequency) return -EOVERFLOW;
    ticks = (sleep_ns * frequency + 999999999ULL) / 1000000000ULL;
    start_ns = timekeeping_monotonic_ns();
    if (ticks > 0) sleep_ticks(ticks);
    if (!task_has_pending_signals()) return 0;
    if (!(flags & 1) && remain_ptr) {
        now_ns = timekeeping_monotonic_ns();
        elapsed_ns = now_ns >= start_ns ? now_ns - start_ns : 0;
        sleep_ns = elapsed_ns < sleep_ns ? sleep_ns - elapsed_ns : 0;
        remain.tv_sec = (long)(sleep_ns / 1000000000ULL);
        remain.tv_nsec = (long)(sleep_ns % 1000000000ULL);
        if (copy_to_user((void *)(uintptr_t)remain_ptr, &remain,
                         sizeof(remain)) != 0) return -EFAULT;
    }
    return -EINTR;
}

static int sys_execve(int path_ptr, const char *argv_ptr, int envp_ptr) {
    char **argv;
    char **envp;
    char **new_argv;
    char *path;
    uint64_t path_addr;
    uint64_t argv_addr;
    uint64_t envp_addr;
    uint64_t size;
    uint64_t read_len;
    uint64_t header_size;
    uint64_t interp_size;
    int argc;
    int envc;
    int result;
    int ret;
    int i;
    int new_argc;
    int na;
    int kern_args;
    uint64_t exec_budget;
    uint64_t stack_limit;
    char shebang_interp[256];
    char shebang_arg[256];
    int shebang_has_arg;
    int shebang_line_end;
    int si;
    int sj;
    registers_t *regs;
    vfs_node_t *node;
    vfs_node_t *interp_node;
    uint8_t header[256];

    argv = NULL;
    envp = NULL;
    new_argv = NULL;
    path = NULL;
    node = NULL;
    interp_node = NULL;
    argc = 0;
    envc = 0;

    path_addr = (uint64_t)path_ptr;
    argv_addr = (uint64_t)argv_ptr;
    envp_addr = (uint64_t)envp_ptr;

    ret = posix_copy_user_string(&path, (const char *)path_addr, POSIX_EXEC_PATH_MAX);
    if (ret != 0) return ret;

    stack_limit = task_rlimit_get(current_task, 3, 0);
    exec_budget = USER_STACK_TOP - USER_STACK_FLOOR;
    if (stack_limit < exec_budget) exec_budget = stack_limit;
    ret = posix_copy_user_string_array(&argv, &argc, argv_addr,
                                       &exec_budget);
    if (ret != 0) {
        kfree(path);
        return ret;
    }

    if (argc == 0) {
        argv = (char **)kmalloc(2 * sizeof(char *));
        if (!argv) {
            kfree(path);
            return -ENOMEM;
        }
        argv[0] = posix_kstrdup(path);
        if (!argv[0]) {
            kfree(argv);
            kfree(path);
            return -ENOMEM;
        }
        argv[1] = NULL;
        argc = 1;
    }

    ret = posix_copy_user_string_array(&envp, &envc, envp_addr,
                                       &exec_budget);
    if (ret != 0) {
        posix_free_string_array(argv, argc);
        kfree(path);
        return ret;
    }

    node = vfs_namei(path);
    if (!node) {
        posix_free_string_array(envp, envc);
        posix_free_string_array(argv, argc);
        kfree(path);
        return -ENOENT;
    }
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_NOEXEC) {
        vfs_release(node);
        posix_free_string_array(envp, envc);
        posix_free_string_array(argv, argc);
        kfree(path);
        return -EACCES;
    }
    vfs_open(node, 0);
    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) {
        vfs_close(node);
        posix_free_string_array(envp, envc);
        posix_free_string_array(argv, argc);
        kfree(path);
        return -EACCES;
    }
    size = node->length;
    if (size == 0) {
        vfs_close(node);
        posix_free_string_array(envp, envc);
        posix_free_string_array(argv, argc);
        kfree(path);
        return -ENOEXEC;
    }

    header_size = size;
    if (header_size > sizeof(header)) {
        header_size = sizeof(header);
    }
    read_len = vfs_read(node, 0, header_size, header);
    if (read_len != header_size) {
        vfs_close(node);
        posix_free_string_array(envp, envc);
        posix_free_string_array(argv, argc);
        kfree(path);
        return -EIO;
    }

    if (header_size >= 2 && header[0] == '#' && header[1] == '!') {
        shebang_line_end = 2;
        while (shebang_line_end < (int)header_size && shebang_line_end < 256 &&
               header[shebang_line_end] != '\n' && header[shebang_line_end] != '\r') {
            shebang_line_end++;
        }

        si = 2;
        while (si < shebang_line_end && (header[si] == ' ' || header[si] == '\t')) si++;
        sj = 0;
        while (si < shebang_line_end && header[si] != ' ' && header[si] != '\t' &&
               sj < 255) {
            shebang_interp[sj++] = header[si++];
        }
        shebang_interp[sj] = '\0';
        if (sj == 0) {
            vfs_close(node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOEXEC;
        }

        shebang_has_arg = 0;
        while (si < shebang_line_end && (header[si] == ' ' || header[si] == '\t')) si++;
        if (si < shebang_line_end) {
            sj = 0;
            while (si < shebang_line_end && sj < 255) {
                shebang_arg[sj++] = header[si++];
            }
            while (sj > 0 && (shebang_arg[sj - 1] == ' ' || shebang_arg[sj - 1] == '\t')) sj--;
            shebang_arg[sj] = '\0';
            if (sj > 0) shebang_has_arg = 1;
        }

        vfs_close(node);
        node = NULL;

        interp_node = vfs_namei(shebang_interp);
        if (!interp_node) {
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOENT;
        }
        if (vfs_get_mount_flags_for_node(interp_node) & VFS_MS_NOEXEC) {
            vfs_release(interp_node);
            vfs_close(node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -EACCES;
        }
        vfs_open(interp_node, 0);
        interp_size = interp_node->length;
        if (interp_size == 0) {
            vfs_close(interp_node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOEXEC;
        }

        new_argc = 1 + shebang_has_arg + 1 + (argc > 1 ? argc - 1 : 0);
        new_argv = (char **)kmalloc((new_argc + 1) * sizeof(char *));
        if (!new_argv) {
            vfs_close(interp_node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOMEM;
        }
        memset(new_argv, 0, (new_argc + 1) * sizeof(char *));

        na = 0;
        new_argv[na] = posix_kstrdup(shebang_interp);
        if (!new_argv[na]) {
            kfree(new_argv);
            vfs_close(interp_node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOMEM;
        }
        na++;

        if (shebang_has_arg) {
            new_argv[na] = posix_kstrdup(shebang_arg);
            if (!new_argv[na]) {
                for (i = 0; i < na; i++) kfree(new_argv[i]);
                kfree(new_argv);
                vfs_close(interp_node);
                posix_free_string_array(envp, envc);
                posix_free_string_array(argv, argc);
                kfree(path);
                return -ENOMEM;
            }
            na++;
        }

        new_argv[na] = posix_kstrdup(path);
        if (!new_argv[na]) {
            for (i = 0; i < na; i++) kfree(new_argv[i]);
            kfree(new_argv);
            vfs_close(interp_node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOMEM;
        }
        na++;
        kern_args = na;

        for (i = 1; i < argc; i++) {
            new_argv[na] = argv[i];
            na++;
        }
        new_argv[na] = NULL;

        regs = current_task->syscall_frame;
        if (!regs) {
            for (i = 0; i < kern_args; i++) kfree(new_argv[i]);
            kfree(new_argv);
            vfs_close(interp_node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -EFAULT;
        }

        result = task_exec_node_with_owned_args(interp_node, regs, na,
                                                new_argv, envc, envp);
        vfs_close(interp_node);
        if (argv) {
            if (argv[0]) kfree(argv[0]);
            kfree(argv);
        }
        kfree(path);

        if (result == 0) {
            syscall_set_exec_completed();
            if (current_task) {
                current_task->exec_completed = 1;
            }
            __asm__ volatile ("" ::: "memory");
        }
        return result;
    }

    regs = current_task->syscall_frame;
    if (!regs) {
        vfs_close(node);
        posix_free_string_array(envp, envc);
        posix_free_string_array(argv, argc);
        kfree(path);
        return -EFAULT;
    }

    result = task_exec_node_with_owned_args(node, regs, argc, argv,
                                            envc, envp);
    vfs_close(node);
    kfree(path);

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
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

#define FILE_LOCK_POSIX 0
#define FILE_LOCK_FLOCK 1

typedef struct kernel_flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    pid_t l_pid;
} kernel_flock_t;

typedef struct file_lock_record {
    vfs_node_t *node;
    pid_t owner;
    uint64_t start;
    uint64_t end;
    int type;
    int kind;
    struct file_lock_record *next;
} file_lock_record_t;

static file_lock_record_t *file_locks;
static spinlock_t file_locks_lock = {0};

static int file_lock_overlaps(file_lock_record_t *lock, uint64_t start,
                              uint64_t end) {
    return lock->start < end && start < lock->end;
}

static file_lock_record_t *file_lock_conflict(vfs_node_t *node, pid_t owner,
                                               uint64_t start, uint64_t end,
                                               int type, int kind) {
    file_lock_record_t *lock;

    lock = file_locks;
    while (lock) {
        if (lock->node == node && lock->owner != owner && lock->kind == kind &&
            file_lock_overlaps(lock, start, end) &&
            (type == F_WRLCK || lock->type == F_WRLCK))
            return lock;
        lock = lock->next;
    }
    return NULL;
}

static file_lock_record_t *file_lock_detach_owned(vfs_node_t *node,
                                                   pid_t owner,
                                                   uint64_t start,
                                                   uint64_t end, int kind) {
    file_lock_record_t **link;
    file_lock_record_t *lock;
    file_lock_record_t *removed;

    removed = NULL;
    link = &file_locks;
    while (*link) {
        lock = *link;
        if (lock->node == node && lock->owner == owner &&
            (kind < 0 || lock->kind == kind) &&
            file_lock_overlaps(lock, start, end)) {
            *link = lock->next;
            lock->next = removed;
            removed = lock;
        } else {
            link = &lock->next;
        }
    }
    return removed;
}

static void file_lock_free_records(file_lock_record_t *records) {
    file_lock_record_t *next;

    while (records) {
        next = records->next;
        kfree(records);
        records = next;
    }
}

void file_locks_release_process_node(pid_t owner, vfs_node_t *node,
                                     int release_flock) {
    file_lock_record_t *removed;
    file_lock_record_t *flock_removed;

    if (!node) return;
    spin_lock(&file_locks_lock);
    removed = file_lock_detach_owned(node, owner, 0, UINT64_MAX,
                                     FILE_LOCK_POSIX);
    flock_removed = NULL;
    if (release_flock)
        flock_removed = file_lock_detach_owned(node, owner, 0, UINT64_MAX,
                                               FILE_LOCK_FLOCK);
    spin_unlock(&file_locks_lock);
    file_lock_free_records(removed);
    file_lock_free_records(flock_removed);
}

void file_locks_release_process(pid_t owner) {
    file_lock_record_t **link;
    file_lock_record_t *lock;
    file_lock_record_t *removed;

    removed = NULL;
    spin_lock(&file_locks_lock);
    link = &file_locks;
    while (*link) {
        lock = *link;
        if (lock->owner == owner) {
            *link = lock->next;
            lock->next = removed;
            removed = lock;
        } else {
            link = &lock->next;
        }
    }
    spin_unlock(&file_locks_lock);
    file_lock_free_records(removed);
}

static int file_lock_set(vfs_node_t *node, pid_t owner, uint64_t start,
                         uint64_t end, int type, int kind, int wait) {
    file_lock_record_t *record;
    file_lock_record_t *removed;
    file_lock_record_t *conflict;

    record = NULL;
    if (type != F_UNLCK) {
        record = (file_lock_record_t *)kmalloc(sizeof(file_lock_record_t));
        if (!record) return -ENOMEM;
        record->node = node;
        record->owner = owner;
        record->start = start;
        record->end = end;
        record->type = type;
        record->kind = kind;
        record->next = NULL;
    }

    for (;;) {
        removed = NULL;
        spin_lock(&file_locks_lock);
        conflict = type == F_UNLCK ? NULL :
            file_lock_conflict(node, owner, start, end, type, kind);
        if (!conflict) {
            removed = file_lock_detach_owned(node, owner, start, end, kind);
            if (record) {
                record->next = file_locks;
                file_locks = record;
                record = NULL;
            }
        }
        spin_unlock(&file_locks_lock);
        file_lock_free_records(removed);
        if (!conflict) return 0;
        if (!wait) {
            if (record) kfree(record);
            return -EAGAIN;
        }
        if (signal_pending_mask(current_task)) {
            if (record) kfree(record);
            return -EINTR;
        }
        sleep_ticks(1);
    }
}

static int file_lock_range(vfs_node_t *node, task_fd_t *tfd,
                           kernel_flock_t *flock, uint64_t *start,
                           uint64_t *end) {
    int64_t base;
    int64_t first;
    int64_t last;

    if (flock->l_whence == VFS_SEEK_SET) base = 0;
    else if (flock->l_whence == VFS_SEEK_CUR)
        base = (int64_t)task_fd_position_get(tfd);
    else if (flock->l_whence == VFS_SEEK_END)
        base = (int64_t)node->length;
    else return -EINVAL;
    if (__builtin_add_overflow(base, flock->l_start, &first) || first < 0)
        return -EINVAL;
    if (flock->l_len == 0) {
        *start = (uint64_t)first;
        *end = UINT64_MAX;
        return 0;
    }
    if (flock->l_len > 0) {
        if (__builtin_add_overflow(first, flock->l_len, &last) || last <= first)
            return -EINVAL;
        *start = (uint64_t)first;
        *end = (uint64_t)last;
        return 0;
    }
    if (__builtin_add_overflow(first, flock->l_len, &last) || last < 0)
        return -EINVAL;
    *start = (uint64_t)last;
    *end = (uint64_t)first;
    return *start < *end ? 0 : -EINVAL;
}

static int sys_fcntl(int fd, const char *cmd_ptr, int arg) {
    int cmd;
    int minfd;
    int newfd;
    uint64_t user_addr;
    uint64_t start;
    uint64_t end;
    kernel_flock_t flock;
    file_lock_record_t *conflict;
    vfs_node_t *node;
    int result;

    cmd = (int)(uintptr_t)cmd_ptr;
    if (!current_task) return -ESRCH;
    if (is_socket_fd(fd)) return socket_fcntl(fd, cmd, arg);
    if (fd < 0 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
            minfd = arg;
            if (minfd < 0) return -EINVAL;
            newfd = fd_alloc_from(minfd);
            if (newfd < 0) return -EMFILE;
            memcpy(&fd_table[newfd], &fd_table[fd], sizeof(task_fd_t));
            fd_table[newfd].ref_count = 1;
            if (cmd == F_DUPFD_CLOEXEC)
                fd_table[newfd].flags |= 1;
            else
                fd_table[newfd].flags &= ~1;
            fd_retain_entry(&fd_table[newfd]);
            task_fd_position_share(&fd_table[fd], &fd_table[newfd]);
            return newfd;
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
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            if (fd_table[fd].type != FD_TYPE_FILE || !fd_table[fd].node)
                return -EBADF;
            user_addr = (uint64_t)(uint32_t)arg;
            if (!posix_user_range_mapped(user_addr, sizeof(flock)))
                return -EFAULT;
            memcpy(&flock, (void *)user_addr, sizeof(flock));
            if (flock.l_type != F_RDLCK && flock.l_type != F_WRLCK &&
                flock.l_type != F_UNLCK)
                return -EINVAL;
            node = (vfs_node_t *)fd_table[fd].node;
            result = file_lock_range(node, &fd_table[fd], &flock,
                                     &start, &end);
            if (result != 0) return result;
            if (cmd == F_GETLK) {
                spin_lock(&file_locks_lock);
                conflict = file_lock_conflict(node, current_task->pid, start,
                                              end, flock.l_type,
                                              FILE_LOCK_POSIX);
                if (conflict) {
                    flock.l_type = (int16_t)conflict->type;
                    flock.l_whence = VFS_SEEK_SET;
                    flock.l_start = (int64_t)conflict->start;
                    flock.l_len = conflict->end == UINT64_MAX ? 0 :
                        (int64_t)(conflict->end - conflict->start);
                    flock.l_pid = conflict->owner;
                } else {
                    flock.l_type = F_UNLCK;
                }
                spin_unlock(&file_locks_lock);
                memcpy((void *)user_addr, &flock, sizeof(flock));
                return 0;
            }
            return file_lock_set(node, current_task->pid, start, end,
                                 flock.l_type, FILE_LOCK_POSIX,
                                 cmd == F_SETLKW);
        default:
            return -EINVAL;
    }
}

static int sys_truncate(int path_ptr, const char *len_ptr, int unused) {
    uint64_t path_addr;
    uint64_t length;
    const char *path;
    vfs_node_t *node;
    int result;

    (void)unused;
    path_addr = (uint64_t)path_ptr;
    length = (uint64_t)(uintptr_t)len_ptr;
    
    if (!path_addr || path_addr >= KERNEL_VMA || path_addr < 0x1000) return -1;
    
    path = (const char *)path_addr;
    node = vfs_namei(path);
    if (!node) return -1;

    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) { vfs_release(node); return -1; }
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) {
        vfs_release(node);
        return -EROFS;
    }
    
    if (node->truncate) {
        result = node->truncate(node, length);
        vfs_release(node);
        return result;
    }
    
    vfs_release(node);
    return -1;
}

static int sys_ftruncate(int fd, const char *len_ptr, int unused) {
    uint64_t length;
    vfs_node_t *node;

    (void)unused;
    length = (uint64_t)(uintptr_t)len_ptr;
    
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    
    node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;

    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -EISDIR;
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) return -EROFS;
    
    if (node->truncate) {
        return node->truncate(node, length);
    }
    
    return -ENOSYS;
}

static int sys_umask(int mask, const char *unused1, int unused2) {
    int old;

    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    old = (int)current_task->creation_mask;
    current_task->creation_mask = (uint64_t)(mask & 0777);
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
    uint64_t dirp_addr;
    uint8_t *buf;
    int written;
    uint64_t dir_offset;
    int name_len;
    int reclen;
    struct linux_dirent *de;
    int i;
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t local_copy;

    dirp_addr = (uint64_t)(uintptr_t)dirp_ptr;
    if (!current_task) return -ESRCH;
    if (!dirp_addr || dirp_addr >= KERNEL_VMA || dirp_addr < 0x1000) return -EFAULT;
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
    dir_offset = task_fd_position_get(tfd);

    while (written < count) {
        if (vfs_readdir_copy(node, dir_offset, &local_copy) != 0) break;

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
    }

    task_fd_position_set(tfd, dir_offset);
    return written;
}

static int sys_rename(int oldpath_ptr, const char *newpath_ptr, int unused) {
    uint64_t old_addr = (uint64_t)oldpath_ptr;
    uint64_t new_addr = (uint64_t)(uintptr_t)newpath_ptr;
    const char *oldpath;
    const char *newpath;
    char new_parent_path[256];
    char new_name[64];
    int len;
    int last_slash;
    char old_name[64];
    int old_len;
    int old_last_slash;
    int k;
    (void)unused;
    
    if (!old_addr || old_addr >= KERNEL_VMA || old_addr < 0x1000) return -1;
    if (!new_addr || new_addr >= KERNEL_VMA || new_addr < 0x1000) return -1;
    
    oldpath = (const char *)old_addr;
    newpath = (const char *)new_addr;
    
    vfs_node_t *old_node = vfs_namei(oldpath);
    if (!old_node) return -1;
    
    vfs_node_t *old_parent = old_node->parent;
    if (!old_parent || !old_parent->rename) { vfs_release(old_node); return -1; }
    
    len = 0;
    while (newpath[len]) len++;
    last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (newpath[i] == '/') last_slash = i;
    }
    
    vfs_node_t *new_parent;
    if (last_slash <= 0) {
        int j;
        int k;
        new_parent_path[0] = '/';
        new_parent_path[1] = '\0';
        j = (last_slash == 0) ? 1 : 0;
        k = 0;
        while (newpath[j] && k < 63) new_name[k++] = newpath[j++];
        new_name[k] = '\0';
    } else {
        int k;
        for (int i = 0; i < last_slash && i < 255; i++) new_parent_path[i] = newpath[i];
        new_parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        k = 0;
        for (int i = last_slash + 1; i < len && k < 63; i++) new_name[k++] = newpath[i];
        new_name[k] = '\0';
    }
    
    new_parent = vfs_namei(new_parent_path);
    if (!new_parent) { vfs_release(old_node); return -1; }
    
    old_len = 0;
    while (oldpath[old_len]) old_len++;
    old_last_slash = -1;
    for (int i = 0; i < old_len; i++) {
        if (oldpath[i] == '/') old_last_slash = i;
    }
    k = 0;
    for (int i = old_last_slash + 1; i < old_len && k < 63; i++) old_name[k++] = oldpath[i];
    old_name[k] = '\0';
    
    {
        int ret;
        ret = old_parent->rename(old_parent, old_name, new_parent, new_name);
        vfs_release(old_node);
        vfs_release(new_parent);
        return ret;
    }
}

static int sys_link(int oldpath_ptr, const char *newpath_ptr, int unused) {
    char *oldpath;
    char *newpath;
    int result;

    (void)unused;
    result = posix_copy_user_string(&oldpath,
                                    (const char *)(uintptr_t)oldpath_ptr,
                                    VFS_MAX_PATH);
    if (result < 0) return result;
    result = posix_copy_user_string(&newpath, newpath_ptr, VFS_MAX_PATH);
    if (result < 0) {
        kfree(oldpath);
        return result;
    }
    result = ext4_vfs_link_node(oldpath, newpath);
    kfree(newpath);
    kfree(oldpath);
    return result == 0 ? 0 : -EIO;
}

static int posix_vfs_symlink(const char *target, const char *linkpath, uint64_t flags) {
    if (ext4_vfs_symlink_node(target, linkpath, flags) == 0) {
        return 0;
    }
    return -ENOSYS;
}

static int sys_symlink(int target_ptr, const char *linkpath_ptr, int unused) {
    const char *target;
    const char *linkpath;
    char *target_copy;
    char *link_copy;
    int ret;

    (void)unused;
    target = (const char *)(uintptr_t)target_ptr;
    linkpath = (const char *)(uintptr_t)linkpath_ptr;

    ret = posix_copy_user_string(&target_copy, target, VFS_MAX_PATH);
    if (ret < 0) return ret;
    ret = posix_copy_user_string(&link_copy, linkpath, VFS_MAX_PATH);
    if (ret < 0) {
        kfree(target_copy);
        return ret;
    }

    ret = posix_vfs_symlink(target_copy, link_copy, (uint64_t)unused);
    if (ret == -ENOSYS) {
        ret = ramfs_create_symlink(link_copy, target_copy, 0777);
        if (ret == 0) {
            kfree(target_copy);
            kfree(link_copy);
            return 0;
        }
        if (ret == RAMFS_ERR_EXIST) ret = -EEXIST;
        else if (ret == RAMFS_ERR_NOENT) ret = -ENOENT;
        else if (ret == RAMFS_ERR_NOSPC) ret = -ENOSPC;
        else if (ret == RAMFS_ERR_NOMEM) ret = -ENOMEM;
        else ret = -EIO;
    }

    kfree(target_copy);
    kfree(link_copy);
    if (ret == 0) return 0;
    if (ret == -EEXIST || ret == -ENOENT || ret == -ENOSPC || ret == -ENOMEM || ret == -EINVAL || ret == -ENOTDIR) return ret;
    return -EIO;
}

static int sys_readlink(int path_ptr, const char *buf_ptr, int bufsiz) {
    uint64_t path_addr = (uint64_t)path_ptr;
    uint64_t buf_addr = (uint64_t)(uintptr_t)buf_ptr;

    char target[VFS_MAX_PATH];
    uint64_t n;
    uint64_t copy_len;
    if (!current_task) return -ESRCH;
    if (bufsiz <= 0) return -EINVAL;

    if (!path_addr || path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint64_t)bufsiz >= KERNEL_VMA) return -EFAULT;

    vfs_node_t *node = vfs_namei_nofollow((const char *)path_addr);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) { vfs_release(node); return -EINVAL; }

    n = vfs_read(node, 0, sizeof(target) - 1, (uint8_t *)target);
    vfs_release(node);
    if (n >= sizeof(target)) n = sizeof(target) - 1;
    target[n] = '\0';

    copy_len = n;
    if (copy_len > (uint64_t)bufsiz) copy_len = (uint64_t)bufsiz;

    for (uint64_t i = 0; i < copy_len; i++) {
        ((char *)buf_addr)[i] = target[i];
    }

    return (int)copy_len;
}

static int sys_dup3(int oldfd, int newfd, int flags) {
    int ret;

    if (!current_task) return -ESRCH;
    if (flags & ~VFS_O_CLOEXEC) return -EINVAL;
    if (oldfd < 0 || oldfd >= current_task->fds_capacity || !fd_table[oldfd].in_use) return -EBADF;
    if (newfd < 0) return -EBADF;
    if (newfd >= current_task->fds_capacity) {
        ret = task_fd_ensure_capacity(current_task, newfd);
        if (ret != 0) return -EMFILE;
    }
    if (oldfd == newfd) return -EINVAL;
    if (fd_table[newfd].in_use) {
        fd_release_entry(&fd_table[newfd]);
    }
    memcpy(&fd_table[newfd], &fd_table[oldfd], sizeof(task_fd_t));
    fd_table[newfd].ref_count = 1;
    if (flags & VFS_O_CLOEXEC)
        fd_table[newfd].flags |= 1;
    else
        fd_table[newfd].flags &= ~1;
    fd_retain_entry(&fd_table[newfd]);
    task_fd_position_share(&fd_table[oldfd], &fd_table[newfd]);
    return newfd;
}

static int sys_pipe2(int *pipefd, int flags) {
    uint64_t addr;
    int rfd;
    int wfd;
    pipe_t *p;

    if (!current_task) return -ESRCH;
    if (flags & ~(VFS_O_CLOEXEC | VFS_O_NONBLOCK)) return -EINVAL;
    if (!pipefd) return -EFAULT;
    addr = (uint64_t)pipefd;
    if (addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(pipe_t));
    waitq_init(&p->read_waitq);
    waitq_init(&p->write_waitq);
    spinlock_init(&p->lock);
    p->readers = 1;
    p->writers = 1;
    rfd = fd_alloc();
    if (rfd < 0) { kfree(p); return -EMFILE; }
    wfd = fd_alloc();
    if (wfd < 0) { fd_table[rfd].in_use = 0; kfree(p); return -EMFILE; }
    fd_table[rfd].type = FD_TYPE_PIPE_R;
    fd_table[rfd].private_data = p;
    fd_table[wfd].type = FD_TYPE_PIPE_W;
    fd_table[wfd].private_data = p;
    if (flags & VFS_O_CLOEXEC) {
        fd_table[rfd].flags |= 1;
        fd_table[wfd].flags |= 1;
    }
    if (flags & VFS_O_NONBLOCK) {
        fd_table[rfd].flags |= VFS_O_NONBLOCK;
        fd_table[wfd].flags |= VFS_O_NONBLOCK;
    }
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

static int sys_fchdir(int fd) {
    char *resolved;
    char resolved_path[VFS_MAX_PATH];
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;
    resolved = vfs_get_path(node, resolved_path, sizeof(resolved_path));
    if (!resolved) return -ENOENT;
    return task_set_cwd(current_task, resolved) == 0 ? 0 : -ENOMEM;
}

static int sys_fchmod(int fd, int mode) {
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;

    if (current_task->euid != 0 && current_task->euid != node->uid)
        return -EPERM;
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) return -EROFS;

    if (node->chmod) {
        return node->chmod(node, mode & 07777);
    }
    node->mask = mode & 07777;
    return 0;
}

static int sys_fchown(int fd, int uid, int gid) {
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;

    if (current_task->euid != 0)
        return -EPERM;
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) return -EROFS;

    if (node->chown) {
        return node->chown(node, uid, gid);
    }
    if (uid != -1) node->uid = uid;
    if (gid != -1) node->gid = gid;
    return 0;
}

static int sys_fsync(int fd) {
    task_fd_t *tfd;
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!fd_table[fd].in_use) return -EBADF;
    tfd = &fd_table[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EINVAL;
    node = (vfs_node_t *)tfd->node;
    return vfs_sync_node(node, 0) == 0 ? 0 : -EIO;
}

static int sys_fdatasync(int fd) {
    task_fd_t *tfd;
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!fd_table[fd].in_use) return -EBADF;
    tfd = &fd_table[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EINVAL;
    node = (vfs_node_t *)tfd->node;
    return vfs_sync_node(node, 1) == 0 ? 0 : -EIO;
}

static int sys_sync(int unused1, const char *unused2, int unused3) {
    int result;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    result = vfs_sync_all(0);
    return result == 0 ? 0 : -EIO;
}

static int sys_syncfs(int fd, const char *unused1, int unused2) {
    task_fd_t *tfd;
    vfs_node_t *node;

    (void)unused1;
    (void)unused2;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!fd_table[fd].in_use) return -EBADF;
    tfd = &fd_table[fd];
    if ((tfd->type != FD_TYPE_FILE && !FD_TYPE_IS_PIPE(tfd->type)) ||
        !tfd->node) return -EINVAL;
    node = (vfs_node_t *)tfd->node;
    return vfs_sync_node(node, 0) == 0 ? 0 : -EIO;
}

static int sys_flock(int fd, int operation) {
    int mode;
    int type;
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!fd_table[fd].in_use) return -EBADF;
    if (fd_table[fd].type != FD_TYPE_FILE || !fd_table[fd].node)
        return -EBADF;
    if (operation & ~(1 | 2 | 4 | 8)) return -EINVAL;
    mode = operation & ~4;
    if (mode != 1 && mode != 2 && mode != 8) return -EINVAL;
    type = mode == 1 ? F_RDLCK : mode == 2 ? F_WRLCK : F_UNLCK;
    node = (vfs_node_t *)fd_table[fd].node;
    return file_lock_set(node, current_task->pid, 0, UINT64_MAX, type,
                         FILE_LOCK_FLOCK, !(operation & 4));
}

static int sys_pread64(int fd, void *buf, size_t count, long long offset) {
    vfs_node_t *node;

    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    if (!buf) return -EFAULT;
    if (offset < 0) return -EINVAL;
    node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -EISDIR;
    return vfs_read(node, (uint64_t)offset, count, buf);
}

static int sys_pwrite64(int fd, const void *buf, size_t count, long long offset) {
    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    if (!buf) return -EFAULT;
    vfs_node_t *node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) == VFS_DIRECTORY) return -EISDIR;
    return vfs_write(node, (uint64_t)offset, count, (uint8_t *)buf);
}

struct iovec {
    void *iov_base;
    size_t iov_len;
};

static int sys_readv(int fd, const struct iovec *iov, int iovcnt) {
    uint64_t iov_addr;
    uint64_t iov_len;
    uint64_t base;
    uint64_t len;
    uint64_t vector_len;
    struct iovec local_iov;
    int total;
    int ret;
    int i;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!iov || iovcnt <= 0) return -EINVAL;
    iov_addr = (uint64_t)iov;
    iov_len = (uint64_t)iovcnt * sizeof(struct iovec);
    if (iov_addr >= KERNEL_VMA || iov_addr < 0x1000) return -EFAULT;
    if (iov_addr + iov_len < iov_addr || iov_addr + iov_len >= KERNEL_VMA) return -EFAULT;
    if (!posix_user_range_mapped(iov_addr, iov_len)) return -EFAULT;

    total = 0;
    for (i = 0; i < iovcnt; i++) {
        if (copy_from_user(&local_iov,
                (const void *)(uintptr_t)(iov_addr +
                (uint64_t)i * sizeof(struct iovec)), sizeof(local_iov)) < 0)
            return total > 0 ? total : -EFAULT;
        if (local_iov.iov_len == 0) continue;
        base = (uint64_t)local_iov.iov_base;
        vector_len = (uint64_t)local_iov.iov_len;
        len = vector_len;
        if (len > 0x7FFFFFFFULL)
            return total > 0 ? total : -EINVAL;
        if (!base || base >= KERNEL_VMA || base < 0x1000)
            return total > 0 ? total : -EFAULT;
        if (base + len < base || base + len >= KERNEL_VMA)
            return total > 0 ? total : -EFAULT;
        if (!posix_user_range_mapped(base, len))
            return total > 0 ? total : -EFAULT;
        if (len > (uint64_t)(0x7FFFFFFF - total))
            len = (uint64_t)(0x7FFFFFFF - total);
        if (len == 0) return total;

        ret = syscall_core_read_for_readv(fd, local_iov.iov_base, (int)len);
        if (ret < 0) {
            if (total > 0) return total;
            return ret;
        }
        if (ret == 0) break;
        total += ret;
        if ((uint64_t)ret < len || len < vector_len) break;
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
    uint64_t dirp_addr;
    uint8_t *buf;
    int written;
    uint64_t dir_offset;
    int name_len;
    int reclen;
    struct linux_dirent64 *de;
    int i;
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t local_copy;

    dirp_addr = (uint64_t)dirp;
    if (!current_task) return -ESRCH;
    if (!dirp_addr || dirp_addr >= KERNEL_VMA || dirp_addr < 0x1000) return -EFAULT;
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
    dir_offset = task_fd_position_get(tfd);
    
    while ((unsigned int)written < count) {
        if (vfs_readdir_copy(node, dir_offset, &local_copy) != 0) break;
        
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
    }
    
    task_fd_position_set(tfd, dir_offset);
    return written;
}

void syscalls_posix_init(void) {
    syscall_table_set(SYSCALL_DUP, (void *)(sys_dup));
    syscall_table_set(SYSCALL_DUP2, (void *)(sys_dup2));
    syscall_table_set(SYSCALL_DUP3, (void *)(sys_dup3));
    syscall_table_set(SYSCALL_PIPE, (void *)(sys_pipe));
    syscall_table_set(SYSCALL_PIPE2, (void *)(sys_pipe2));
    syscall_table_set(SYSCALL_STAT, (void *)(sys_stat));
    syscall_table_set(SYSCALL_FSTAT, (void *)(sys_fstat));
    syscall_table_set(SYSCALL_GETCWD, (void *)(sys_getcwd));
    syscall_table_set(SYSCALL_CHDIR, (void *)(sys_chdir));
    syscall_table_set(SYSCALL_CHROOT, (void *)(sys_chroot));
    syscall_table_set(SYSCALL_PIVOT_ROOT, (void *)(sys_pivot_root));
    syscall_table_set(SYSCALL_FCHDIR, (void *)(sys_fchdir));
    syscall_table_set(SYSCALL_ACCESS, (void *)(sys_access));
    syscall_table_set(SYSCALL_CLOCK_GETTIME, (void *)(sys_clock_gettime));
    syscall_table_set(SYSCALL_CLOCK_SETTIME, (void *)(sys_clock_settime));
    syscall_table_set(SYSCALL_SETTIMEOFDAY, (void *)(sys_settimeofday));
    syscall_table_set(SYSCALL_CLOCK_GETRES, (void *)(sys_clock_getres));
    syscall_table_set(SYSCALL_CLOCK_NANOSLEEP, (void *)(sys_clock_nanosleep));
    syscall_table_set(SYSCALL_GETTIMEOFDAY, (void *)(sys_gettimeofday));
    syscall_table_set(SYSCALL_EXECVE, (void *)(sys_execve));
    syscall_table_set(SYSCALL_LSEEK, (void *)(sys_lseek_new));
    syscall_table_set(SYSCALL_FCNTL, (void *)(sys_fcntl));
    syscall_table_set(SYSCALL_TRUNCATE, (void *)(sys_truncate));
    syscall_table_set(SYSCALL_FTRUNCATE, (void *)(sys_ftruncate));
    syscall_table_set(SYSCALL_UMASK, (void *)(sys_umask));
    syscall_table_set(SYSCALL_RENAME, (void *)(sys_rename));
    syscall_table_set(SYSCALL_LINK, (void *)(sys_link));
    syscall_table_set(SYSCALL_SYMLINK, (void *)(sys_symlink));
    syscall_table_set(SYSCALL_READLINK, (void *)(sys_readlink));
    syscall_table_set(SYSCALL_GETDENTS, (void *)(sys_getdents));
    syscall_table_set(SYSCALL_GETDENTS64, (void *)(sys_getdents64));
    syscall_table_set(SYSCALL_FCHMOD, (void *)(sys_fchmod));
    syscall_table_set(SYSCALL_FCHOWN, (void *)(sys_fchown));
    syscall_table_set(SYSCALL_FSYNC, (void *)(sys_fsync));
    syscall_table_set(SYSCALL_FDATASYNC, (void *)(sys_fdatasync));
    syscall_table_set(SYSCALL_SYNC, (void *)(sys_sync));
    syscall_table_set(SYSCALL_SYNCFS, (void *)(sys_syncfs));
    syscall_table_set(SYSCALL_FLOCK, (void *)(sys_flock));
    syscall_table_set(SYSCALL_PREAD64, (void *)(sys_pread64));
    syscall_table_set(SYSCALL_PWRITE64, (void *)(sys_pwrite64));
    syscall_table_set(SYSCALL_READV, (void *)(sys_readv));
}

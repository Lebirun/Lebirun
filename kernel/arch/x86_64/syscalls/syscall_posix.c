#include "syscall_defs.h"
#include <lebirun/pit.h>
#include <lebirun/pty.h>
#include <lebirun/common.h>
#include <lebirun/ramfs.h>
#include <lebirun/spinlock.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/timekeeping.h>
#include <lebirun/creds.h>
#include <lebirun/mem_map.h>
#include <lebirun/overlayfs.h>
#include <lebirun/squashfs.h>
#include <string.h>

extern int is_socket_fd(int fd);
extern int socket_fcntl(int fd, int cmd, int arg);
extern void socket_retain_task_fd(task_fd_t *descriptor);
extern void socket_release_task_fd(task_fd_t *descriptor);
extern int syscall_core_read_for_readv(int fd, char *buf, int len);
void file_locks_release_process_node(pid_t owner, vfs_node_t *node,
                                     int release_flock);

#define fd_table (current_task->fds)
#define POSIX_AT_FDCWD -100

#define COPY_TRANSFER_SCRATCH_SIZE 16384

typedef struct {
    task_t *owner;
    vfs_node_t *mount_root;
    uint64_t physical;
    uint64_t pages;
    uint32_t active;
    int release_pending;
    squashfs_transfer_cache_t *cache;
} copy_transfer_session_t;

static copy_transfer_session_t copy_transfer_session;
static mutex_t copy_transfer_lock;

static void copy_transfer_release_locked(void) {
    if (copy_transfer_session.physical)
        pfa_free_contiguous(copy_transfer_session.physical,
                            copy_transfer_session.pages);
    memset(&copy_transfer_session, 0, sizeof(copy_transfer_session));
}

static copy_transfer_session_t *copy_transfer_acquire(task_t *owner,
                                                       vfs_node_t *output,
                                                       uint64_t data_size) {
    vfs_mount_t *mount;
    uint64_t total_size;
    uint64_t pages;
    uint64_t physical;
    uint8_t *memory;

    if (!owner || !output || data_size == 0) return NULL;
    mount = vfs_get_mount_for_node(output);
    if (!mount || !mount->root || !mount->fs_type ||
        !mount->fs_type->name || strcmp(mount->fs_type->name, "ext4") != 0)
        return NULL;
    mutex_lock(&copy_transfer_lock);
    if (copy_transfer_session.physical) {
        if (copy_transfer_session.owner == owner &&
            copy_transfer_session.mount_root == mount->root &&
            copy_transfer_session.cache &&
            copy_transfer_session.cache->data_capacity >= data_size &&
            !copy_transfer_session.release_pending) {
            copy_transfer_session.active++;
            mutex_unlock(&copy_transfer_lock);
            return &copy_transfer_session;
        }
        if (copy_transfer_session.owner != owner) {
            mutex_unlock(&copy_transfer_lock);
            return NULL;
        }
        copy_transfer_release_locked();
    }
    if (data_size > UINT64_MAX - COPY_TRANSFER_SCRATCH_SIZE) {
        mutex_unlock(&copy_transfer_lock);
        return NULL;
    }
    total_size = data_size + COPY_TRANSFER_SCRATCH_SIZE;
    pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    physical = pfa_alloc_contiguous(pages);
    if (!physical) {
        mutex_unlock(&copy_transfer_lock);
        return NULL;
    }
    memory = (uint8_t *)(uintptr_t)(physical + KERNEL_VMA);
    memset(memory, 0, pages * PAGE_SIZE);
    copy_transfer_session.owner = owner;
    copy_transfer_session.mount_root = mount->root;
    copy_transfer_session.physical = physical;
    copy_transfer_session.pages = pages;
    copy_transfer_session.cache =
        (squashfs_transfer_cache_t *)(memory + data_size);
    copy_transfer_session.cache->data = memory;
    copy_transfer_session.cache->data_capacity = data_size;
    copy_transfer_session.cache->scratch = memory + data_size +
                                            sizeof(*copy_transfer_session.cache);
    copy_transfer_session.cache->scratch_capacity =
        pages * PAGE_SIZE - data_size - sizeof(*copy_transfer_session.cache);
    copy_transfer_session.active = 1;
    mutex_unlock(&copy_transfer_lock);
    return &copy_transfer_session;
}

static void copy_transfer_finish(copy_transfer_session_t *session) {
    mutex_lock(&copy_transfer_lock);
    if (session == &copy_transfer_session && copy_transfer_session.active) {
        copy_transfer_session.active--;
        if (copy_transfer_session.active == 0 &&
            copy_transfer_session.release_pending)
            copy_transfer_release_locked();
    }
    mutex_unlock(&copy_transfer_lock);
}

void copy_file_range_release_task(void *owner) {
    mutex_lock(&copy_transfer_lock);
    if (copy_transfer_session.owner == owner) {
        if (copy_transfer_session.active)
            copy_transfer_session.release_pending = 1;
        else
            copy_transfer_release_locked();
    }
    mutex_unlock(&copy_transfer_lock);
}

void copy_file_range_release_mount(vfs_node_t *root) {
    mutex_lock(&copy_transfer_lock);
    if (copy_transfer_session.mount_root == root) {
        if (copy_transfer_session.active)
            copy_transfer_session.release_pending = 1;
        else
            copy_transfer_release_locked();
    }
    mutex_unlock(&copy_transfer_lock);
}

#define posix_user_range_mapped(addr, size) \
    syscall_user_range_mapped((addr), (size), 0)

static int posix_user_read_u64(uint64_t addr, uint64_t *out) {
    if (!out) return -EFAULT;
    if (copy_from_user(out, (const void *)(uintptr_t)addr,
                       sizeof(uint64_t)) != 0)
        return -EFAULT;
    return 0;
}

static int posix_copy_user_string(char **out, const char *src_user, uint64_t max_len) {
    char *dst;
    char scratch[64];
    uint64_t addr;
    uint64_t cur;
    uint64_t chunk;
    uint64_t page_remaining;
    uint64_t i;
    uint64_t j;
    uint64_t alloc_len;
    uint64_t address_limit;

    if (!out || !src_user || max_len == 0) return -EFAULT;
    *out = NULL;
    addr = (uint64_t)src_user;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return -EFAULT;
    address_limit = KERNEL_VMA - addr;
    if (max_len > address_limit) max_len = address_limit;
    i = 0;
    while (i < max_len) {
        cur = addr + i;
        chunk = max_len - i;
        page_remaining = PAGE_SIZE - (cur & (PAGE_SIZE - 1));
        if (chunk > page_remaining) chunk = page_remaining;
        if (chunk > sizeof(scratch)) chunk = sizeof(scratch);
        if (copy_from_user(scratch, (const void *)(uintptr_t)cur,
                           (size_t)chunk) != 0)
            return -EFAULT;
        for (j = 0; j < chunk; j++) {
            if (scratch[j] == '\0') {
                alloc_len = i + j + 1;
                dst = (char *)kmalloc(alloc_len);
                if (!dst) return -ENOMEM;
                if (copy_from_user(dst, src_user, (size_t)alloc_len) != 0) {
                    kfree(dst);
                    return -EFAULT;
                }
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
    size_t len;

    if (!src) return NULL;
    len = strlen(src);
    dst = (char *)kmalloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len + 1);
    return dst;
}

static int posix_read_shebang(vfs_node_t *node, uint64_t size,
                              char **interp_out, char **arg_out) {
    uint8_t *line;
    uint8_t *new_line;
    uint64_t capacity;
    uint64_t read_len;
    uint64_t line_end;
    uint64_t i;
    uint64_t start;
    uint64_t interp_len;
    uint64_t arg_len;
    char *interp;
    char *arg;

    *interp_out = NULL;
    *arg_out = NULL;
    capacity = size < 256 ? size : 256;
    if (capacity < 2) return -ENOEXEC;
    line = (uint8_t *)kmalloc((size_t)capacity + 1);
    if (!line) return -ENOMEM;

    for (;;) {
        read_len = vfs_read(node, 0, capacity, line);
        if (read_len != capacity) {
            kfree(line);
            return -EIO;
        }
        line_end = 2;
        while (line_end < read_len && line[line_end] != '\n' &&
               line[line_end] != '\r') line_end++;
        if (line_end < read_len || read_len == size) break;
        if (capacity > SIZE_MAX / 2) {
            kfree(line);
            return -ENOMEM;
        }
        read_len = capacity * 2;
        if (read_len > size) read_len = size;
        new_line = (uint8_t *)kmalloc((size_t)read_len + 1);
        if (!new_line) {
            kfree(line);
            return -ENOMEM;
        }
        kfree(line);
        line = new_line;
        capacity = read_len;
    }

    i = 2;
    while (i < line_end && (line[i] == ' ' || line[i] == '\t')) i++;
    start = i;
    while (i < line_end && line[i] != ' ' && line[i] != '\t') i++;
    interp_len = i - start;
    if (interp_len == 0) {
        kfree(line);
        return -ENOEXEC;
    }
    interp = (char *)kmalloc((size_t)interp_len + 1);
    if (!interp) {
        kfree(line);
        return -ENOMEM;
    }
    memcpy(interp, line + start, (size_t)interp_len);
    interp[interp_len] = '\0';

    while (i < line_end && (line[i] == ' ' || line[i] == '\t')) i++;
    start = i;
    while (line_end > start &&
           (line[line_end - 1] == ' ' || line[line_end - 1] == '\t')) {
        line_end--;
    }
    arg_len = line_end - start;
    arg = NULL;
    if (arg_len) {
        arg = (char *)kmalloc((size_t)arg_len + 1);
        if (!arg) {
            kfree(interp);
            kfree(line);
            return -ENOMEM;
        }
        memcpy(arg, line + start, (size_t)arg_len);
        arg[arg_len] = '\0';
    }
    kfree(line);
    *interp_out = interp;
    *arg_out = arg;
    return 0;
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
    int pty_endpoint;
    int pty_type;

    if (!tfd || !tfd->in_use) return;

    if (tfd->type == FD_TYPE_SOCKET) {
        socket_release_task_fd(tfd);
        return;
    }

    node_to_close = NULL;
    pipe_to_release = NULL;
    pipe_type = 0;
    release_flock = 1;
    pty_endpoint = -1;
    pty_type = 0;

    if (FD_TYPE_IS_PIPE(tfd->type)) {
        pipe_to_release = (pipe_t *)tfd->private_data;
        pipe_type = tfd->type;
    } else if (FD_TYPE_IS_PTY(tfd->type)) {
        pty_endpoint = (int)(uintptr_t)tfd->private_data;
        pty_type = tfd->type;
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
    if (pty_endpoint >= 0) {
        if (pty_type == FD_TYPE_PTY_MASTER)
            pty_close_master(pty_endpoint);
        else
            pty_close_slave(pty_endpoint);
    }
}

static void fd_retain_entry(task_fd_t *tfd) {
    pipe_t *p;
    vfs_node_t *node;

    if (!tfd || !tfd->in_use) return;
    if (tfd->type == FD_TYPE_SOCKET) {
        socket_retain_task_fd(tfd);
        return;
    }
    if (FD_TYPE_IS_PTY(tfd->type)) {
        pty_retain_endpoint((int)(uintptr_t)tfd->private_data);
        return;
    }
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

static int sys_pipe(uint64_t pipefd_ptr, const char *unused1, int unused2) {
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

static int64_t sys_getcwd(uint64_t buf_ptr, const char *size_ptr, int unused) {
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
    return (int64_t)buf_ptr;
}

static int sys_chdir(uint64_t path_ptr, const char *unused1, int unused2) {
    uint64_t addr;
    char *path;
    char *resolved;
    vfs_node_t *node;
    int ret;

    (void)unused1; (void)unused2;
    addr = (uint64_t)path_ptr;
    ret = posix_copy_user_string(&path, (const char *)addr, UINT64_MAX);
    if (ret < 0) return ret;
    if (strncmp(path, "/ro", 3) == 0 && (path[3] == '\0' || path[3] == '/')) {
        kfree(path);
        return -EACCES;
    }
    node = vfs_namei(path);
    if (!node) { kfree(path); return -ENOENT; }
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) { vfs_release(node); kfree(path); return -ENOTDIR; }
    if (!current_task) { vfs_release(node); kfree(path); return -EFAULT; }
    resolved = vfs_get_path_alloc(node);
    ret = task_set_cwd(current_task, resolved ? resolved : path);
    if (resolved) kfree(resolved);
    vfs_release(node);
    kfree(path);
    return ret == 0 ? 0 : -ENOMEM;
}

static int sys_chroot(const char *path_arg, int unused1, int unused2) {
    char *path;
    char *canonical;
    vfs_node_t *node;
    int result;

    (void)unused1;
    (void)unused2;
    if (!current_task) return -ESRCH;
    if (!creds_has_capability(current_task, 18)) return -EPERM;
    result = posix_copy_user_string(&path, path_arg, UINT64_MAX);
    if (result < 0) return result;
    node = vfs_namei(path);
    kfree(path);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        vfs_release(node);
        return -ENOTDIR;
    }
    canonical = vfs_get_path_alloc(node);
    if (!canonical) {
        vfs_release(node);
        return -EINVAL;
    }
    result = task_set_root(current_task, canonical);
    if (result == 0) result = task_set_cwd(current_task, "/");
    kfree(canonical);
    vfs_release(node);
    return result == 0 ? 0 : -ENOMEM;
}

static int sys_pivot_root(const char *new_root_arg, const char *put_old_arg,
                          int unused) {
    char *new_root;
    char *put_old;
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
    result = posix_copy_user_string(&new_root, new_root_arg, UINT64_MAX);
    if (result < 0) return result;
    result = posix_copy_user_string(&put_old, put_old_arg, UINT64_MAX);
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
    new_canonical = vfs_get_path_alloc(new_node);
    old_canonical = vfs_get_path_alloc(old_node);
    if (!new_canonical || !old_canonical) {
        if (new_canonical) kfree(new_canonical);
        if (old_canonical) kfree(old_canonical);
        vfs_release(new_node);
        vfs_release(old_node);
        return -EINVAL;
    }
    new_length = strlen(new_canonical);
    if (strncmp(old_canonical, new_canonical, new_length) != 0 ||
        old_canonical[new_length] != '/') {
        kfree(new_canonical);
        kfree(old_canonical);
        vfs_release(new_node);
        vfs_release(old_node);
        return -EINVAL;
    }
    result = task_set_root(current_task, new_canonical);
    if (result == 0) result = task_set_cwd(current_task, "/");
    kfree(new_canonical);
    kfree(old_canonical);
    vfs_release(new_node);
    vfs_release(old_node);
    return result == 0 ? 0 : -ENOMEM;
}

static inline uint64_t vfs_mask_to_unix_perms(uint64_t mask);

static int sys_access(uint64_t path_ptr, const char *mode_ptr, int unused) {
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
    ret = posix_copy_user_string(&path, (const char *)addr, UINT64_MAX);
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
        case VFS_SOCKET:
            return S_IFSOCK | (perms ? perms : 0777);
        default:
            return S_IFREG | (perms ? perms : 0644);
    }
}

static int sys_stat(uint64_t path_ptr, const char *buf_ptr, int unused) {
    (void)unused;
    return syscall_fstatat(POSIX_AT_FDCWD,
                           (const char *)(uintptr_t)path_ptr,
                           (uint64_t)(uintptr_t)buf_ptr);
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
        
        if (FD_TYPE_IS_PTY(fd_table[fd].type)) {
            pty_fd = (int)(uintptr_t)fd_table[fd].private_data;
            if (pty_fd < 0) return -EBADF;
            st->st_mode = S_IFCHR | 0620;
            st->st_rdev = 0x8801;
            st->st_blksize = 1024;
            st->st_nlink = 1;
            return 0;
        }

        if (fd_table[fd].type == FD_TYPE_SOCKET) {
            st->st_mode = S_IFSOCK | 0600;
            st->st_blksize = 4096;
            st->st_nlink = 1;
            return 0;
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
    if (FD_TYPE_IS_PTY(tfd->type)) return -ESPIPE;
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

static int sys_gettimeofday(uint64_t tv_ptr, const char *tz_ptr, int unused) {
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

static int sys_settimeofday(uint64_t tv_ptr, const char *tz_ptr, int unused) {
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
                               uint64_t request_ptr,
                               uint64_t remain_ptr) {
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

static int sys_execve(uint64_t path_ptr, const char *argv_ptr,
                      uint64_t envp_ptr) {
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
    char *shebang_interp;
    char *shebang_arg;
    int shebang_has_arg;
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
    shebang_interp = NULL;
    shebang_arg = NULL;
    argc = 0;
    envc = 0;

    path_addr = (uint64_t)path_ptr;
    argv_addr = (uint64_t)argv_ptr;
    envp_addr = (uint64_t)envp_ptr;

    stack_limit = task_rlimit_get(current_task, 3, 0);
    exec_budget = USER_STACK_TOP - USER_STACK_FLOOR;
    if (stack_limit < exec_budget) exec_budget = stack_limit;
    ret = posix_copy_user_string(&path, (const char *)path_addr, exec_budget);
    if (ret != 0) return ret;

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
        ret = posix_read_shebang(node, size, &shebang_interp, &shebang_arg);
        if (ret != 0) {
            vfs_close(node);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return ret;
        }
        shebang_has_arg = shebang_arg != NULL;

        vfs_close(node);
        node = NULL;

        interp_node = vfs_namei(shebang_interp);
        if (!interp_node) {
            if (shebang_arg) kfree(shebang_arg);
            kfree(shebang_interp);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOENT;
        }
        if (vfs_get_mount_flags_for_node(interp_node) & VFS_MS_NOEXEC) {
            vfs_release(interp_node);
            if (shebang_arg) kfree(shebang_arg);
            kfree(shebang_interp);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -EACCES;
        }
        vfs_open(interp_node, 0);
        interp_size = interp_node->length;
        if (interp_size == 0) {
            vfs_close(interp_node);
            if (shebang_arg) kfree(shebang_arg);
            kfree(shebang_interp);
            posix_free_string_array(envp, envc);
            posix_free_string_array(argv, argc);
            kfree(path);
            return -ENOEXEC;
        }

        new_argc = 1 + shebang_has_arg + 1 + (argc > 1 ? argc - 1 : 0);
        new_argv = (char **)kmalloc((new_argc + 1) * sizeof(char *));
        if (!new_argv) {
            vfs_close(interp_node);
            if (shebang_arg) kfree(shebang_arg);
            kfree(shebang_interp);
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
            if (shebang_arg) kfree(shebang_arg);
            kfree(shebang_interp);
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
                kfree(shebang_arg);
                kfree(shebang_interp);
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
            if (shebang_arg) kfree(shebang_arg);
            kfree(shebang_interp);
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
        if (shebang_arg) kfree(shebang_arg);
        kfree(shebang_interp);
        shebang_arg = NULL;
        shebang_interp = NULL;

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

static int sys_truncate(uint64_t path_ptr, const char *len_ptr, int unused) {
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
    
    if (node->ops && node->ops->truncate) {
        result = node->ops->truncate(node, length);
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
    
    if (node->ops && node->ops->truncate) {
        return node->ops->truncate(node, length);
    }
    
    return -ENOSYS;
}

static int sys_fallocate(int fd, const char *mode_ptr, int64_t offset,
                         int64_t length) {
    int mode;
    uint64_t end;
    task_fd_t *tfd;
    vfs_node_t *node;

    mode = (int)(uintptr_t)mode_ptr;
    if (mode != 0) return -EOPNOTSUPP;
    if (offset < 0 || length <= 0) return -EINVAL;
    if ((uint64_t)offset > UINT64_MAX - (uint64_t)length)
        return -EFBIG;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    tfd = &fd_table[fd];
    if (!tfd->in_use || tfd->type != FD_TYPE_FILE || !tfd->node)
        return -EBADF;
    if ((tfd->flags & 3) == VFS_O_RDONLY) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) != VFS_FILE) return -EINVAL;
    if (vfs_get_mount_flags_for_node(node) & VFS_MS_RDONLY) return -EROFS;
    end = (uint64_t)offset + (uint64_t)length;
    if (end <= node->length) return 0;
    if (!node->ops || !node->ops->truncate) return -EOPNOTSUPP;
    return node->ops->truncate(node, end);
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
    size_t name_len;
    size_t reclen;
    struct linux_dirent *de;
    int i;
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t local_copy;
    const char *entry_name;

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

        entry_name = vfs_dirent_name(&local_copy);
        name_len = strlen(entry_name);
        if (name_len > UINT16_MAX - sizeof(struct linux_dirent) - 4)
            return written ? written : -EOVERFLOW;
        reclen = sizeof(struct linux_dirent) + name_len + 1;
        reclen = (reclen + 3) & ~3;

        if (reclen > UINT16_MAX)
            return written ? written : -EOVERFLOW;
        if (reclen > (size_t)(count - written))
            return written ? written : -EINVAL;

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
        else if (local_copy.type == VFS_SOCKET) de->d_type = 12;
        else de->d_type = 0;

        for (i = 0; i < (int)name_len; i++) {
            de->d_name[i] = entry_name[i];
        }
        de->d_name[name_len] = '\0';

        written += (int)reclen;
        dir_offset++;
    }

    task_fd_position_set(tfd, dir_offset);
    return written;
}

static int sys_rename(uint64_t oldpath_ptr, const char *newpath_ptr,
                      int unused) {
    (void)unused;
    return syscall_renameat(POSIX_AT_FDCWD,
                            (const char *)(uintptr_t)oldpath_ptr,
                            POSIX_AT_FDCWD, newpath_ptr);
}
static int sys_link(uint64_t oldpath_ptr, const char *newpath_ptr,
                    int unused) {
    (void)unused;
    return syscall_linkat(POSIX_AT_FDCWD,
                          (const char *)(uintptr_t)oldpath_ptr,
                          POSIX_AT_FDCWD, newpath_ptr, 0);
}
static int sys_symlink(uint64_t target_ptr, const char *linkpath_ptr,
                       int unused) {
    (void)unused;
    return syscall_symlinkat(target_ptr,
        (const char *)(intptr_t)POSIX_AT_FDCWD,
        (uint64_t)(uintptr_t)linkpath_ptr);
}
static int sys_readlink(uint64_t path_ptr, const char *buf_ptr,
                        int bufsiz) {
    if (bufsiz <= 0) return -EINVAL;
    return syscall_readlinkat(POSIX_AT_FDCWD,
        (const char *)(uintptr_t)path_ptr,
        (uint64_t)(uintptr_t)buf_ptr, (uint64_t)bufsiz);
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
    vfs_node_t *node;
    int result;

    if (!current_task) return -ESRCH;
    if (fd < 3 || fd >= current_task->fds_capacity || !fd_table[fd].in_use) return -EBADF;
    node = (vfs_node_t *)fd_table[fd].node;
    if (!node) return -EBADF;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;
    resolved = vfs_get_path_alloc(node);
    if (!resolved) return -ENOENT;
    result = task_set_cwd(current_task, resolved);
    kfree(resolved);
    return result == 0 ? 0 : -ENOMEM;
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

    if (node->ops && node->ops->chmod) {
        return node->ops->chmod(node, mode & 07777);
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

    if (node->ops && node->ops->chown) {
        return node->ops->chown(node, uid, gid);
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

static int sys_copy_file_range(int fd_in, uint64_t off_in_ptr, int fd_out,
                               uint64_t off_out_ptr, uint64_t length,
                               unsigned int flags) {
    task_fd_t *input_fd;
    task_fd_t *output_fd;
    vfs_node_t *input_node;
    vfs_node_t *output_node;
    int64_t input_offset_value;
    int64_t output_offset_value;
    uint64_t input_offset;
    uint64_t output_offset;
    uint64_t available;
    uint64_t limit;
    uint64_t window_size;
    uint64_t window_pages;
    uint64_t window_phys;
    uint8_t *window;
    uint8_t *view;
    uint8_t *scratch;
    uint64_t scratch_capacity;
    copy_transfer_session_t *session;
    uint64_t total;
    uint64_t request;
    uint64_t received;
    uint64_t consumed;
    uint64_t written;
    int error;

    if (flags != 0) return -EINVAL;
    if (!current_task) return -ESRCH;
    if (fd_in < 0 || fd_in >= current_task->fds_capacity) return -EBADF;
    if (fd_out < 0 || fd_out >= current_task->fds_capacity) return -EBADF;
    input_fd = &fd_table[fd_in];
    output_fd = &fd_table[fd_out];
    if (!input_fd->in_use || !output_fd->in_use) return -EBADF;
    if (input_fd->type != FD_TYPE_FILE ||
        output_fd->type != FD_TYPE_FILE) return -EBADF;
    if (!input_fd->node || !output_fd->node) return -EBADF;
    if ((input_fd->flags & 3) == VFS_O_WRONLY) return -EBADF;
    if ((output_fd->flags & 3) == VFS_O_RDONLY) return -EBADF;
    if (output_fd->flags & VFS_O_APPEND) return -EBADF;
    input_node = (vfs_node_t *)input_fd->node;
    output_node = (vfs_node_t *)output_fd->node;
    if (VFS_GET_TYPE(input_node->flags) != VFS_FILE ||
        VFS_GET_TYPE(output_node->flags) != VFS_FILE) return -EINVAL;
    if (input_node == output_node ||
        overlay_same_file(input_node, output_node)) return -EINVAL;
    if (vfs_get_mount_flags_for_node(output_node) & VFS_MS_RDONLY)
        return -EROFS;
    error = vfs_check_perm(input_node, VFS_PERM_READ);
    if (error < 0) return error;
    error = vfs_check_perm(output_node, VFS_PERM_WRITE);
    if (error < 0) return error;
    if (length == 0) return 0;

    if (off_in_ptr != 0) {
        if (!posix_user_range_mapped(off_in_ptr,
                                     sizeof(input_offset_value)))
            return -EFAULT;
        if (copy_from_user(&input_offset_value,
                           (const void *)(uintptr_t)off_in_ptr,
                           sizeof(input_offset_value)) != 0)
            return -EFAULT;
        if (input_offset_value < 0) return -EINVAL;
        input_offset = (uint64_t)input_offset_value;
    } else {
        input_offset = task_fd_position_get(input_fd);
    }
    if (off_out_ptr != 0) {
        if (!posix_user_range_mapped(off_out_ptr,
                                     sizeof(output_offset_value)))
            return -EFAULT;
        if (copy_from_user(&output_offset_value,
                           (const void *)(uintptr_t)off_out_ptr,
                           sizeof(output_offset_value)) != 0)
            return -EFAULT;
        if (output_offset_value < 0) return -EINVAL;
        output_offset = (uint64_t)output_offset_value;
    } else {
        output_offset = task_fd_position_get(output_fd);
    }
    if (input_offset >= input_node->length) return 0;
    available = input_node->length - input_offset;
    limit = length;
    if (limit > available) limit = available;
    if (limit > INT32_MAX) limit = INT32_MAX;
    if (output_offset + limit < output_offset) return -EOVERFLOW;

    window_size = vfs_transfer_window_size(input_node);
    if (window_size == 0) return -EIO;
    window_phys = 0;
    window_pages = 0;
    scratch = NULL;
    scratch_capacity = 0;
    session = NULL;
    if (vfs_transfer_reuse_supported(input_node))
        session = copy_transfer_acquire(current_task, output_node,
                                        window_size);
    if (session) {
        window = session->cache->data;
        scratch = session->cache->scratch;
        scratch_capacity = session->cache->scratch_capacity;
    } else {
        window_pages = (window_size + PAGE_SIZE - 1) / PAGE_SIZE;
        window_phys = pfa_alloc_contiguous(window_pages);
        if (!window_phys) return -ENOMEM;
        window = (uint8_t *)(uintptr_t)(window_phys + KERNEL_VMA);
    }
    total = 0;
    error = 0;
    while (total < limit) {
        request = limit - total;
        if (request > window_size) request = window_size;
        view = window;
        if (session) {
            received = vfs_transfer_read_view(input_node,
                                              input_offset + total,
                                              request, session->cache,
                                              &view);
            if (received == 0) {
                view = window;
                received = vfs_transfer_read(input_node,
                                             input_offset + total,
                                             request, window,
                                             window_size);
            }
        } else {
            received = vfs_transfer_read(input_node, input_offset + total,
                                         request, window, window_size);
        }
        if (received > request) received = request;
        if (received == 0) {
            error = -EIO;
            break;
        }
        consumed = 0;
        while (consumed < received) {
            if (session) {
                written = vfs_transfer_write(output_node,
                                             output_offset + total,
                                             received - consumed,
                                             view + consumed, scratch,
                                             scratch_capacity);
            } else {
                written = vfs_write(output_node, output_offset + total,
                                    received - consumed,
                                    window + consumed);
            }
            if (written > received - consumed)
                written = received - consumed;
            if (written == 0) {
                error = -EIO;
                break;
            }
            consumed += written;
            total += written;
        }
        if (error != 0 || consumed < received) break;
        if (received < request) break;
    }
    if (window_phys) pfa_free_contiguous(window_phys, window_pages);
    if (session) copy_transfer_finish(session);

    input_offset_value = (int64_t)(input_offset + total);
    output_offset_value = (int64_t)(output_offset + total);
    if (off_in_ptr != 0) {
        if (copy_to_user((void *)(uintptr_t)off_in_ptr,
                         &input_offset_value,
                         sizeof(input_offset_value)) != 0)
            return total ? (int)total : -EFAULT;
    } else {
        task_fd_position_set(input_fd, input_offset + total);
    }
    if (off_out_ptr != 0) {
        if (copy_to_user((void *)(uintptr_t)off_out_ptr,
                         &output_offset_value,
                         sizeof(output_offset_value)) != 0)
            return total ? (int)total : -EFAULT;
    } else {
        task_fd_position_set(output_fd, output_offset + total);
    }
    if (total != 0) return (int)total;
    if (error != 0) return error;
    return 0;
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
    size_t name_len;
    size_t reclen;
    struct linux_dirent64 *de;
    int i;
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t local_copy;
    const char *entry_name;

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
        
        entry_name = vfs_dirent_name(&local_copy);
        name_len = strlen(entry_name);
        if (name_len > UINT16_MAX - sizeof(struct linux_dirent64) - 8)
            return written ? written : -EOVERFLOW;
        reclen = sizeof(unsigned long long) + sizeof(long long) + sizeof(unsigned short) + sizeof(unsigned char) + name_len + 1;
        reclen = (reclen + 7) & ~7;
        
        if (reclen > UINT16_MAX)
            return written ? written : -EOVERFLOW;
        if (reclen > (size_t)(count - (unsigned int)written))
            return written ? written : -EINVAL;
        
        de = (struct linux_dirent64 *)(buf + written);
        de->d_ino = local_copy.inode ? local_copy.inode : dir_offset + 1;
        de->d_off = dir_offset + 1;
        de->d_reclen = (unsigned short)reclen;
        
        if (local_copy.type == VFS_DIRECTORY) de->d_type = 4;
        else if (local_copy.type == VFS_FILE) de->d_type = 8;
        else if (local_copy.type == VFS_SYMLINK) de->d_type = 10;
        else if (local_copy.type == VFS_CHARDEVICE) de->d_type = 2;
        else if (local_copy.type == VFS_BLOCKDEVICE) de->d_type = 6;
        else if (local_copy.type == VFS_PIPE) de->d_type = 1;
        else if (local_copy.type == VFS_SOCKET) de->d_type = 12;
        else de->d_type = 0;
        
        for (i = 0; i < (int)name_len; i++) {
            de->d_name[i] = entry_name[i];
        }
        de->d_name[name_len] = '\0';
        
        written += (int)reclen;
        dir_offset++;
    }
    
    task_fd_position_set(tfd, dir_offset);
    return written;
}

void syscalls_posix_init(void) {
    mutex_init(&copy_transfer_lock);
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
    syscall_table_set(SYSCALL_FALLOCATE, (void *)(sys_fallocate));
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
    syscall_table_set(SYSCALL_COPY_FILE_RANGE,
                      (void *)(sys_copy_file_range));
    syscall_table_set(SYSCALL_READV, (void *)(sys_readv));
}

#include "syscall_defs.h"
#include <lebirun/ramfs.h>
#include <lebirun/squashfs.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>

extern int is_socket_fd(int fd);
extern int socket_close_fd(int fd);
extern int is_epoll_special_fd(int fd);
extern int epoll_close_fd(int fd);

#define VFS_RW_STACK_BUF 512
#define VFS_RW_HEAP_LIMIT 4096
#define VFS_BLOCK_IO_CHUNK 65536

static int vfs_user_range_mapped(uint64_t addr, uint64_t len) {
    uint64_t end;
    uint64_t p;
    uint64_t pd;
    uint64_t phys;
    uint64_t *new_user_pages;

    if (len == 0) return 1;
    if (!current_task) return 0;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return 0;
    if (addr + len < addr || addr + len > KERNEL_VMA) return 0;
    pd = current_task->cr3 ? current_task->cr3 : current_task->pml4_phys;
    if (!pd) return 0;

    end = addr + len - 1;
    p = addr & ~0xFFFULL;
    while (p <= end) {
        if (vmm_get_phys_in_pml4(pd, p) == 0) {
            if (!task_handle_file_page_fault(current_task, p)) {
                if ((p >= USER_STACK_FLOOR && p < USER_STACK_TOP) ||
                        (p >= current_task->user_brk && p < 0x40000000u) ||
                        (p >= 0x1000u && p < current_task->user_brk)) {
                    phys = pfa_alloc();
                    if (!phys) return 0;
                    pmm_zero_page_phys(phys);
                    vmm_map_page_in_pml4(pd, p, phys, 0x7);
                    if (vmm_get_phys_in_pml4(pd, p) == 0) {
                        pfa_free(phys);
                        return 0;
                    }
                    new_user_pages = (uint64_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint64_t));
                    if (!new_user_pages) {
                        vmm_unmap_page_in_pml4(pd, p);
                        pfa_free(phys);
                        return 0;
                    }
                    current_task->user_pages = new_user_pages;
                    current_task->user_pages[current_task->user_pages_count] = phys;
                    current_task->user_pages_count++;
                } else {
                    return 0;
                }
            }
            if (vmm_get_phys_in_pml4(pd, p) == 0) return 0;
        }
        if (p + 0x1000ULL <= p) return 0;
        p += 0x1000ULL;
    }
    return 1;
}

static int vfs_user_string_mapped(const char *s, size_t max) {
    uint64_t addr;
    uint64_t current;
    size_t chunk;
    size_t page_remaining;
    size_t i;
    size_t j;

    if (!s || max == 0) return 0;
    addr = (uint64_t)(uintptr_t)s;
    i = 0;
    while (i < max) {
        current = addr + i;
        if (current < addr) return 0;
        page_remaining = 0x1000 - (size_t)(current & 0xFFF);
        chunk = max - i;
        if (chunk > page_remaining) chunk = page_remaining;
        if (!vfs_user_range_mapped(current, chunk)) return 0;
        for (j = 0; j < chunk; j++) {
            if (s[i + j] == '\0') return 1;
        }
        i += chunk;
    }
    return 0;
}

static size_t vfs_bounded_strlen(const char *s, size_t max) {
    size_t i;

    if (!s) return 0;
    for (i = 0; i < max; i++) {
        if (s[i] == '\0') return i;
    }
    return max;
}

static int vfs_check_perm(vfs_node_t *node, int want) {
    uint64_t mode;
    uint64_t uid;
    uint64_t gid;
    int shift;
    int allowed;

    if (!current_task) return -ESRCH;
    if (!node) return -ENOENT;

    uid = current_task->euid;
    gid = current_task->egid;

    if (uid == 0) return 0;

    mode = node->mask;

    if (uid == node->uid) {
        shift = 6;
    } else if (gid == node->gid) {
        shift = 3;
    } else {
        shift = 0;
    }

    allowed = (int)((mode >> shift) & 7);
    if ((allowed & want) == want) return 0;
    return -EACCES;
}

static int task_fd_alloc_from(int start) {
    int i;
    int new_cap;
    task_fd_t *new_fds;

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
    if (current_task->fds_capacity >= TASK_MAX_FDS) return -EMFILE;
    new_cap = current_task->fds_capacity * 2;
    if (new_cap > TASK_MAX_FDS) new_cap = TASK_MAX_FDS;
    if (start >= new_cap) new_cap = start + 16;
    if (new_cap > TASK_MAX_FDS) new_cap = TASK_MAX_FDS;
    new_fds = (task_fd_t *)krealloc(current_task->fds, new_cap * sizeof(task_fd_t));
    if (!new_fds) return -ENOMEM;
    memset(&new_fds[current_task->fds_capacity], 0, (new_cap - current_task->fds_capacity) * sizeof(task_fd_t));
    i = current_task->fds_capacity;
    if (start > i) i = start;
    current_task->fds = new_fds;
    current_task->fds_capacity = new_cap;
    memset(&current_task->fds[i], 0, sizeof(task_fd_t));
    current_task->fds[i].in_use = 1;
    current_task->fds[i].ref_count = 1;
    current_task->fds[i].type = FD_TYPE_FILE;
    return i;
}

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

static int sys_vfs_open(uint64_t path_ptr, uint64_t flags_arg, uint64_t mode_arg) {
    uint64_t path_addr;
    int flags;
    int mode;
    uint64_t create_mode;
    const char *path;
    int fd;
    char resolved_path[256];
    char parent_path[256];
    char filename[64];
    int len;
    int last_slash;
    int i;
    int j;
    int ret;
    vfs_node_t *node;
    vfs_node_t *parent;

    path_addr = path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    flags = (int)flags_arg;
    mode = (int)mode_arg;
    if (!current_task) return -ESRCH;

    path = resolve_cwd_path((const char *)path_addr, resolved_path, sizeof(resolved_path));
    if (!path) return -EFAULT;
    node = vfs_namei(path);

    if (node && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
        vfs_release(node);
        return -EEXIST;
    }

    if (!node && (flags & VFS_O_CREAT)) {
        create_mode = (uint64_t)(mode & 0777);
        create_mode &= ~current_task->creation_mask;
        if (flags & VFS_O_EXCL) create_mode |= VFS_O_EXCL;

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

        ret = vfs_create(parent, filename, create_mode);
        vfs_release(parent);
        if (ret < 0 && !(flags & VFS_O_EXCL)) {
            node = vfs_namei(path);
        } else if (ret == 0) {
            node = vfs_namei(path);
        }
    }

    if (!node) {
        return -ENOENT;
    }

    {
        int want;
        int perm_ret;

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
    }

    if ((flags & VFS_O_TRUNC) && node->truncate) {
        node->truncate(node, 0);
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

static int sys_vfs_close(int fd, const char *unused1, int unused2) {
    task_fd_t *tfd;
    pipe_t *p;
    vfs_node_t *node;

    (void)unused1; (void)unused2;
    if (is_socket_fd(fd)) return socket_close_fd(fd);
    if (is_epoll_special_fd(fd)) return epoll_close_fd(fd);
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    p = NULL;
    node = NULL;

    if (tfd->type == FD_TYPE_PIPE_R || tfd->type == FD_TYPE_PIPE_W) {
        p = (pipe_t *)tfd->private_data;
        if (p) {
            if (tfd->type == FD_TYPE_PIPE_R) p->readers--;
            else p->writers--;
            waitq_wake_all(&p->read_waitq);
            waitq_wake_all(&p->write_waitq);
            if (p->readers <= 0 && p->writers <= 0) {
                if (p->buffer) kfree(p->buffer);
                kfree(p);
            }
        }
        memset(tfd, 0, sizeof(*tfd));
        task_fd_reclaim_unused(current_task);
        return 0;
    }

    if (tfd->type == FD_TYPE_FILE && tfd->node) {
        node = (vfs_node_t *)tfd->node;
        memset(tfd, 0, sizeof(*tfd));
        vfs_close(node);
        task_fd_reclaim_unused(current_task);
        return 0;
    }

    memset(tfd, 0, sizeof(*tfd));
    task_fd_reclaim_unused(current_task);
    return 0;
}

static int sys_vfs_read(int fd, const char *buf, int len) {
    uint64_t buf_addr;
    uint64_t work_size;
    uint64_t remaining;
    uint64_t total;
    uint64_t chunk;
    uint64_t bytes;
    uint8_t stack_buf[VFS_RW_STACK_BUF];
    uint8_t *kbuf;
    int heap_buf;
    task_fd_t *tfd;
    vfs_node_t *node;

    if (!buf || len <= 0) return -EINVAL;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -EFAULT;
    if (!vfs_user_range_mapped(buf_addr, (uint64_t)len)) return -EFAULT;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) == VFS_BLOCKDEVICE) {
        total = 0;
        remaining = (uint64_t)len;
        while (remaining > 0) {
            chunk = remaining;
            if (chunk > VFS_BLOCK_IO_CHUNK) chunk = VFS_BLOCK_IO_CHUNK;
            bytes = vfs_read(node, task_fd_position_get(tfd) + total, chunk,
                             (uint8_t *)(buf_addr + total));
            if (bytes > chunk) bytes = chunk;
            if (bytes == 0) break;
            total += bytes;
            remaining -= bytes;
            if (bytes < chunk) break;
        }
        task_fd_position_add(tfd, total);
        return (int)total;
    }
    work_size = (uint64_t)len;
    if (tfd->flags & VFS_O_APPEND) {
        task_fd_position_set(tfd, node->length);
    }
    if (work_size > VFS_RW_HEAP_LIMIT) work_size = VFS_RW_HEAP_LIMIT;
    heap_buf = 0;
    if (work_size <= VFS_RW_STACK_BUF) {
        kbuf = stack_buf;
    } else {
        kbuf = (uint8_t *)kmalloc(work_size);
        if (!kbuf) return -ENOMEM;
        heap_buf = 1;
    }
    total = 0;
    remaining = (uint64_t)len;
    while (remaining > 0) {
        chunk = remaining;
        if (chunk > work_size) chunk = work_size;
        bytes = vfs_read(node, task_fd_position_get(tfd) + total, chunk, kbuf);
        if (bytes > chunk) bytes = chunk;
        if (bytes == 0) break;
        memcpy((void *)(buf_addr + total), kbuf, bytes);
        total += bytes;
        remaining -= bytes;
        if (bytes < chunk) break;
    }
    task_fd_position_add(tfd, total);
    if (heap_buf) kfree(kbuf);
    return (int)total;
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

    fd = (int)regs->rbx;
    name_addr = regs->rcx;
    type_addr = regs->rdx;
    index = regs->rsi;

    if (name_addr && !vfs_user_range_mapped(name_addr, 64)) return -EFAULT;
    if (type_addr && !vfs_user_range_mapped(type_addr, sizeof(uint32_t))) return -EFAULT;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;

    if (vfs_readdir_copy(node, index, &local_copy) != 0) {
        return -ENOENT;
    }

    if (name_addr) {
        i = 0;
        for (; i < 63 && local_copy.name[i]; i++) {
            ((char*)name_addr)[i] = local_copy.name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }

    if (type_addr) {
        *(uint32_t*)type_addr = (uint32_t)local_copy.type;
    }

    return 0;
}

static int sys_vfs_stat(int fd, const char *size_ptr, int type_ptr) {
    uint64_t size_addr = (uint64_t)size_ptr;
    uint64_t type_addr = (uint64_t)type_ptr;
    uint64_t size;
    uint64_t flags;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)tfd->node;
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

static int sys_vfs_write(int fd, const char *buf, int len) {
    uint64_t buf_addr;
    uint64_t work_size;
    uint64_t remaining;
    uint64_t total;
    uint64_t chunk;
    uint64_t bytes;
    uint8_t stack_buf[VFS_RW_STACK_BUF];
    uint8_t *kbuf;
    int heap_buf;
    task_fd_t *tfd;
    vfs_node_t *node;

    if (!buf || len <= 0) return -EINVAL;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -EFAULT;
    if (!vfs_user_range_mapped(buf_addr, (uint64_t)len)) return -EFAULT;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) == VFS_BLOCKDEVICE) {
        total = 0;
        remaining = (uint64_t)len;
        while (remaining > 0) {
            chunk = remaining;
            if (chunk > VFS_BLOCK_IO_CHUNK) chunk = VFS_BLOCK_IO_CHUNK;
            bytes = vfs_write(node, task_fd_position_get(tfd) + total, chunk,
                              (uint8_t *)(buf_addr + total));
            if (bytes > chunk) bytes = chunk;
            if (bytes == 0) break;
            total += bytes;
            remaining -= bytes;
            if (bytes < chunk) break;
        }
        task_fd_position_add(tfd, total);
        return (int)total;
    }
    work_size = (uint64_t)len;
    if (work_size > VFS_RW_HEAP_LIMIT) work_size = VFS_RW_HEAP_LIMIT;
    heap_buf = 0;
    if (work_size <= VFS_RW_STACK_BUF) {
        kbuf = stack_buf;
    } else {
        kbuf = (uint8_t *)kmalloc(work_size);
        if (!kbuf) return -ENOMEM;
        heap_buf = 1;
    }
    total = 0;
    remaining = (uint64_t)len;
    while (remaining > 0) {
        chunk = remaining;
        if (chunk > work_size) chunk = work_size;
        memcpy(kbuf, (const void *)(buf_addr + total), chunk);
        bytes = vfs_write(node, task_fd_position_get(tfd) + total, chunk, kbuf);
        if (bytes > chunk) bytes = chunk;
        if (bytes == 0) break;
        total += bytes;
        remaining -= bytes;
        if (bytes < chunk) break;
    }
    task_fd_position_add(tfd, total);
    if (heap_buf) kfree(kbuf);
    return (int)total;
}

static int sys_vfs_create(int path_ptr, const char *perms_ptr, int unused) {
    uint64_t path_addr;
    const char *path;
    uint64_t perms;
    char resolved_path[256];
    char parent_path[256];
    char filename[64];
    int ret;
    vfs_node_t *parent;
    int perm_ret;
    int r;

    (void)unused;
    path_addr = (uint64_t)path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path((const char *)path_addr, resolved_path, sizeof(resolved_path));
    if (!path) return -EFAULT;
    perms = (uint64_t)(uintptr_t)perms_ptr;
    if (current_task) perms &= ~current_task->creation_mask;

    ret = split_parent_child_path(path, parent_path, sizeof(parent_path), filename, sizeof(filename));
    if (ret < 0) return ret;

    parent = vfs_namei(parent_path);
    if (!parent) return -ENOENT;
    perm_ret = vfs_check_perm(parent, VFS_PERM_WRITE);
    if (perm_ret < 0) { vfs_release(parent); return perm_ret; }
    r = vfs_create(parent, filename, perms);
    vfs_release(parent);
    return r;
}

static int sys_vfs_mkdir(int path_ptr, const char *perms_ptr, int unused) {
    uint64_t path_addr;
    const char *path;
    uint64_t perms;
    char resolved_path[256];
    char parent_path[256];
    char dirname[64];
    int ret;
    vfs_node_t *parent;
    int perm_ret;
    int r;

    (void)unused;
    path_addr = (uint64_t)path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path((const char *)path_addr, resolved_path, sizeof(resolved_path));
    if (!path) return -EFAULT;
    perms = (uint64_t)(uintptr_t)perms_ptr;
    if (current_task) perms &= ~current_task->creation_mask;

    ret = split_parent_child_path(path, parent_path, sizeof(parent_path), dirname, sizeof(dirname));
    if (ret < 0) return ret;

    parent = vfs_namei(parent_path);
    if (!parent) return -ENOENT;
    perm_ret = vfs_check_perm(parent, VFS_PERM_WRITE);
    if (perm_ret < 0) { vfs_release(parent); return perm_ret; }
    r = vfs_mkdir(parent, dirname, perms);
    vfs_release(parent);
    return r;
}

static int sys_vfs_unlink(int path_ptr, const char *unused1, int unused2) {
    uint64_t path_addr;
    const char *path;
    char resolved_path[256];
    char parent_path[256];
    char filename[64];
    int ret;
    vfs_node_t *parent;
    int perm_ret;
    int r;

    (void)unused1; (void)unused2;
    path_addr = (uint64_t)path_ptr;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    path = resolve_cwd_path((const char *)path_addr, resolved_path, sizeof(resolved_path));
    if (!path) return -EFAULT;

    ret = split_parent_child_path(path, parent_path, sizeof(parent_path), filename, sizeof(filename));
    if (ret < 0) return ret;

    parent = vfs_namei(parent_path);
    if (!parent) return -ENOENT;
    perm_ret = vfs_check_perm(parent, VFS_PERM_WRITE);
    if (perm_ret < 0) { vfs_release(parent); return perm_ret; }
    r = vfs_unlink_checked(parent, filename, 0);
    vfs_release(parent);
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


    memset(buf, 0, sizeof(struct statfs_kernel));
    buf->f_namelen = 255;

    best = NULL;
    best_len = 0;
    mount_count = vfs_get_mount_count();
    for (i = 0; i < mount_count; i++) {
        mount = vfs_get_mount(i);
        if (!mount) continue;
        mlen = vfs_bounded_strlen(mount->path, VFS_MAX_PATH);
        if (mlen == VFS_MAX_PATH) continue;
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
        uint64_t total_blocks, free_blocks;
        uint32_t bsize32;
        if (ext4_get_stats(&total_blocks, &free_blocks, &bsize32) == 0) {
            uint64_t bsize = bsize32;
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
        uint64_t total_kb;
        uint64_t free_kb;
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
        ramfs_stats_t rs;
        uint64_t used;
        squashfs_context_t *sqctx;
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
        uint64_t total_kb;
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
        uint64_t total_kb;
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
        uint64_t total_kb;
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

static int sys_statfs(int path_ptr, const char *size_ptr, int buf_ptr_int) {
    uint64_t path_addr;
    int size;
    uint64_t buf_addr;
    uint64_t arg2_addr;
    char path[VFS_MAX_PATH];
    struct statfs_kernel *buf;
    size_t i;

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

    if (!vfs_user_string_mapped((const char *)path_addr, sizeof(path))) {
        return -EFAULT;
    }
    for (i = 0; i < sizeof(path); i++) {
        path[i] = ((const char *)path_addr)[i];
        if (path[i] == '\0') break;
    }
    path[sizeof(path) - 1] = '\0';
    buf = (struct statfs_kernel *)buf_addr;

    fill_statfs_for_path(path, buf);

    return 0;
}

static int sys_fstatfs(int fd, const char *size_ptr, int buf_ptr_int) {
    int size = (int)(uintptr_t)size_ptr;
    uint64_t buf_addr = (uint64_t)buf_ptr_int;
    uint64_t arg2_addr;
    struct statfs_kernel *buf;

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

    buf = (struct statfs_kernel *)buf_addr;

    fill_statfs_for_path("/", buf);

    return 0;
}

static int sys_vfs_mount_user(int source_ptr, const char *target_ptr, int fstype_ptr, int flags_val) {
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
    if (current_task->uid != 0) return -EPERM;

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

static int sys_vfs_umount_user(int target_ptr, const char *unused1, int unused2) {
    uint64_t tgt_addr;
    const char *target;

    (void)unused1;
    (void)unused2;
    tgt_addr = (uint64_t)target_ptr;
    if (tgt_addr >= KERNEL_VMA || tgt_addr < 0x1000) return -EFAULT;
    if (!current_task) return -ESRCH;
    if (current_task->uid != 0) return -EPERM;

    target = (const char *)tgt_addr;
    return vfs_unmount(target);
}

void syscalls_vfs_init(void) {
    syscall_table[SYSCALL_OPEN] = sys_vfs_open;
    syscall_table[SYSCALL_CLOSE] = sys_vfs_close;
    syscall_table[SYSCALL_VFS_OPEN] = sys_vfs_open;
    syscall_table[SYSCALL_VFS_CLOSE] = sys_vfs_close;
    syscall_table[SYSCALL_VFS_READ] = sys_vfs_read;
    syscall_table[SYSCALL_VFS_READDIR] = (void*)1;
    syscall_table[SYSCALL_VFS_STAT] = sys_vfs_stat;
    syscall_table[SYSCALL_VFS_MOUNTS] = sys_vfs_mounts;
    syscall_table[SYSCALL_VFS_WRITE] = sys_vfs_write;
    syscall_table[SYSCALL_VFS_CREATE] = sys_vfs_create;
    syscall_table[SYSCALL_VFS_MKDIR] = sys_vfs_mkdir;
    syscall_table[SYSCALL_VFS_UNLINK] = sys_vfs_unlink;
    syscall_table[SYSCALL_STATFS] = sys_statfs;
    syscall_table[SYSCALL_FSTATFS] = sys_fstatfs;
    syscall_table[SYSCALL_VFS_MOUNT] = sys_vfs_mount_user;
    syscall_table[SYSCALL_VFS_UMOUNT] = sys_vfs_umount_user;
}

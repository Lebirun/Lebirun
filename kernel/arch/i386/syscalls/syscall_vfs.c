#include "syscall_defs.h"
#include <kernel/ramfs.h>

static int task_fd_alloc_from(int start) {
    int i;
    int new_cap;
    task_fd_t *new_fds;

    if (!current_task || !current_task->fds) return -ESRCH;
    if (start < 0) start = 0;
    for (i = start; i < current_task->fds_capacity; i++) {
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
    current_task->fds[i].in_use = 1;
    current_task->fds[i].ref_count = 1;
    current_task->fds[i].type = FD_TYPE_FILE;
    current_task->fds[i].node = NULL;
    current_task->fds[i].offset = 0;
    current_task->fds[i].flags = 0;
    current_task->fds[i].private_data = NULL;
    return i;
}

static int sys_vfs_open(int path_ptr, const char *flags_ptr, int unused) {
    uint32_t path_addr;
    int flags;
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

    (void)unused;
    path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    flags = (int)(uintptr_t)flags_ptr;
    if (!current_task) return -ESRCH;

    path = (const char *)path_addr;
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

    fd = task_fd_alloc_from(0);
    if (fd < 0) return fd;

    vfs_open(node, flags);

    current_task->fds[fd].type = FD_TYPE_FILE;
    current_task->fds[fd].node = node;
    current_task->fds[fd].offset = (flags & VFS_O_APPEND) ? node->length : 0;
    current_task->fds[fd].flags = (uint32_t)flags;
    return fd;
}

static int sys_vfs_close(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    task_fd_t *tfd = &current_task->fds[fd];

    if (tfd->type == FD_TYPE_PIPE_R || tfd->type == FD_TYPE_PIPE_W) {
        pipe_t *p = (pipe_t *)tfd->private_data;
        if (p) {
            if (tfd->type == FD_TYPE_PIPE_R) p->readers--;
            else p->writers--;
            waitq_wake_all(&p->read_waitq);
            waitq_wake_all(&p->write_waitq);
            if (p->readers <= 0 && p->writers <= 0) {
                kfree(p);
            }
        }
        memset(tfd, 0, sizeof(*tfd));
        return 0;
    }

    if (tfd->type == FD_TYPE_FILE && tfd->node) {
        vfs_node_t *node = (vfs_node_t *)tfd->node;
        memset(tfd, 0, sizeof(*tfd));
        vfs_close(node);
        return 0;
    }

    memset(tfd, 0, sizeof(*tfd));
    return 0;
}

static int sys_vfs_read(int fd, const char *buf, int len) {
    if (!buf || len <= 0) return -EINVAL;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint32_t)len < buf_addr || buf_addr + (uint32_t)len >= 0xC0000000) return -EFAULT;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)tfd->node;
    uint32_t bytes = vfs_read(node, tfd->offset, (uint32_t)len, (uint8_t *)buf_addr);
    tfd->offset += bytes;
    return (int)bytes;
}

int sys_vfs_readdir(registers_t *regs) {
    int fd;
    uint32_t name_addr;
    uint32_t type_addr;
    uint32_t index;
    task_fd_t *tfd;
    vfs_node_t *node;
    dirent_t *result;
    dirent_t local_copy;
    uint32_t flags;
    int i;

    fd = (int)regs->ebx;
    name_addr = regs->ecx;
    type_addr = regs->edx;
    index = regs->esi;

    if (name_addr && (name_addr >= 0xC0000000 || name_addr < 0x1000)) return -EFAULT;
    if (type_addr && (type_addr >= 0xC0000000 || type_addr < 0x1000)) return -EFAULT;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    node = (vfs_node_t *)tfd->node;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return -ENOTDIR;

    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    result = vfs_readdir(node, index);
    if (!result) {
        __asm__ volatile("push %0; popf" : : "r"(flags));
        return -ENOENT;
    }
    for (i = 0; i < VFS_MAX_NAME; i++) {
        local_copy.name[i] = result->name[i];
    }
    local_copy.inode = result->inode;
    local_copy.type = result->type;
    __asm__ volatile("push %0; popf" : : "r"(flags));

    if (name_addr) {
        i = 0;
        for (; i < 63 && local_copy.name[i]; i++) {
            ((char*)name_addr)[i] = local_copy.name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }

    if (type_addr) {
        *(uint32_t*)type_addr = local_copy.type;
    }

    return 0;
}

static int sys_vfs_stat(int fd, const char *size_ptr, int type_ptr) {
    uint32_t size_addr = (uint32_t)size_ptr;
    uint32_t type_addr = (uint32_t)type_ptr;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)tfd->node;
    uint32_t size = node->length;
    uint32_t flags = node->flags;
    
    if (size_addr && size_addr < 0xC0000000 && size_addr >= 0x1000) {
        *(uint32_t*)size_addr = size;
    }
    if (type_addr && type_addr < 0xC0000000 && type_addr >= 0x1000) {
        *(uint32_t*)type_addr = flags;
    }
    return 0;
}

static int sys_vfs_mounts(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    vfs_list_mounts();
    return vfs_get_mount_count();
}

static int sys_vfs_write(int fd, const char *buf, int len) {
    if (!buf || len <= 0) return -EINVAL;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint32_t)len < buf_addr || buf_addr + (uint32_t)len >= 0xC0000000) return -EFAULT;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) return -EBADF;
    if (!current_task->fds[fd].in_use) return -EBADF;

    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -EBADF;
    vfs_node_t *node = (vfs_node_t *)tfd->node;
    uint32_t bytes = vfs_write(node, tfd->offset, (uint32_t)len, (uint8_t *)buf_addr);
    tfd->offset += bytes;
    return (int)bytes;
}

static int sys_vfs_create(int path_ptr, const char *perms_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    const char *path = (const char *)path_addr;
    uint32_t perms = (uint32_t)(uintptr_t)perms_ptr;
    
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
    return vfs_create(parent, filename, perms);
}

static int sys_vfs_mkdir(int path_ptr, const char *perms_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    const char *path = (const char *)path_addr;
    uint32_t perms = (uint32_t)(uintptr_t)perms_ptr;
    
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
    return vfs_mkdir(parent, dirname, perms);
}

static int sys_vfs_unlink(int path_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    const char *path = (const char *)path_addr;
    
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
    return vfs_unlink(parent, filename);
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
    memset(buf, 0, sizeof(struct statfs_kernel));
    
    extern int ramfs_get_stats(ramfs_stats_t *stats);
    
    if (path[0] == '/' && (path[1] == '\0' || 
        (path[1] == 't' && path[2] == 'm' && path[3] == 'p'))) {
        ramfs_stats_t rs;
        if (ramfs_get_stats(&rs) == 0) {
            buf->f_type = RAMFS_MAGIC;
            buf->f_bsize = 4096;
            buf->f_frsize = 4096;
            buf->f_blocks = rs.total_size / 4096;
            buf->f_bfree = (rs.total_size - rs.used_size) / 4096;
            buf->f_bavail = buf->f_bfree;
            buf->f_files = rs.file_count + rs.dir_count + 1000;
            buf->f_ffree = 1000;
            buf->f_namelen = 255;
            return;
        }
    }
    
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' && 
        path[3] == 'o' && path[4] == 'c') {
        buf->f_type = PROCFS_MAGIC;
        buf->f_bsize = 1024;
        buf->f_frsize = 1024;
        buf->f_blocks = 1024;
        buf->f_bfree = 1024;
        buf->f_bavail = 1024;
        buf->f_files = 128;
        buf->f_ffree = 1;
        buf->f_namelen = 255;
        return;
    }
    
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v') {
        buf->f_type = DEVFS_MAGIC;
        buf->f_bsize = 1024;
        buf->f_frsize = 1024;
        buf->f_blocks = 512;
        buf->f_bfree = 512;
        buf->f_bavail = 512;
        buf->f_files = 32;
        buf->f_ffree = 1;
        buf->f_namelen = 255;
        return;
    }
    
    buf->f_type = RAMFS_MAGIC;
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 1024;
    buf->f_bfree = 768;
    buf->f_bavail = 768;
    buf->f_files = 256;
    buf->f_ffree = 200;
    buf->f_namelen = 255;
}

static int sys_statfs(int path_ptr, const char *buf_ptr, int size) {
    uint32_t path_addr = (uint32_t)path_ptr;
    uint32_t buf_addr = (uint32_t)buf_ptr;
    const char *path;
    struct statfs_kernel *buf;

    if (path_addr >= 0xC0000000 || path_addr < 0x1000) {
        return -EFAULT;
    }
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) {
        return -EFAULT;
    }

    if (size < (int)sizeof(struct statfs_kernel)) {
        return -EINVAL;
    }

    path = (const char *)path_addr;
    buf = (struct statfs_kernel *)buf_addr;

    fill_statfs_for_path(path, buf);

    return 0;
}

static int sys_fstatfs(int fd, const char *buf_ptr, int size) {
    uint32_t buf_addr = (uint32_t)buf_ptr;
    struct statfs_kernel *buf;

    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= current_task->fds_capacity) {
        return -EBADF;
    }
    if (!current_task->fds[fd].in_use) {
        return -EBADF;
    }

    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) {
        return -EFAULT;
    }

    if (size < (int)sizeof(struct statfs_kernel)) {
        return -EINVAL;
    }

    buf = (struct statfs_kernel *)buf_addr;

    fill_statfs_for_path("/", buf);

    return 0;
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
}

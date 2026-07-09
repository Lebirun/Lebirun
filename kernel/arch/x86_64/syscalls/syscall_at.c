#include "syscall_defs.h"
#include <lebirun/ramfs.h>
#include <lebirun/fs/ext4/ext4.h>

#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200
#define AT_EMPTY_PATH       0x1000

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

static int at_user_range_mapped(uint64_t addr, uint64_t size) {
    uint64_t pd;
    uint64_t start;
    uint64_t end;
    uint64_t p;
    uint64_t phys;
    uint64_t *new_user_pages;

    if (!current_task) return 0;
    if (size == 0) return 0;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return 0;
    if (addr + size < addr || addr + size > KERNEL_VMA) return 0;
    pd = current_task->cr3 ? current_task->cr3 : current_task->pml4_phys;
    if (!pd) return 0;
    start = addr & ~0xFFFu;
    end = (addr + size - 1) & ~0xFFFu;
    p = start;
    for (;;) {
        if (vmm_get_phys_in_pml4(pd, p) == 0) {
            if (!task_handle_file_page_fault(current_task, p)) {
                if ((p >= 0x00700000u && p < 0x00800000u) ||
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
        if (p == end) break;
        if (p > end) return 0;
        p += 0x1000;
    }
    return 1;
}

static int at_user_string_mapped(const char *s, size_t max) {
    uint64_t addr;
    size_t i;

    if (!s || max == 0) return 0;
    addr = (uint64_t)(uintptr_t)s;
    for (i = 0; i < max; i++) {
        if (!at_user_range_mapped(addr + i, 1)) return 0;
        if (s[i] == '\0') return 1;
    }
    return 0;
}

static const char *resolve_at_path(int dirfd, const char *pathname, char *resolved, size_t size) {
    uint64_t path_addr;
    const char *cwd;
    size_t cwd_len;
    size_t path_len;
    size_t pos;
    size_t i;
    task_fd_t *tfd;
    vfs_node_t *dir_node;
    char dir_path[256];

    if (!pathname) return NULL;
    
    path_addr = (uint64_t)(uintptr_t)pathname;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return NULL;
    if (!at_user_string_mapped(pathname, size)) return NULL;
    
    if (pathname[0] == '/') {
        return pathname;
    }
    
    if (dirfd == AT_FDCWD) {
        cwd = current_task ? current_task->cwd : "/";
        if (!cwd[0]) cwd = "/";
        
        cwd_len = 0;
        while (cwd[cwd_len]) cwd_len++;
        
        path_len = 0;
        while (pathname[path_len]) path_len++;
        
        if (cwd_len + 1 + path_len + 1 > size) return NULL;
        
        pos = 0;
        for (i = 0; i < cwd_len && pos < size - 1; i++) {
            resolved[pos++] = cwd[i];
        }
        if (pos > 0 && resolved[pos - 1] != '/' && pos < size - 1) {
            resolved[pos++] = '/';
        }
        for (i = 0; i < path_len && pos < size - 1; i++) {
            resolved[pos++] = pathname[i];
        }
        resolved[pos] = '\0';
        return resolved;
    }

    if (!current_task) return NULL;
    if (dirfd < 0 || dirfd >= current_task->fds_capacity) return NULL;
    if (!current_task->fds[dirfd].in_use) return NULL;

    tfd = &current_task->fds[dirfd];
    if (!tfd->node) return NULL;
    dir_node = (vfs_node_t *)tfd->node;

    if (vfs_get_path(dir_node, dir_path, sizeof(dir_path)) == NULL) return NULL;

    cwd_len = 0;
    while (dir_path[cwd_len]) cwd_len++;

    path_len = 0;
    while (pathname[path_len]) path_len++;

    if (cwd_len + 1 + path_len + 1 > size) return NULL;

    pos = 0;
    for (i = 0; i < cwd_len && pos < size - 1; i++) {
        resolved[pos++] = dir_path[i];
    }
    if (pos > 0 && resolved[pos - 1] != '/' && pos < size - 1) {
        resolved[pos++] = '/';
    }
    for (i = 0; i < path_len && pos < size - 1; i++) {
        resolved[pos++] = pathname[i];
    }
    resolved[pos] = '\0';
    return resolved;
}

static int sys_openat(int dirfd, const char *pathname, int flags) {
    char resolved[256];
    const char *path;
    vfs_node_t *node;
    int fd;
    uint64_t create_mode;
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

    if (node && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
        vfs_release(node);
        return -EEXIST;
    }

    if (!node && (flags & VFS_O_CREAT)) {
        create_mode = 0644;
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

    if (!node) return -ENOENT;

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
    
    {
        int r;
        r = vfs_mkdir(parent, dirname, (uint64_t)mode);
        vfs_release(parent);
        return r;
    }
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
    const char *path;
    vfs_node_t *node;

    path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    if (!current_task) return -ESRCH;

    if (current_task->euid != 0)
        return -EPERM;

    node = vfs_namei(path);
    if (!node) return -ENOENT;

    if (node->chown) {
        int r;
        r = node->chown(node, (uint64_t)owner, node->gid);
        vfs_release(node);
        return r;
    }
    if (owner != -1) node->uid = (uint64_t)owner;
    vfs_release(node);
    return 0;
}

static int sys_unlinkat(int dirfd, const char *pathname, int flags) {
    char resolved[256];
    const char *path;
    char parent_path[256];
    char filename[64];
    int len;
    int last_slash;
    int i;
    int j;
    vfs_node_t *parent;

    path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    if (!current_task) return -ESRCH;

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

    if (current_task->euid != 0) {
        uint64_t pmode;
        int pshift;
        int pallowed;

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
            return -EACCES;
        }
    }

    (void)flags;
    {
        int r;
        r = vfs_unlink(parent, filename);
        vfs_release(parent);
        return r;
    }
}

static int sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath_arg) {
    char old_resolved[256];
    char new_resolved[256];
    const char *old_path;
    const char *new_path;
    vfs_node_t *old_node;
    vfs_node_t *old_parent;
    vfs_node_t *new_parent;
    char new_parent_path[256];
    char old_name[64];
    char new_name[64];
    int len;
    int last_slash;
    int k;
    int i;

    if (!oldpath || !newpath_arg) return -EFAULT;

    old_path = resolve_at_path(olddirfd, oldpath, old_resolved, sizeof(old_resolved));
    if (!old_path) return -EFAULT;

    new_path = resolve_at_path(newdirfd, newpath_arg, new_resolved, sizeof(new_resolved));
    if (!new_path) return -EFAULT;

    old_node = vfs_namei(old_path);
    if (!old_node) return -ENOENT;

    old_parent = old_node->parent;
    if (!old_parent || !old_parent->rename) {
        vfs_release(old_node);
        return ramfs_rename(old_path, new_path) ? -ENOENT : 0;
    }

    len = 0;
    while (new_path[len]) len++;
    last_slash = -1;
    for (i = 0; i < len; i++) {
        if (new_path[i] == '/') last_slash = i;
    }

    if (last_slash <= 0) {
        new_parent_path[0] = '/';
        new_parent_path[1] = '\0';
        k = 0;
        i = (last_slash == 0) ? 1 : 0;
        while (new_path[i] && k < 63) new_name[k++] = new_path[i++];
        new_name[k] = '\0';
    } else {
        for (i = 0; i < last_slash && i < 255; i++) new_parent_path[i] = new_path[i];
        new_parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        k = 0;
        for (i = last_slash + 1; i < len && k < 63; i++) new_name[k++] = new_path[i];
        new_name[k] = '\0';
    }

    new_parent = vfs_namei(new_parent_path);
    if (!new_parent) {
        vfs_release(old_node);
        return -ENOENT;
    }

    len = 0;
    while (old_path[len]) len++;
    last_slash = -1;
    for (i = 0; i < len; i++) {
        if (old_path[i] == '/') last_slash = i;
    }
    k = 0;
    for (i = last_slash + 1; i < len && k < 63; i++) old_name[k++] = old_path[i];
    old_name[k] = '\0';

    {
        int r;
        r = old_parent->rename(old_parent, old_name, new_parent, new_name);
        vfs_release(old_node);
        vfs_release(new_parent);
        return r;
    }
}

static int sys_linkat(int olddirfd, const char *oldpath, int newdirfd) {
    (void)olddirfd; (void)oldpath; (void)newdirfd;
    return -ENOSYS;
}

static int sys_symlinkat(int target_ptr, const char *newdirfd_ptr, int linkpath) {
    char link_resolved[256];
    const char *link_path;
    const char *target;
    int newdirfd;
    int ret;

    target = (const char *)(uintptr_t)target_ptr;
    newdirfd = (int)(uintptr_t)newdirfd_ptr;

    if (!target || (uint64_t)target < 0x1000 || (uint64_t)target >= KERNEL_VMA) return -EFAULT;
    if (!linkpath || (uint64_t)(uintptr_t)linkpath < 0x1000 || (uint64_t)(uintptr_t)linkpath >= KERNEL_VMA) return -EFAULT;
    if (!at_user_string_mapped(target, VFS_MAX_PATH)) return -EFAULT;

    link_path = resolve_at_path(newdirfd, (const char *)(uintptr_t)linkpath, link_resolved, sizeof(link_resolved));
    if (!link_path) return -EFAULT;

    ret = ext4_vfs_symlink_node(target, link_path, 0);
    if (ret == 0) return 0;

    ret = ramfs_create_symlink(link_path, target, 0777);
    if (ret == 0) return 0;
    if (ret == RAMFS_ERR_EXIST) return -EEXIST;
    if (ret == RAMFS_ERR_NOENT) return -ENOENT;
    if (ret == RAMFS_ERR_NOSPC) return -ENOSPC;
    if (ret == RAMFS_ERR_NOMEM) return -ENOMEM;
    return -EIO;
}

static int sys_readlinkat(int dirfd, const char *pathname, int buf_ptr) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;

    uint64_t buf_addr = (uint64_t)buf_ptr;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;

    vfs_node_t *node = vfs_namei_nofollow(path);
    if (!node) return -ENOENT;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) {
        vfs_release(node);
        return -EINVAL;
    }

    char target[VFS_MAX_PATH];
    uint64_t n = vfs_read(node, 0, sizeof(target) - 1, (uint8_t *)target);
    vfs_release(node);
    if (n >= sizeof(target)) n = sizeof(target) - 1;
    target[n] = '\0';

    for (uint64_t i = 0; i < n; i++) {
        ((char *)buf_addr)[i] = target[i];
    }
    return (int)n;
}

static int sys_fchmodat(int dirfd, const char *pathname, int mode) {
    char resolved[256];
    const char *path;
    vfs_node_t *node;

    path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    if (!current_task) return -ESRCH;

    node = vfs_namei(path);
    if (!node) return -ENOENT;

    if (current_task->euid != 0 && current_task->euid != node->uid) {
        vfs_release(node);
        return -EPERM;
    }

    if (node->chmod) {
        int r;
        r = node->chmod(node, (uint64_t)mode & 07777);
        vfs_release(node);
        return r;
    }
    node->mask = (uint64_t)mode & 07777;
    vfs_release(node);
    return 0;
}

static int sys_faccessat(int dirfd, const char *pathname, int mode) {
    char resolved[256];
    const char *path;
    vfs_node_t *node;
    uint64_t uid;
    uint64_t gid;
    uint64_t fmode;
    int shift;
    int allowed;
    int want;

    path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    if (!current_task) return -ESRCH;

    node = vfs_namei(path);
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

static int sys_fstatat(int dirfd, const char *pathname, int statbuf) {
    char resolved[256];
    const char *path = resolve_at_path(dirfd, pathname, resolved, sizeof(resolved));
    if (!path) return -EFAULT;
    
    uint64_t buf_addr = (uint64_t)statbuf;
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    
    vfs_node_t *node = vfs_namei(path);
    if (!node) return -ENOENT;
    
    struct kernel_stat *st = (struct kernel_stat *)buf_addr;
    memset(st, 0, sizeof(struct kernel_stat));
    
    st->st_dev = 1;
    st->st_ino = node->inode ? node->inode : 1;

    uint64_t perms = 0;
    if (node->mask) {
        if ((node->mask & ~0x07u) == 0) {
            if (node->mask & VFS_PERM_READ) perms |= 0444;
            if (node->mask & VFS_PERM_WRITE) perms |= 0222;
            if (node->mask & VFS_PERM_EXEC) perms |= 0111;
        } else {
            perms = node->mask & 07777u;
        }
    }

    uint64_t mode;
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
    
    vfs_release(node);
    return 0;
}

static int sys_utimensat(int dirfd, const char *pathname, int times_ptr) {
    (void)dirfd; (void)pathname; (void)times_ptr;
    return 0;
}

static int sys_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    return sys_renameat(olddirfd, oldpath, newdirfd, newpath);
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

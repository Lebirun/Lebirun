#include <kernel/vfs.h>
#include <kernel/tty.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/debug.h>
#include <kernel/mem_map.h>
#include <kernel/ramfs.h>
#include <string.h>
#include <stddef.h>

static vfs_node_t *vfs_root = NULL;
static vfs_fs_type_t *registered_fs = NULL;
static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static vfs_fd_t fd_table[VFS_MAX_FDS];
static mutex_t vfs_lock;
static int squashfs_access_blocked = 0;

static vfs_node_t root_node;
static dirent_t root_dirent;

static dirent_t *root_readdir(vfs_node_t *node, uint32_t index);
static vfs_node_t *root_finddir(vfs_node_t *node, const char *name);

void vfs_init(void) {
    int i;
    
    mutex_init(&vfs_lock);
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].in_use = 0;
        mounts[i].path[0] = '\0';
        mounts[i].device[0] = '\0';
        mounts[i].root = NULL;
        mounts[i].fs_type = NULL;
    }
    
    for (i = 0; i < VFS_MAX_FDS; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].node = NULL;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }
    
    memset(&root_node, 0, sizeof(vfs_node_t));
    strcpy(root_node.name, "/");
    root_node.flags = VFS_DIRECTORY;
    root_node.mask = VFS_PERM_READ | VFS_PERM_EXEC;
    root_node.readdir = root_readdir;
    root_node.finddir = root_finddir;
    root_node.ref_count = 1;
    root_node.parent = NULL;
    
    vfs_root = &root_node;
    
    printf("[VFS] Virtual Filesystem initialized\n");
}

int vfs_register_fs(vfs_fs_type_t *fs) {
    vfs_fs_type_t *cur;
    
    if (!fs) {
        printf("[VFS] ERROR: NULL filesystem struct\n");
        return -1;
    }
    
    if (!fs->name) {
        printf("[VFS] ERROR: Filesystem has NULL name\n");
        return -1;
    }
    
    if ((uintptr_t)fs->name < 0x1000 || (uintptr_t)fs->name > 0xFFFFFFFF) {
        printf("[VFS] ERROR: Invalid name pointer: %p\n", (void*)fs->name);
        return -1;
    }
    
    cur = registered_fs;
    while (cur) {
        if (strcmp(cur->name, fs->name) == 0) {
            printf("[VFS] WARNING: Filesystem '%s' already registered\n", fs->name);
            return -1;
        }
        cur = cur->next;
    }
    
    fs->next = registered_fs;
    registered_fs = fs;
    
    printf("[VFS] Registered filesystem: %s\n", fs->name);
    return 0;
}

int vfs_unregister_fs(const char *name) {
    if (!name) return -1;
    
    vfs_fs_type_t **pp = &registered_fs;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            *pp = (*pp)->next;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

static int vfs_fs_type_valid(vfs_fs_type_t *fs) {
    uintptr_t p;
    uintptr_t n;
    if (!fs) return 0;
    p = (uintptr_t)fs;
    if (p < 0x1000) return 0;
    n = (uintptr_t)fs->name;
    if (n < 0x1000) return 0;
    return 1;
}

vfs_fs_type_t *vfs_find_fs(const char *name) {
    vfs_fs_type_t *cur;
    int iterations = 0;
    if (!name) return NULL;
    cur = registered_fs;
    while (cur) {
        if (++iterations > 100) {
            printf("[VFS] ERROR: vfs_find_fs loop detected after %d iterations!\n", iterations);
            return NULL;
        }
        if (!vfs_fs_type_valid(cur)) {
            printf("[VFS] ERROR: Invalid fs_type at %p (iter %d)\n", (void*)cur, iterations);
            return NULL;
        }
        if (strcmp(cur->name, name) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

int vfs_mount(const char *device, const char *mountpoint, const char *fs_type) {
    vfs_node_t *existing;
    int slot;
    int i;
    vfs_fs_type_t *fs;
    vfs_node_t *root;
    size_t cp;
    const char *end;
    const char *base;
    size_t n;
    size_t di;
    
    
    if (!mountpoint || !fs_type) {
        return -1;
    }
    
    fs = vfs_find_fs(fs_type);
    if (!fs || !vfs_fs_type_valid(fs)) {
        return -1;
    }
    
    if (!fs->mount) {
        return -1;
    }
    
    if ((uintptr_t)fs->mount < 0x1000) {
        return -1;
    }
    
    slot = -1;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        printf("[VFS] Mount table full\n");
        return -1;
    }
    
    root = fs->mount(device, mountpoint);
    if (!root) {
        return -1;
    }
    
    existing = vfs_namei(mountpoint);
    if (existing) {
        root->flags |= VFS_MOUNTPOINT;
        root->ptr = existing;
    }

    mounts[slot].in_use = 1;
    cp = 0;
    while (mountpoint[cp] && cp < VFS_MAX_PATH - 1) { mounts[slot].path[cp] = mountpoint[cp]; cp++; }
    mounts[slot].path[cp] = '\0';
    mounts[slot].root = root;
    mounts[slot].fs_type = fs;
    if (device && device[0] != '\0') {
        di = 0;
        while (device[di] && di < VFS_MAX_PATH - 1) {
            mounts[slot].device[di] = device[di];
            ++di;
        }
        mounts[slot].device[di] = '\0';
    } else {
        mounts[slot].device[0] = '\0';
    }
    
    root->parent = vfs_root;
    
    if (mountpoint[0] == '/' && !(mountpoint[0] == '/' && mountpoint[1] == '\0')) {
        end = mountpoint + strlen(mountpoint);
        while (end > mountpoint + 1 && end[-1] == '/') {
            end--;
        }
        base = end;
        while (base > mountpoint && base[-1] != '/') {
            base--;
        }
        if (base < end) {
            n = (size_t)(end - base);
            if (n >= VFS_MAX_NAME) {
                n = VFS_MAX_NAME - 1;
            }
            memcpy(root->name, base, n);
            root->name[n] = '\0';
            
            existing = root_finddir(vfs_root, root->name);
            if (existing) {
                root->ptr = existing;
                root->flags |= VFS_MOUNTPOINT;
            }
        }
    }
    
    printf("[VFS] Mounted %s on %s (type: %s)\n", 
           device ? device : "(none)", mountpoint, fs_type);
    
    return 0;
}

int vfs_unmount(const char *mountpoint) {
    int i;
    
    if (!mountpoint) return -1;
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            if (mounts[i].fs_type && mounts[i].fs_type->unmount) {
                mounts[i].fs_type->unmount(mounts[i].root);
            }
            
            mounts[i].in_use = 0;
            mounts[i].path[0] = '\0';
            mounts[i].root = NULL;
            mounts[i].fs_type = NULL;
            mounts[i].device[0] = '\0';
            
            printf("[VFS] Unmounted %s\n", mountpoint);
            return 0;
        }
    }
    
    return -1; 
}

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || !buffer) return 0;
    
    if (node->read) {
        return node->read(node, offset, size, buffer);
    }
    
    return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || !buffer) return 0;
    
    if (node->write) {
        return node->write(node, offset, size, buffer);
    }
    
    return 0;
}

void vfs_open(vfs_node_t *node, uint32_t flags) {
    if (!node) return;
    node->ref_count++;
    if (node->open) node->open(node, flags);
}

void vfs_close(vfs_node_t *node) {
    if (!node) return;
    
    if (node->ref_count > 0) {
        node->ref_count--;
    }
    
    if (node->close) {
        node->close(node);
    }
}

dirent_t *vfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!node) return NULL;

    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY &&
        (node->flags & VFS_MOUNTPOINT) == 0) {
        return NULL;
    }
    
    if (node->readdir) {
        return node->readdir(node, index);
    }
    
    return NULL;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    if (!node || !name) return NULL;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY && (node->flags & VFS_MOUNTPOINT) == 0) return NULL;
    if (node->finddir) {
        if ((uintptr_t)node->finddir < 0x1000) {
            return NULL;
        }
        vfs_node_t *result = node->finddir(node, name);
        return result;
    }
    return NULL;
}

int vfs_create(vfs_node_t *parent, const char *name, uint32_t flags) {
    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->create) return parent->create(parent, name, flags);
    return -1;
}

int vfs_unlink(vfs_node_t *parent, const char *name) {
    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->unlink) return parent->unlink(parent, name);
    return -1;
}

int vfs_mkdir(vfs_node_t *parent, const char *name, uint32_t perms) {
    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->mkdir) return parent->mkdir(parent, name, perms);
    return -1;
}

static vfs_mount_t *find_mount_for_path(const char *path) {
    vfs_mount_t *best;
    size_t best_len;
    int i;
    size_t len;
    
    best = NULL;
    best_len = 0;
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        
        len = strlen(mounts[i].path);
        if (strncmp(path, mounts[i].path, len) == 0) {
            if (path[len] == '\0' || path[len] == '/' || 
                (len == 1 && mounts[i].path[0] == '/')) {
                if (len > best_len) {
                    best = &mounts[i];
                    best_len = len;
                }
            }
        }
    }
    
    return best;
}

static int vfs_resolve_path(const char *path, char *resolved, size_t size) {
    size_t i;
    const char *cwd;
    size_t cwd_len;
    size_t path_len;
    size_t pos;
    
    if (!path || !resolved || size == 0) return -1;
    
    if (path[0] == '/') {
        i = 0;
        while (path[i] && i < size - 1) { resolved[i] = path[i]; i++; }
        resolved[i] = '\0';
        return 0;
    }
    
    cwd = "/";
    if (current_task && current_task->cwd[0]) {
        cwd = current_task->cwd;
    }
    
    cwd_len = 0;
    while (cwd[cwd_len]) cwd_len++;
    
    path_len = 0;
    while (path[path_len]) path_len++;
    
    pos = 0;
    for (i = 0; i < cwd_len && pos < size - 1; i++) {
        resolved[pos++] = cwd[i];
    }

    if (pos > 0 && resolved[pos - 1] != '/' && pos < size - 1) {
        resolved[pos++] = '/';
    }
    
    for (i = 0; i < path_len && pos < size - 1; i++) {
        resolved[pos++] = path[i];
    }
    resolved[pos] = '\0';
    
    return 0;
}

static void vfs_normalize_path(char *path) {
    char *src;
    char *dst;
    char *components[64];
    int count;
    char *comp_start;
    size_t comp_len;
    int i;
    char *comp;
    
    if (!path || path[0] != '/') return;

    src = path;
    dst = path;
    count = 0;

    while (*src) {
        while (*src == '/') src++;
        if (*src == '\0') break;

        comp_start = src;
        while (*src && *src != '/') src++;
        comp_len = src - comp_start;

        if (comp_len == 1 && comp_start[0] == '.') {
            continue;
        } else if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            if (count > 0) count--;
            continue;
        }

        if (count < 64) {
            components[count++] = comp_start;
            if (*src) *src++ = '\0';
        }
    }

    dst = path;
    *dst++ = '/';
    for (i = 0; i < count; i++) {
        comp = components[i];
        while (*comp) *dst++ = *comp++;
        if (i < count - 1) *dst++ = '/';
    }
    *dst = '\0';

    if (path[1] == '\0') return;
}

#define VFS_MAX_SYMLINKS 8

static int vfs_readlink_node(vfs_node_t *node, char *buf, size_t size) {
    if (!node || !buf || size == 0) return -1;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) return -1;

    uint32_t n = vfs_read(node, 0, (uint32_t)(size - 1), (uint8_t *)buf);
    if (n >= size) n = (uint32_t)(size - 1);
    buf[n] = '\0';
    if (n == 0) return -1;
    return (int)n;
}

static int vfs_build_symlink_path(char *out, size_t outsz,
                                  const char *base_dir,
                                  const char *target,
                                  const char *rest_raw) {
    char tmp[VFS_MAX_PATH];
    size_t len;
    size_t i;
    
    if (!out || outsz == 0 || !base_dir || !target || !rest_raw) return -1;

    tmp[0] = '\0';

    if (target[0] == '/') {
        strncpy(tmp, target, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        if (strcmp(base_dir, "/") == 0) {
            snprintf(tmp, sizeof(tmp), "/%s", target);
        } else {
            snprintf(tmp, sizeof(tmp), "%s/%s", base_dir, target);
        }
    }

    if (rest_raw[0] != '\0') {
        len = strlen(tmp);
        if (len + strlen(rest_raw) + 1 >= sizeof(tmp)) return -1;
        for (i = 0; rest_raw[i] && len + i + 1 < sizeof(tmp); i++) {
            tmp[len + i] = rest_raw[i];
            tmp[len + i + 1] = '\0';
        }
    }

    strncpy(out, tmp, outsz - 1);
    out[outsz - 1] = '\0';
    return 0;
}

static vfs_node_t *vfs_namei_internal(const char *in_path, int follow_final, int depth) {
    char resolved[VFS_MAX_PATH];
    char prefix[VFS_MAX_PATH];
    char component[VFS_MAX_NAME];
    char target[VFS_MAX_PATH];
    char newpath[VFS_MAX_PATH];
    char new_prefix[VFS_MAX_PATH];
    const char *path;
    const char *remaining;
    const char *rest_raw;
    const char *rest_non_slash;
    vfs_mount_t *mount;
    vfs_node_t *node;
    vfs_node_t *next;
    char *last;
    size_t plen;
    int i;
    int has_more;

    if (!in_path) return NULL;
    if (depth > VFS_MAX_SYMLINKS) return NULL;

    path = in_path;

    if (path[0] != '/') {
        if (vfs_resolve_path(path, resolved, sizeof(resolved)) < 0) return NULL;
        vfs_normalize_path(resolved);
        path = resolved;
    } else {
        i = 0;
        while (path[i] && (size_t)i < sizeof(resolved) - 1) {
            resolved[i] = path[i];
            i++;
        }
        resolved[i] = '\0';
        vfs_normalize_path(resolved);
        path = resolved;
    }

    if (path[0] != '/') return NULL;

    if (squashfs_access_blocked &&
        path[0] == '/' && path[1] == 's' && path[2] == 'q' &&
        path[3] == 'u' && path[4] == 'a' && path[5] == 's' &&
        path[6] == 'h' && path[7] == 'f' && path[8] == 's' &&
        (path[9] == '\0' || path[9] == '/')) {
        return NULL;
    }

    if (path[0] == '/' && path[1] == '\0') {
        return vfs_root;
    }

    mount = find_mount_for_path(path);
    prefix[0] = '\0';

    if (mount && mount->root) {
        node = mount->root;
        remaining = path + strlen(mount->path);
        if (*remaining == '/') remaining++;
        strncpy(prefix, mount->path, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
        plen = strlen(prefix);
        if (plen > 1 && prefix[plen - 1] == '/') prefix[plen - 1] = '\0';
    } else {
        node = vfs_root;
        remaining = path + 1;
        strncpy(prefix, "/", sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
    }

    if (*remaining == '\0') return node;

    while (*remaining) {
        while (*remaining == '/') remaining++;
        if (*remaining == '\0') break;

        i = 0;
        while (*remaining && *remaining != '/' && i < VFS_MAX_NAME - 1) {
            component[i++] = *remaining++;
        }
        component[i] = '\0';

        if (i == 0) continue;

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (node->parent) {
                node = node->parent;
                if (strcmp(prefix, "/") != 0) {
                    last = strrchr(prefix, '/');
                    if (last) {
                        if (last == prefix) {
                            prefix[1] = '\0';
                        } else {
                            *last = '\0';
                        }
                    }
                }
            }
            continue;
        }

        rest_raw = remaining;
        rest_non_slash = rest_raw;
        while (*rest_non_slash == '/') rest_non_slash++;
        has_more = (*rest_non_slash != '\0');

        next = vfs_finddir(node, component);
        if (!next) return NULL;

        if ((next->flags & VFS_MOUNTPOINT) && next->ptr) {
            next = next->ptr;
        }

        if (VFS_GET_TYPE(next->flags) == VFS_SYMLINK && (has_more || follow_final)) {
            if (vfs_readlink_node(next, target, sizeof(target)) >= 0) {
                if (vfs_build_symlink_path(newpath, sizeof(newpath), prefix, target, rest_raw) < 0) return NULL;
                vfs_normalize_path(newpath);
                return vfs_namei_internal(newpath, follow_final, depth + 1);
            }
        }

        node = next;

        if (strcmp(prefix, "/") == 0) {
            snprintf(new_prefix, sizeof(new_prefix), "/%s", component);
        } else {
            snprintf(new_prefix, sizeof(new_prefix), "%s/%s", prefix, component);
        }
        strncpy(prefix, new_prefix, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
    }

    return node;
}

vfs_node_t *vfs_namei(const char *path) {
    vfs_node_t *result;

    result = vfs_namei_internal(path, 1, 0);
    return result;
}

void vfs_block_squashfs_access(void) {
    squashfs_access_blocked = 1;
}

vfs_node_t *vfs_namei_nofollow(const char *path) {
    return vfs_namei_internal(path, 0, 0);
}

vfs_node_t *vfs_lookup(const char *path) {
    return vfs_namei(path);
}

char *vfs_get_path(vfs_node_t *node, char *buf, size_t size) {
    if (!node || !buf || size == 0) return NULL;
    
    if (node == vfs_root) {
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }
    
    char temp[VFS_MAX_PATH];
    int pos = VFS_MAX_PATH - 1;
    temp[pos] = '\0';
    
    vfs_node_t *cur = node;
    while (cur && cur != vfs_root) {
        size_t len = strlen(cur->name);
        pos -= len;
        if (pos < 1) return NULL;
        memcpy(&temp[pos], cur->name, len);
        temp[--pos] = '/';
        cur = cur->parent;
    }
    
    if (pos == VFS_MAX_PATH - 1) temp[--pos] = '/';
    
    size_t pathlen = VFS_MAX_PATH - pos;
    if (pathlen >= size) return NULL;
    
    memcpy(buf, &temp[pos], pathlen);
    return buf;
}

static int vfs_split_path(const char *path, char *parent_buf, size_t parent_size, 
                          char *name_buf, size_t name_size) {
    int len;
    int last_slash;
    int i;
    int j;
    int k;
    
    if (!path || !parent_buf || !name_buf) return -1;
    
    len = 0;
    while (path[len]) len++;
    
    last_slash = -1;
    for (i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash < 0) {
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
        j = 0;
        while (path[j] && j < (int)name_size - 1) { name_buf[j] = path[j]; j++; }
        name_buf[j] = '\0';
    } else if (last_slash == 0) {
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
        j = 0;
        for (i = 1; i < len && j < (int)name_size - 1; i++, j++) name_buf[j] = path[i];
        name_buf[j] = '\0';
    } else {
        i = 0;
        while (i < last_slash && i < (int)parent_size - 1) { parent_buf[i] = path[i]; i++; }
        parent_buf[i] = '\0';
        j = 0;
        for (k = last_slash + 1; k < len && j < (int)name_size - 1; k++, j++) name_buf[j] = path[k];
        name_buf[j] = '\0';
    }
    
    return 0;
}

int vfs_open_path(const char *path, int flags) {
    if (!path) return -1;
    
    vfs_node_t *node;
    char parent_path[VFS_MAX_PATH];
    char filename[VFS_MAX_NAME];
    vfs_node_t *parent;
    int ret;
    int fd;
    int i;
    
    node = vfs_namei(path);
    
    if (!node && (flags & VFS_O_CREAT)) {
        if (vfs_split_path(path, parent_path, sizeof(parent_path), 
                           filename, sizeof(filename)) < 0) {
            return -1;
        }
        
        parent = vfs_namei(parent_path);
        if (!parent) return -1;
        
        ret = vfs_create(parent, filename, VFS_FILE);
        if (ret < 0 && !(flags & VFS_O_EXCL)) {
            node = vfs_namei(path);
        } else if (ret == 0) {
            node = vfs_namei(path);
        }
        
        if (!node) return -1;
    }
    
    if (!node) return -1;
    
    if ((flags & VFS_O_TRUNC) && node->truncate) {
        node->truncate(node, 0);
    }
    
    mutex_lock(&vfs_lock);
    
    fd = -1;
    for (i = 3; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            fd = i;
            break;
        }
    }
    
    if (fd < 0) {
        mutex_unlock(&vfs_lock);
        return -1;
    }
    
    vfs_open(node, flags);
    
    fd_table[fd].in_use = 1;
    fd_table[fd].node = node;
    fd_table[fd].offset = (flags & VFS_O_APPEND) ? node->length : 0;
    fd_table[fd].flags = flags;
    
    mutex_unlock(&vfs_lock);
    return fd;
}

int vfs_close_fd(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    
    mutex_lock(&vfs_lock);
    if (!fd_table[fd].in_use) {
        mutex_unlock(&vfs_lock);
        return -1;
    }
    
    vfs_node_t *node = fd_table[fd].node;
    
    fd_table[fd].in_use = 0;
    fd_table[fd].node = NULL;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = 0;
    mutex_unlock(&vfs_lock);
    
    if (node) vfs_close(node);
    
    return 0;
}

int vfs_read_fd(int fd, void *buffer, uint32_t size) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;
    if (!buffer || size == 0) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    
    uint32_t bytes = vfs_read(node, fd_table[fd].offset, size, (uint8_t *)buffer);
    fd_table[fd].offset += bytes;
    
    return (int)bytes;
}

int vfs_write_fd(int fd, const void *buffer, uint32_t size) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;
    if (!buffer || size == 0) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    
    uint32_t bytes = vfs_write(node, fd_table[fd].offset, size, (uint8_t *)buffer);
    fd_table[fd].offset += bytes;
    
    return (int)bytes;
}

int vfs_seek(int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    
    int32_t new_offset;
    
    switch (whence) {
        case VFS_SEEK_SET:
            new_offset = offset;
            break;
        case VFS_SEEK_CUR:
            new_offset = (int32_t)fd_table[fd].offset + offset;
            break;
        case VFS_SEEK_END:
            new_offset = (int32_t)node->length + offset;
            break;
        default:
            return -1;
    }
    
    if (new_offset < 0) return -1;
    
    fd_table[fd].offset = (uint32_t)new_offset;
    return (int)fd_table[fd].offset;
}

int vfs_tell(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;
    
    return (int)fd_table[fd].offset;
}

int vfs_stat_fd(int fd, uint32_t *size, uint32_t *flags) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    
    if (size) *size = node->length;
    if (flags) *flags = node->flags;
    
    DEBUG_VFS("VFS_STAT: fd=%d node=%p name='%s' length=%u flags=0x%X\n", 
           fd, node, node->name, node->length, node->flags);
    
    return 0;
}

int vfs_readdir_fd(int fd, dirent_t *entry, uint32_t index) {
    vfs_node_t *node;
    dirent_t *result;

    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use || !entry) return -1;
    
    node = fd_table[fd].node;
    if (!node) return -1;
    
    result = vfs_readdir(node, index);
    if (!result) return -1;
    
    memcpy(entry, result, sizeof(dirent_t));
    return 0;
}

vfs_node_t *vfs_get_root(void) {
    int i;
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0) {
            return mounts[i].root;
        }
    }
    return vfs_root;
}

int vfs_replace_mount_root(const char *mountpoint, vfs_node_t *new_root) {
    int i;

    if (!mountpoint || !new_root) return -1;

    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            mounts[i].root = new_root;
            return 0;
        }
    }
    return -1;
}

int vfs_get_mount_count(void) {
    int count;
    int i;
    
    count = 0;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) count++;
    }
    return count;
}

vfs_mount_t *vfs_get_mount(int index) {
    int count;
    int i;
    
    count = 0;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) {
            if (count == index) return &mounts[i];
            count++;
        }
    }
    return NULL;
}

void vfs_list_mounts(void) {
    int i;
    
    printf("[VFS] Mount table:\n");
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) {
            printf("  %s -> %s\n", 
                   mounts[i].path,
                   mounts[i].fs_type ? mounts[i].fs_type->name : "(unknown)");
        }
    }
}

static dirent_t *root_readdir(vfs_node_t *node, uint32_t index) {
    uint32_t count;
    int i;
    int j;
    const char *path;
    
    (void)node;
    
    count = 0;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") != 0) {
            if (strcmp(mounts[i].path, "/ro") == 0) {
                continue;
            }
            if (strcmp(mounts[i].path, "/squashfs") == 0) {
                continue;
            }
            if (count == index) {
                path = mounts[i].path;
                if (path[0] == '/') path++;
                
                j = 0;
                while (path[j] && path[j] != '/' && j < VFS_MAX_NAME - 1) {
                    root_dirent.name[j] = path[j];
                    j++;
                }
                root_dirent.name[j] = '\0';
                root_dirent.inode = i;
                root_dirent.type = VFS_DIRECTORY;
                
                return &root_dirent;
            }
            count++;
        }
    }
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0 && mounts[i].root) {
            if (mounts[i].root->readdir) {
                uint32_t ramfs_idx;
                uint32_t ramfs_count;
                uint32_t target;
                dirent_t *entry;
                char mount_path[VFS_MAX_PATH];
                int is_dup;
                int k;

                target = index - count;
                ramfs_count = 0;
                for (ramfs_idx = 0; ; ramfs_idx++) {
                    entry = mounts[i].root->readdir(mounts[i].root, ramfs_idx);
                    if (!entry) return NULL;

                    mount_path[0] = '/';
                    j = 0;
                    while (entry->name[j] && j < VFS_MAX_PATH - 2) {
                        mount_path[1 + j] = entry->name[j];
                        j++;
                    }
                    mount_path[1 + j] = '\0';

                    is_dup = 0;
                    for (k = 0; k < VFS_MAX_MOUNTS; k++) {
                        if (mounts[k].in_use && strcmp(mounts[k].path, mount_path) == 0) {
                            is_dup = 1;
                            break;
                        }
                    }
                    if (is_dup) continue;

                    if (ramfs_count == target) {
                        return entry;
                    }
                    ramfs_count++;
                }
            }
            break;
        }
    }
    
    return NULL;
}

static vfs_node_t *root_finddir(vfs_node_t *node, const char *name) {
    char search_path[VFS_MAX_PATH];
    size_t _ci;
    int i;
    size_t len;
    vfs_node_t *found;
    
    (void)node;
    
    if (strcmp(name, "ro") == 0) {
        return NULL;
    }
    if (strcmp(name, "squashfs") == 0) {
        return NULL;
    }
    
    search_path[0] = '/';
    _ci = 0;
    while (name[_ci] && _ci < VFS_MAX_PATH - 2) { search_path[1 + _ci] = name[_ci]; _ci++; }
    search_path[1 + _ci] = '\0';
    
    if (strcmp(search_path, "/ro") == 0) {
        return NULL;
    }
    if (strcmp(search_path, "/squashfs") == 0) {
        return NULL;
    }
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        
        if (strcmp(mounts[i].path, "/ro") == 0) {
            continue;
        }
        if (strcmp(mounts[i].path, "/squashfs") == 0) {
            continue;
        }
        
        if (strcmp(mounts[i].path, search_path) == 0) {
            return mounts[i].root;
        }
        
        len = strlen(search_path);
        if (strncmp(mounts[i].path, search_path, len) == 0 &&
            (mounts[i].path[len] == '/' || mounts[i].path[len] == '\0')) {
            return mounts[i].root;
        }
    }
    
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        if (strcmp(mounts[i].path, "/") == 0 && mounts[i].root) {
            vfs_node_t *root = mounts[i].root;
            if ((uintptr_t)root < 0x1000) {
                return NULL;
            }
            if (root->finddir) {
                if ((uintptr_t)root->finddir < 0x1000) {
                    return NULL;
                }
                found = root->finddir(root, name);
                if (found) return found;
            }
            break;
        }
    }
    
    return NULL;
}

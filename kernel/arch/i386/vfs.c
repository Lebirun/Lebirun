#include <kernel/vfs.h>
#include <kernel/tty.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/debug.h>
#include <string.h>
#include <stddef.h>

static vfs_node_t *vfs_root = NULL;
static vfs_fs_type_t *registered_fs = NULL;
static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static vfs_fd_t fd_table[VFS_MAX_FDS];
static mutex_t vfs_lock;

static vfs_node_t root_node;
static dirent_t root_dirent;

static dirent_t *root_readdir(vfs_node_t *node, uint32_t index);
static vfs_node_t *root_finddir(vfs_node_t *node, const char *name);

void vfs_init(void) {
    mutex_init(&vfs_lock);
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].in_use = 0;
        mounts[i].path[0] = '\0';
        mounts[i].root = NULL;
        mounts[i].fs_type = NULL;
    }
    
    for (int i = 0; i < VFS_MAX_FDS; i++) {
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
    if (!fs || !fs->name) {
        return -1;
    }
    
    vfs_fs_type_t *cur = registered_fs;
    while (cur) {
        if (strcmp(cur->name, fs->name) == 0) {
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

vfs_fs_type_t *vfs_find_fs(const char *name) {
    vfs_fs_type_t *cur = registered_fs;
    while (cur) {
        if (strcmp(cur->name, name) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

int vfs_mount(const char *device, const char *mountpoint, const char *fs_type) {
    if (!mountpoint || !fs_type) {
        return -1;
    }
    
    vfs_fs_type_t *fs = vfs_find_fs(fs_type);
    if (!fs) {
        printf("[VFS] Unknown filesystem type: %s\n", fs_type);
        return -1;
    }
    
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        printf("[VFS] Mount table full\n");
        return -1;
    }
    
    vfs_node_t *root = NULL;
    if (fs->mount) {
        root = fs->mount(device, mountpoint);
        if (!root) {
            printf("[VFS] Failed to mount %s on %s\n", fs_type, mountpoint);
            return -1;
        }
    }

    mounts[slot].in_use = 1;
    size_t cp = 0;
    while (mountpoint[cp] && cp < VFS_MAX_PATH - 1) { mounts[slot].path[cp] = mountpoint[cp]; cp++; }
    mounts[slot].path[cp] = '\0';
    mounts[slot].root = root;
    mounts[slot].fs_type = fs;
    
    if (root) root->parent = vfs_root;
    
    printf("[VFS] Mounted %s on %s (type: %s)\n", 
           device ? device : "(none)", mountpoint, fs_type);
    
    return 0;
}

int vfs_unmount(const char *mountpoint) {
    if (!mountpoint) return -1;
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            if (mounts[i].fs_type && mounts[i].fs_type->unmount) {
                mounts[i].fs_type->unmount(mounts[i].root);
            }
            
            mounts[i].in_use = 0;
            mounts[i].path[0] = '\0';
            mounts[i].root = NULL;
            mounts[i].fs_type = NULL;
            
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
    if (node->finddir) return node->finddir(node, name);
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
    vfs_mount_t *best = NULL;
    size_t best_len = 0;
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        
        size_t len = strlen(mounts[i].path);
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
    if (!path || !resolved || size == 0) return -1;
    
    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i < size - 1) { resolved[i] = path[i]; i++; }
        resolved[i] = '\0';
        return 0;
    }
    
    const char *cwd = "/";
    if (current_task && current_task->cwd[0]) {
        cwd = current_task->cwd;
    }
    
    size_t cwd_len = 0;
    while (cwd[cwd_len]) cwd_len++;
    
    size_t path_len = 0;
    while (path[path_len]) path_len++;
    
    size_t pos = 0;
    for (size_t i = 0; i < cwd_len && pos < size - 1; i++) {
        resolved[pos++] = cwd[i];
    }

    if (pos > 0 && resolved[pos - 1] != '/' && pos < size - 1) {
        resolved[pos++] = '/';
    }
    
    for (size_t i = 0; i < path_len && pos < size - 1; i++) {
        resolved[pos++] = path[i];
    }
    resolved[pos] = '\0';
    
    return 0;
}

static void vfs_normalize_path(char *path) {
    if (!path || path[0] != '/') return;
    
    char *src = path;
    char *dst = path;
    char *components[64];
    int count = 0;
    
    while (*src) {
        while (*src == '/') src++;
        if (*src == '\0') break;
        
        char *comp_start = src;
        while (*src && *src != '/') src++;
        size_t comp_len = src - comp_start;
        
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
    for (int i = 0; i < count; i++) {
        char *comp = components[i];
        while (*comp) *dst++ = *comp++;
        if (i < count - 1) *dst++ = '/';
    }
    *dst = '\0';
    
    if (path[1] == '\0') return;
}

vfs_node_t *vfs_namei(const char *path) {
    if (!path) return NULL;
    
    char resolved[VFS_MAX_PATH];
    if (path[0] != '/') {
        if (vfs_resolve_path(path, resolved, sizeof(resolved)) < 0) return NULL;
        vfs_normalize_path(resolved);
        path = resolved;
    }
    
    if (path[0] != '/') return NULL;
    if (path[0] == '/' && path[1] == '\0') return vfs_root;
    
    vfs_mount_t *mount = find_mount_for_path(path);
    vfs_node_t *node;
    const char *remaining;
    
    if (mount && mount->root) {
        node = mount->root;
        remaining = path + strlen(mount->path);
        if (*remaining == '/') remaining++;
    } else {
        node = vfs_root;
        remaining = path + 1;
    }
    
    if (*remaining == '\0') return node;
    
    char component[VFS_MAX_NAME];
    while (*remaining) {
        while (*remaining == '/') remaining++;
        if (*remaining == '\0') break;
        
        int i = 0;
        while (*remaining && *remaining != '/' && i < VFS_MAX_NAME - 1) {
            component[i++] = *remaining++;
        }
        component[i] = '\0';
        
        if (i == 0) continue;
        
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (node->parent) node = node->parent;
            continue;
        }
        
        vfs_node_t *next = vfs_finddir(node, component);
        if (!next) return NULL;
        
        if ((next->flags & VFS_MOUNTPOINT) && next->ptr) {
            next = next->ptr;
        }
        
        node = next;
    }
    
    return node;
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
    if (!path || !parent_buf || !name_buf) return -1;
    
    int len = 0;
    while (path[len]) len++;
    
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash < 0) {
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
        int j = 0;
        while (path[j] && j < (int)name_size - 1) { name_buf[j] = path[j]; j++; }
        name_buf[j] = '\0';
    } else if (last_slash == 0) {
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
        int j = 0;
        for (int i = 1; i < len && j < (int)name_size - 1; i++, j++) name_buf[j] = path[i];
        name_buf[j] = '\0';
    } else {
        int i = 0;
        while (i < last_slash && i < (int)parent_size - 1) { parent_buf[i] = path[i]; i++; }
        parent_buf[i] = '\0';
        int j = 0;
        for (int k = last_slash + 1; k < len && j < (int)name_size - 1; k++, j++) name_buf[j] = path[k];
        name_buf[j] = '\0';
    }
    
    return 0;
}

int vfs_open_path(const char *path, int flags) {
    if (!path) return -1;
    
    vfs_node_t *node = vfs_namei(path);
    
    if (!node && (flags & VFS_O_CREAT)) {
        char parent_path[VFS_MAX_PATH];
        char filename[VFS_MAX_NAME];
        
        if (vfs_split_path(path, parent_path, sizeof(parent_path), 
                           filename, sizeof(filename)) < 0) {
            return -1;
        }
        
        vfs_node_t *parent = vfs_namei(parent_path);
        if (!parent) return -1;
        
        int ret = vfs_create(parent, filename, VFS_FILE);
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
    
    int fd = -1;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
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
    
    DPRINTF2("VFS_STAT: fd=%d node=%p name='%s' length=%u flags=0x%X\n", 
           fd, node, node->name, node->length, node->flags);
    
    return 0;
}

int vfs_readdir_fd(int fd, dirent_t *entry, uint32_t index) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    if (!fd_table[fd].in_use || !entry) return -1;
    
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;
    
    dirent_t *result = vfs_readdir(node, index);
    if (!result) return -1;
    
    memcpy(entry, result, sizeof(dirent_t));
    return 0;
}

vfs_node_t *vfs_get_root(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0) {
            return mounts[i].root;
        }
    }
    return vfs_root;
}

int vfs_get_mount_count(void) {
    int count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) count++;
    }
    return count;
}

vfs_mount_t *vfs_get_mount(int index) {
    int count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) {
            if (count == index) return &mounts[i];
            count++;
        }
    }
    return NULL;
}

void vfs_list_mounts(void) {
    printf("[VFS] Mount table:\n");
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) {
            printf("  %s -> %s\n", 
                   mounts[i].path,
                   mounts[i].fs_type ? mounts[i].fs_type->name : "(unknown)");
        }
    }
}

static dirent_t *root_readdir(vfs_node_t *node, uint32_t index) {
    (void)node;
    
    uint32_t count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") != 0) {
            if (count == index) {
                const char *path = mounts[i].path;
                if (path[0] == '/') path++;
                
                int j = 0;
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
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0 && mounts[i].root) {
            if (mounts[i].root->readdir) {
                return mounts[i].root->readdir(mounts[i].root, index - count);
            }
            break;
        }
    }
    
    return NULL;
}

static vfs_node_t *root_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    
    char search_path[VFS_MAX_PATH];
    search_path[0] = '/';
    size_t _ci = 0;
    while (name[_ci] && _ci < VFS_MAX_PATH - 2) { search_path[1 + _ci] = name[_ci]; _ci++; }
    search_path[1 + _ci] = '\0';
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        
        if (strcmp(mounts[i].path, search_path) == 0) {
            return mounts[i].root;
        }
        
        size_t len = strlen(search_path);
        if (strncmp(mounts[i].path, search_path, len) == 0 &&
            (mounts[i].path[len] == '/' || mounts[i].path[len] == '\0')) {
            return mounts[i].root;
        }
    }
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        if (strcmp(mounts[i].path, "/") == 0 && mounts[i].root) {
            if (mounts[i].root->finddir) {
                vfs_node_t *found = mounts[i].root->finddir(mounts[i].root, name);
                if (found) return found;
            }
            break;
        }
    }
    
    return NULL;
}

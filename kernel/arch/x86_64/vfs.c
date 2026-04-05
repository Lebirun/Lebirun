#include <kernel/vfs.h>
#include <kernel/tty.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/debug.h>
#include <kernel/mem_map.h>
#include <kernel/ramfs.h>
#include <kernel/drivers/sata/ahci.h>
#include <string.h>
#include <stddef.h>

static vfs_node_t *vfs_root = NULL;
static vfs_fs_type_t *registered_fs = NULL;
static vfs_mount_t *mounts = NULL;
static int mounts_capacity = 0;
static vfs_fd_t *fd_table = NULL;
static int fd_table_capacity = 0;
static mutex_t vfs_lock;
static int squashfs_access_blocked = 0;

#define VFS_INITIAL_FDS 32

static vfs_node_t root_node;
static dirent_t root_dirent;

static dirent_t *root_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *root_finddir(vfs_node_t *node, const char *name);
static int root_create(vfs_node_t *parent, const char *name, uint64_t flags);
static int root_unlink(vfs_node_t *parent, const char *name);
static int root_mkdir(vfs_node_t *parent, const char *name, uint64_t perms);
static int root_rename(vfs_node_t *old_parent, const char *old_name, vfs_node_t *new_parent, const char *new_name);

static int vfs_grow_mounts(void) {
    int new_cap;
    int i;
    vfs_mount_t *new_mounts;

    new_cap = mounts_capacity * 2;
    if (new_cap > VFS_MAX_MOUNTS) new_cap = VFS_MAX_MOUNTS;
    if (new_cap <= mounts_capacity) return -1;
    new_mounts = (vfs_mount_t *)krealloc(mounts, new_cap * sizeof(vfs_mount_t));
    if (!new_mounts) return -1;
    for (i = mounts_capacity; i < new_cap; i++) {
        new_mounts[i].in_use = 0;
        new_mounts[i].path[0] = '\0';
        new_mounts[i].device[0] = '\0';
        new_mounts[i].root = NULL;
        new_mounts[i].fs_type = NULL;
    }
    mounts = new_mounts;
    mounts_capacity = new_cap;
    return 0;
}

static int vfs_grow_fds(void) {
    int new_cap;
    int i;
    vfs_fd_t *new_table;

    new_cap = fd_table_capacity * 2;
    if (new_cap > VFS_MAX_FDS) new_cap = VFS_MAX_FDS;
    if (new_cap <= fd_table_capacity) return -1;
    new_table = (vfs_fd_t *)krealloc(fd_table, new_cap * sizeof(vfs_fd_t));
    if (!new_table) return -1;
    for (i = fd_table_capacity; i < new_cap; i++) {
        new_table[i].in_use = 0;
        new_table[i].node = NULL;
        new_table[i].offset = 0;
        new_table[i].flags = 0;
    }
    fd_table = new_table;
    fd_table_capacity = new_cap;
    return 0;
}

void vfs_init(void) {
    int i;
    
    mutex_init(&vfs_lock);

    mounts_capacity = VFS_INITIAL_MOUNTS;
    mounts = (vfs_mount_t *)kmalloc(mounts_capacity * sizeof(vfs_mount_t));
    for (i = 0; i < mounts_capacity; i++) {
        mounts[i].in_use = 0;
        mounts[i].path[0] = '\0';
        mounts[i].device[0] = '\0';
        mounts[i].root = NULL;
        mounts[i].fs_type = NULL;
    }
    
    fd_table_capacity = VFS_INITIAL_FDS;
    fd_table = (vfs_fd_t *)kmalloc(fd_table_capacity * sizeof(vfs_fd_t));
    for (i = 0; i < fd_table_capacity; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].node = NULL;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }
    
    memset(&root_node, 0, sizeof(vfs_node_t));
    strcpy(root_node.name, "/");
    root_node.flags = VFS_DIRECTORY;
    root_node.mask = 0755;
    root_node.readdir = root_readdir;
    root_node.finddir = root_finddir;
    root_node.create = root_create;
    root_node.unlink = root_unlink;
    root_node.mkdir = root_mkdir;
    root_node.rename = root_rename;
    root_node.ref_count = 1;
    root_node.parent = NULL;
    
    vfs_root = &root_node;
    
    printf("VFS: Virtual Filesystem initialized\n");
}

int vfs_register_fs(vfs_fs_type_t *fs) {
    vfs_fs_type_t *cur;
    
    if (!fs) {
        printf("VFS: ERROR: NULL filesystem struct\n");
        return -1;
    }
    
    if (!fs->name) {
        printf("VFS: ERROR: Filesystem has NULL name\n");
        return -1;
    }
    
    if ((uintptr_t)fs->name < 0x1000) {
        printf("VFS: ERROR: Invalid name pointer: %p\n", (void*)fs->name);
        return -1;
    }
    
    cur = registered_fs;
    while (cur) {
        if (strcmp(cur->name, fs->name) == 0) {
            printf("VFS: WARNING: Filesystem '%s' already registered\n", fs->name);
            return -1;
        }
        cur = cur->next;
    }
    
    fs->next = registered_fs;
    registered_fs = fs;
    
    printf("VFS: Registered filesystem: %s\n", fs->name);
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
    int iterations;
    vfs_fs_type_t *cur;
    iterations = 0;
    if (!name) return NULL;
    cur = registered_fs;
    while (cur) {
        if (++iterations > 100) {
            printf("VFS: ERROR: vfs_find_fs loop detected after %d iterations!\n", iterations);
            return NULL;
        }
        if (!vfs_fs_type_valid(cur)) {
            printf("VFS: ERROR: Invalid fs_type at %p (iter %d)\n", (void*)cur, iterations);
            return NULL;
        }
        if (strcmp(cur->name, name) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static const char *vfs_detect_fs(const char *device) {
    uint64_t port_idx;
    uint64_t part_start;
    uint8_t *buf;
    uint16_t ext_magic;
    extern uint64_t devfs_get_partition_start(vfs_node_t *node);
    vfs_node_t *dev_node;
    ahci_port_t *port;


    if (!device || device[0] == '\0')
        return NULL;

    dev_node = vfs_namei(device);
    if (!dev_node)
        return NULL;
    if ((dev_node->flags & VFS_TYPE_MASK) != VFS_BLOCKDEVICE)
        return NULL;

    port_idx = dev_node->inode;
    part_start = devfs_get_partition_start(dev_node);

    port = ahci_get_port(port_idx);
    if (!port)
        return NULL;

    buf = (uint8_t *)kmalloc(4096);
    if (!buf)
        return NULL;

    if (ahci_read_sectors(port, part_start, 8, buf) != 0) {
        kfree(buf);
        return NULL;
    }

    ext_magic = *(uint16_t *)(buf + 1024 + 56);
    if (ext_magic == 0xEF53) {
        kfree(buf);
        return "ext4";
    }

    kfree(buf);
    return NULL;
}

int vfs_mount_flags(const char *device, const char *mountpoint, const char *fs_type, uint64_t flags) {
    int slot;
    int i;
    size_t cp;
    const char *end;
    const char *base;
    size_t n;
    size_t di;
    char parent_path[VFS_MAX_PATH];
    const char *detected_fs;
    vfs_node_t *existing;
    vfs_node_t *parent_node;
    vfs_fs_type_t *fs;
    vfs_node_t *root;
    
    
    if (!mountpoint || !fs_type) {
        return -1;
    }

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use) {
            if (device && device[0] != '\0' && strcmp(mounts[i].device, device) == 0) {
                printf("VFS: %s is already mounted on %s\n", device, mounts[i].path);
                return -1;
            }
            if (strcmp(mounts[i].path, mountpoint) == 0) {
                printf("VFS: %s already has a filesystem mounted\n", mountpoint);
                return -1;
            }
        }
    }

    if (strcmp(fs_type, "auto") == 0) {
        detected_fs = vfs_detect_fs(device);
        if (!detected_fs) {
            printf("VFS: Auto-detect: no recognized filesystem on %s\n",
                   device ? device : "(none)");
            return -1;
        }
        printf("VFS: Auto-detect: detected %s on %s\n", detected_fs,
               device ? device : "(none)");
        fs_type = detected_fs;
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
    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        if (vfs_grow_mounts() == 0) {
            for (i = 0; i < mounts_capacity; i++) {
                if (!mounts[i].in_use) {
                    slot = i;
                    break;
                }
            }
        }
    }

    if (slot < 0) {
        printf("VFS: Mount table full\n");
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
    mounts[slot].flags = flags;
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
            
            n = (size_t)(base - mountpoint);
            if (n == 0) n = 1;
            if (n >= VFS_MAX_PATH) n = VFS_MAX_PATH - 1;
            memcpy(parent_path, mountpoint, n);
            if (n > 1 && parent_path[n - 1] == '/')
                parent_path[n - 1] = '\0';
            else
                parent_path[n] = '\0';
            parent_node = vfs_namei(parent_path);
            if (parent_node)
                root->parent = parent_node;
            
            existing = root_finddir(vfs_root, root->name);
            if (existing) {
                root->ptr = existing;
                root->flags |= VFS_MOUNTPOINT;
            }
        }
    }
    
    printf("VFS: Mounted %s on %s (type: %s)\n", 
           device ? device : "(none)", mountpoint, fs_type);
    
    return 0;
}

int vfs_mount(const char *device, const char *mountpoint, const char *fs_type) {
    return vfs_mount_flags(device, mountpoint, fs_type, 0);
}

int vfs_unmount(const char *mountpoint) {
    int i;
    
    if (!mountpoint) return -1;
    
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            if (mounts[i].fs_type && mounts[i].fs_type->unmount) {
                mounts[i].fs_type->unmount(mounts[i].root);
            }
            
            mounts[i].in_use = 0;
            mounts[i].path[0] = '\0';
            mounts[i].root = NULL;
            mounts[i].fs_type = NULL;
            mounts[i].device[0] = '\0';
            
            printf("VFS: Unmounted %s\n", mountpoint);
            return 0;
        }
    }
    
    return -1; 
}

int vfs_remove_mount(const char *mountpoint) {
    int i;

    if (!mountpoint) return -1;

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            mounts[i].in_use = 0;
            mounts[i].path[0] = '\0';
            mounts[i].root = NULL;
            mounts[i].fs_type = NULL;
            mounts[i].device[0] = '\0';
            return 0;
        }
    }

    return -1;
}

uint64_t vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer) return 0;
    
    if (node->read) {
        return node->read(node, offset, size, buffer);
    }
    
    return 0;
}

uint64_t vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer) return 0;
    
    if (node->write) {
        return node->write(node, offset, size, buffer);
    }
    
    return 0;
}

void vfs_open(vfs_node_t *node, uint64_t flags) {
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

static dirent_t mount_child_dirent;

static int vfs_node_to_path(vfs_node_t *node, char *buf, size_t size) {
    int i;
    char temp[VFS_MAX_PATH];
    int pos;
    size_t len;
    size_t pathlen;
    vfs_node_t *cur;

    if (!node || !buf || size == 0)
        return -1;

    if (node == vfs_root) {
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && mounts[i].root == node) {
            len = strlen(mounts[i].path);
            if (len >= size)
                return -1;
            memcpy(buf, mounts[i].path, len + 1);
            return 0;
        }
    }

    pos = VFS_MAX_PATH - 1;
    temp[pos] = '\0';

    cur = node;
    while (cur && cur != vfs_root) {
        for (i = 0; i < mounts_capacity; i++) {
            if (mounts[i].in_use && mounts[i].root == cur) {
                len = strlen(mounts[i].path);
                pathlen = (VFS_MAX_PATH - 1) - pos;
                if (len > 1 && len + pathlen >= size)
                    return -1;
                if (len == 1 && mounts[i].path[0] == '/') {
                    if (pathlen + 1 >= size)
                        return -1;
                    memcpy(buf, &temp[pos], pathlen + 1);
                } else {
                    if (len + pathlen >= size)
                        return -1;
                    memcpy(buf, mounts[i].path, len);
                    memcpy(buf + len, &temp[pos], pathlen + 1);
                }
                return 0;
            }
        }
        len = strlen(cur->name);
        pos -= (int)len;
        if (pos < 1)
            return -1;
        memcpy(&temp[pos], cur->name, len);
        temp[--pos] = '/';
        cur = cur->parent;
    }

    if (pos == VFS_MAX_PATH - 1)
        temp[--pos] = '/';

    pathlen = (VFS_MAX_PATH - 1) - pos + 1;
    if (pathlen >= size)
        return -1;
    memcpy(buf, &temp[pos], pathlen);
    return 0;
}

static dirent_t *vfs_readdir_mount_children(vfs_node_t *node, uint64_t mount_index) {
    char dir_path[VFS_MAX_PATH];
    size_t dir_len;
    int i;
    uint64_t count;
    const char *child_name;
    size_t child_len;
    int k;
    int is_dup;
    uint64_t fi;
    dirent_t *fs_entry;

    if (node == vfs_root)
        return NULL;

    if (vfs_node_to_path(node, dir_path, sizeof(dir_path)) < 0)
        return NULL;

    dir_len = strlen(dir_path);
    if (dir_len > 1 && dir_path[dir_len - 1] == '/') {
        dir_path[--dir_len] = '\0';
    }

    count = 0;
    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use)
            continue;
        if (strncmp(mounts[i].path, dir_path, dir_len) != 0)
            continue;
        if (mounts[i].path[dir_len] != '/')
            continue;
        child_name = mounts[i].path + dir_len + 1;
        if (strchr(child_name, '/') != NULL)
            continue;
        if (*child_name == '\0')
            continue;
        if (node->readdir) {
            is_dup = 0;
            for (fi = 0; ; fi++) {
                fs_entry = node->readdir(node, fi);
                if (!fs_entry)
                    break;
                if (strcmp(fs_entry->name, child_name) == 0) {
                    is_dup = 1;
                    break;
                }
            }
            if (is_dup)
                continue;
        }
        if (count == mount_index) {
            child_len = strlen(child_name);
            if (child_len >= VFS_MAX_NAME)
                child_len = VFS_MAX_NAME - 1;
            for (k = 0; k < (int)child_len; k++)
                mount_child_dirent.name[k] = child_name[k];
            mount_child_dirent.name[child_len] = '\0';
            mount_child_dirent.inode = i;
            mount_child_dirent.type = VFS_DIRECTORY;
            return &mount_child_dirent;
        }
        count++;
    }

    return NULL;
}

dirent_t *vfs_readdir(vfs_node_t *node, uint64_t index) {
    uint64_t fs_count;
    dirent_t *result;

    if (!node) return NULL;

    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY &&
        (node->flags & VFS_MOUNTPOINT) == 0) {
        return NULL;
    }
    
    if (node->readdir) {
        result = node->readdir(node, index);
        if (result)
            return result;
        for (fs_count = 0; fs_count < index; fs_count++) {
            if (!node->readdir(node, fs_count))
                break;
        }
        return vfs_readdir_mount_children(node, index - fs_count);
    }
    
    return vfs_readdir_mount_children(node, index);
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    char dir_path[VFS_MAX_PATH];
    char child_path[VFS_MAX_PATH];
    size_t dir_len;
    size_t name_len;
    int i;
    vfs_node_t *result;

    if (!node || !name) return NULL;
    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY && (node->flags & VFS_MOUNTPOINT) == 0) return NULL;
    if (node->finddir) {
        if ((uintptr_t)node->finddir < 0x1000) {
            return NULL;
        }
        result = node->finddir(node, name);
        if (result)
            return result;
    }

    if (node == vfs_root)
        return NULL;

    if (vfs_node_to_path(node, dir_path, sizeof(dir_path)) < 0)
        return NULL;

    dir_len = strlen(dir_path);
    if (dir_len > 1 && dir_path[dir_len - 1] == '/') {
        dir_path[--dir_len] = '\0';
    }

    name_len = strlen(name);
    if (dir_len + 1 + name_len >= VFS_MAX_PATH)
        return NULL;

    memcpy(child_path, dir_path, dir_len);
    child_path[dir_len] = '/';
    memcpy(child_path + dir_len + 1, name, name_len);
    child_path[dir_len + 1 + name_len] = '\0';

    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use)
            continue;
        if (strcmp(mounts[i].path, child_path) == 0)
            return mounts[i].root;
    }

    return NULL;
}

int vfs_create(vfs_node_t *parent, const char *name, uint64_t flags) {
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

int vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->mkdir) return parent->mkdir(parent, name, perms);
    return -1;
}

static vfs_mount_t *find_mount_for_path(const char *path) {
    size_t best_len;
    int i;
    size_t len;
    vfs_mount_t *best;
    
    best = NULL;
    best_len = 0;
    
    for (i = 0; i < mounts_capacity; i++) {
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
    uint64_t n;
    if (!node || !buf || size == 0) return -1;
    if (VFS_GET_TYPE(node->flags) != VFS_SYMLINK) return -1;

    n = vfs_read(node, 0, (uint64_t)(size - 1), (uint8_t *)buf);
    if (n >= size) n = (uint64_t)(size - 1);
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
    char *last;
    size_t plen;
    int i;
    int has_more;
    vfs_mount_t *mount;
    vfs_node_t *node;
    vfs_node_t *next;

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
    char temp[VFS_MAX_PATH];
    int pos;
    size_t pathlen;
    if (!node || !buf || size == 0) return NULL;
    
    if (node == vfs_root) {
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }
    
    pos = VFS_MAX_PATH - 1;
    temp[pos] = '\0';
    
    vfs_node_t *cur = node;
    while (cur && cur != vfs_root) {
        size_t len = strlen(cur->name);
        if (len > 0) {
            pos -= len;
            if (pos < 1) return NULL;
            memcpy(&temp[pos], cur->name, len);
            temp[--pos] = '/';
        }
        cur = cur->parent;
    }
    
    if (pos == VFS_MAX_PATH - 1) temp[--pos] = '/';
    
    pathlen = VFS_MAX_PATH - pos;
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
    char parent_path[VFS_MAX_PATH];
    char filename[VFS_MAX_NAME];
    int ret;
    int fd;
    int i;
    if (!path) return -1;
    
    vfs_node_t *node;
    vfs_node_t *parent;
    
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
    for (i = 3; i < fd_table_capacity; i++) {
        if (!fd_table[i].in_use) {
            fd = i;
            break;
        }
    }
    
    if (fd < 0) {
        if (vfs_grow_fds() == 0) {
            for (i = 3; i < fd_table_capacity; i++) {
                if (!fd_table[i].in_use) {
                    fd = i;
                    break;
                }
            }
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
    if (fd < 0 || fd >= fd_table_capacity) return -1;
    
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

int vfs_read_fd(int fd, void *buffer, uint64_t size) {
    uint64_t bytes;
    vfs_node_t *node;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table[fd].in_use) return -1;
    if (!buffer || size == 0) return -1;
    
    node = fd_table[fd].node;
    if (!node) return -1;
    
    bytes = vfs_read(node, fd_table[fd].offset, size, (uint8_t *)buffer);
    fd_table[fd].offset += bytes;
    
    return (int)bytes;
}

int vfs_write_fd(int fd, const void *buffer, uint64_t size) {
    uint64_t bytes;
    vfs_node_t *node;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table[fd].in_use) return -1;
    if (!buffer || size == 0) return -1;
    
    node = fd_table[fd].node;
    if (!node) return -1;
    
    bytes = vfs_write(node, fd_table[fd].offset, size, (uint8_t *)buffer);
    fd_table[fd].offset += bytes;
    
    return (int)bytes;
}

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    int64_t new_offset;
    vfs_node_t *node;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table[fd].in_use) return -1;
    
    node = fd_table[fd].node;
    if (!node) return -1;
    
    switch (whence) {
        case VFS_SEEK_SET:
            new_offset = offset;
            break;
        case VFS_SEEK_CUR:
            new_offset = (int64_t)fd_table[fd].offset + offset;
            break;
        case VFS_SEEK_END:
            new_offset = (int64_t)node->length + offset;
            break;
        default:
            return -1;
    }
    
    if (new_offset < 0) return -1;
    
    fd_table[fd].offset = (uint64_t)new_offset;
    return (int64_t)fd_table[fd].offset;
}

int64_t vfs_tell(int fd) {
    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table[fd].in_use) return -1;
    
    return (int64_t)fd_table[fd].offset;
}

int vfs_stat_fd(int fd, uint64_t *size, uint64_t *flags) {
    vfs_node_t *node;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table[fd].in_use) return -1;
    
    node = fd_table[fd].node;
    if (!node) return -1;
    
    if (size) *size = node->length;
    if (flags) *flags = node->flags;
    
    DEBUG_VFS("VFS_STAT: fd=%d node=%p name='%s' length=%llu flags=0x%X\n", 
           fd, node, node->name, (unsigned long long)node->length, node->flags);
    
    return 0;
}

int vfs_readdir_fd(int fd, dirent_t *entry, uint64_t index) {
    vfs_node_t *node;
    dirent_t *result;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
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
    
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0) {
            return mounts[i].root;
        }
    }
    return vfs_root;
}

int vfs_replace_mount_root(const char *mountpoint, vfs_node_t *new_root, const char *device, const char *fs_name) {
    int i;

    if (!mountpoint || !new_root) return -1;

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            mounts[i].root = new_root;
            new_root->parent = vfs_root;
            if (device) {
                strncpy(mounts[i].device, device, VFS_MAX_PATH - 1);
                mounts[i].device[VFS_MAX_PATH - 1] = '\0';
            }
            if (fs_name)
                mounts[i].fs_type = vfs_find_fs(fs_name);
            return 0;
        }
    }
    return -1;
}

int vfs_get_mount_count(void) {
    int count;
    int i;
    
    count = 0;
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use) count++;
    }
    return count;
}

vfs_mount_t *vfs_get_mount(int index) {
    int count;
    int i;
    
    count = 0;
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use) {
            if (count == index) return &mounts[i];
            count++;
        }
    }
    return NULL;
}

void vfs_list_mounts(void) {
    int i;
    
    printf("VFS: Mount table:\n");
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use) {
            printf("  %s -> %s\n", 
                   mounts[i].path,
                   mounts[i].fs_type ? mounts[i].fs_type->name : "(unknown)");
        }
    }
}

static dirent_t *root_readdir(vfs_node_t *node, uint64_t index) {
    uint64_t count;
    int i;
    int j;
    const char *path;
    
    (void)node;
    
    count = 0;
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") != 0) {
            if (strcmp(mounts[i].path, "/ro") == 0) {
                continue;
            }
            if (strcmp(mounts[i].path, "/squashfs") == 0) {
                continue;
            }
            path = mounts[i].path;
            if (path[0] == '/') path++;
            if (strchr(path, '/') != NULL) {
                continue;
            }
            if (count == index) {
                j = 0;
                while (path[j] && j < VFS_MAX_NAME - 1) {
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
    
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0 && mounts[i].root) {
            if (mounts[i].root->readdir) {
                uint64_t ramfs_idx;
                uint64_t ramfs_count;
                uint64_t target;
                char mount_path[VFS_MAX_PATH];
                int is_dup;
                int k;
                dirent_t *entry;

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
                    for (k = 0; k < mounts_capacity; k++) {
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
    
    for (i = 0; i < mounts_capacity; i++) {
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
    }
    
    for (i = 0; i < mounts_capacity; i++) {
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

static vfs_node_t *root_mount_root(void) {
    int i;

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, "/") == 0 && mounts[i].root)
            return mounts[i].root;
    }
    return NULL;
}

static int root_create(vfs_node_t *parent, const char *name, uint64_t flags) {
    vfs_node_t *r;

    (void)parent;
    r = root_mount_root();
    if (r && r->create)
        return r->create(r, name, flags);
    return -1;
}

static int root_unlink(vfs_node_t *parent, const char *name) {
    vfs_node_t *r;

    (void)parent;
    r = root_mount_root();
    if (r && r->unlink)
        return r->unlink(r, name);
    return -1;
}

static int root_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
    vfs_node_t *r;

    (void)parent;
    r = root_mount_root();
    if (r && r->mkdir)
        return r->mkdir(r, name, perms);
    return -1;
}

static int root_rename(vfs_node_t *old_parent, const char *old_name, vfs_node_t *new_parent, const char *new_name) {
    vfs_node_t *r;

    (void)old_parent;
    r = root_mount_root();
    if (r && r->rename)
        return r->rename(r, old_name, new_parent, new_name);
    return -1;
}

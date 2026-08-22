#include <lebirun/vfs.h>
#include <lebirun/common.h>
#include <lebirun/tty.h>
#include <lebirun/mutex.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/ramfs.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/inotify.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/overlayfs.h>
#include <lebirun/squashfs.h>
#include <string.h>
#include <stddef.h>

extern void overlay_flush_cache(void);
extern void squashfs_flush_cache(void);
extern void squashfs_set_access_blocked(int blocked);
extern void kstack_reclaim_unused(void);
extern void heap_reclaim_unused(void);
extern void pfa_ref_gc(void);
extern void copy_file_range_release_mount(vfs_node_t *root);

static vfs_node_t *vfs_root = NULL;
static vfs_fs_type_t *registered_fs = NULL;
static vfs_mount_t *mounts = NULL;
static int mounts_capacity = 0;
static vfs_fd_t *fd_table = NULL;
static int fd_table_capacity = 0;
static mutex_t vfs_lock;
static int squashfs_access_blocked = 0;

#define VFS_INITIAL_FDS 4

static vfs_node_t root_node;
static dirent_t root_dirent;

static dirent_t *root_readdir(vfs_node_t *node, uint64_t index);

const char *vfs_node_name(const vfs_node_t *node) {
    if (!node) return "";
    if (node->flags & VFS_NAME_DYNAMIC)
        return node->dynamic_name ? node->dynamic_name : "";
    return node->name;
}

int vfs_node_set_name(vfs_node_t *node, const char *name) {
    if (!node || !name) return -1;
    return vfs_node_set_name_n(node, name, strlen(name));
}

int vfs_node_set_name_n(vfs_node_t *node, const char *name, size_t length) {
    char *copy;

    if (!node || !name) return -1;
    copy = NULL;
    if (length >= VFS_NODE_INLINE_NAME) {
        if (length == SIZE_MAX) return -1;
        copy = (char *)kmalloc(length + 1);
        if (!copy) return -1;
        memcpy(copy, name, length);
        copy[length] = '\0';
    }
    vfs_node_release_name(node);
    if (copy) {
        node->dynamic_name = copy;
        node->flags |= VFS_NAME_DYNAMIC;
    } else {
        memcpy(node->name, name, length);
        node->name[length] = '\0';
    }
    return 0;
}

void vfs_node_release_name(vfs_node_t *node) {
    if (!node) return;
    if (node->flags & VFS_NAME_DYNAMIC) {
        if (node->dynamic_name) kfree(node->dynamic_name);
        node->dynamic_name = NULL;
        node->flags &= ~VFS_NAME_DYNAMIC;
    }
    node->name[0] = '\0';
}

const char *vfs_dirent_name(const dirent_t *entry) {
    if (!entry) return "";
    if (entry->name_dynamic)
        return entry->dynamic_name ? entry->dynamic_name : "";
    return entry->name;
}

int vfs_dirent_set_name(dirent_t *entry, const char *name) {
    if (!entry || !name) return -1;
    return vfs_dirent_set_name_n(entry, name, strlen(name));
}

int vfs_dirent_set_name_n(dirent_t *entry, const char *name, size_t length) {
    char *copy;

    if (!entry || !name) return -1;
    copy = NULL;
    if (length >= VFS_MAX_NAME) {
        if (length == SIZE_MAX) return -1;
        copy = (char *)kmalloc(length + 1);
        if (!copy) return -1;
        memcpy(copy, name, length);
        copy[length] = '\0';
    }
    vfs_dirent_release_name(entry);
    if (copy) {
        entry->dynamic_name = copy;
        entry->name_dynamic = 1;
    } else {
        memcpy(entry->name, name, length);
        entry->name[length] = '\0';
    }
    return 0;
}

void vfs_dirent_release_name(dirent_t *entry) {
    if (!entry) return;
    if (entry->name_dynamic && entry->dynamic_name)
        kfree(entry->dynamic_name);
    entry->dynamic_name = NULL;
    entry->name_dynamic = 0;
    entry->name[0] = '\0';
}
static vfs_node_t *root_finddir(vfs_node_t *node, const char *name);
static int root_create(vfs_node_t *parent, const char *name, uint64_t flags);
static int root_unlink(vfs_node_t *parent, const char *name);
static int root_mkdir(vfs_node_t *parent, const char *name, uint64_t perms);
static int root_rename(vfs_node_t *old_parent, const char *old_name, vfs_node_t *new_parent, const char *new_name);

static int vfs_grow_mounts(void) {
    int new_cap;
    int i;
    vfs_mount_t *new_mounts;

    if (mounts_capacity == INT32_MAX) return -1;
    new_cap = mounts_capacity + 1;
    if (new_cap <= mounts_capacity) return -1;
    if ((uint64_t)new_cap > UINT64_MAX / sizeof(vfs_mount_t)) return -1;
    new_mounts = (vfs_mount_t *)krealloc(mounts, new_cap * sizeof(vfs_mount_t));
    if (!new_mounts) return -1;
    for (i = mounts_capacity; i < new_cap; i++) {
        new_mounts[i].in_use = 0;
        new_mounts[i].path = NULL;
        new_mounts[i].device = NULL;
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

    if (fd_table_capacity > INT32_MAX / 2) return -1;
    new_cap = fd_table_capacity ? fd_table_capacity * 2 : VFS_INITIAL_FDS;
    if (new_cap <= fd_table_capacity) return -1;
    if ((uint64_t)new_cap > UINT64_MAX / sizeof(vfs_fd_t)) return -1;
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

static void vfs_reclaim_fds(void) {
    int i;
    int active;
    vfs_fd_t *new_table;

    mutex_lock(&vfs_lock);
    active = 0;
    for (i = 0; i < fd_table_capacity; i++) {
        if (fd_table[i].in_use) {
            active = 1;
            break;
        }
    }
    if (!active) {
        if (fd_table) kfree(fd_table);
        fd_table = NULL;
        fd_table_capacity = 0;
        mutex_unlock(&vfs_lock);
        return;
    }
    if (fd_table_capacity <= VFS_INITIAL_FDS) {
        mutex_unlock(&vfs_lock);
        return;
    }
    for (i = VFS_INITIAL_FDS; i < fd_table_capacity; i++) {
        if (fd_table[i].in_use) {
            mutex_unlock(&vfs_lock);
            return;
        }
    }
    new_table = (vfs_fd_t *)krealloc(fd_table, VFS_INITIAL_FDS * sizeof(vfs_fd_t));
    if (!new_table) {
        mutex_unlock(&vfs_lock);
        return;
    }
    fd_table = new_table;
    fd_table_capacity = VFS_INITIAL_FDS;
    mutex_unlock(&vfs_lock);
}

void KERNEL_INIT vfs_init(void) {
    mutex_init(&vfs_lock);

    mounts_capacity = 0;
    mounts = NULL;
    
    fd_table_capacity = 0;
    fd_table = NULL;
    
    memset(&root_node, 0, sizeof(vfs_node_t));
    root_node.flags = VFS_DIRECTORY;
    vfs_node_set_name(&root_node, "/");
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

int KERNEL_INIT vfs_register_fs(vfs_fs_type_t *fs) {
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

static int vfs_mount_set_strings(vfs_mount_t *mount, const char *path, const char *device) {
    size_t path_len;
    size_t device_len;
    char *strings;

    if (!mount || !path) return -1;
    path_len = strlen(path);
    device_len = device ? strlen(device) : 0;
    if (path_len > SIZE_MAX - 2 ||
        device_len > SIZE_MAX - path_len - 2) return -1;
    strings = (char *)kmalloc(path_len + device_len + 2);
    if (!strings) return -1;
    memcpy(strings, path, path_len + 1);
    memcpy(strings + path_len + 1, device ? device : "", device_len + 1);
    if (mount->path) kfree(mount->path);
    mount->path = strings;
    mount->device = strings + path_len + 1;
    return 0;
}

static void vfs_mount_clear_strings(vfs_mount_t *mount) {
    if (!mount) return;
    if (mount->path) kfree(mount->path);
    mount->path = NULL;
    mount->device = NULL;
}

vfs_fs_type_t *vfs_find_fs(const char *name) {
    vfs_fs_type_t *cur;
    vfs_fs_type_t *slow;
    vfs_fs_type_t *fast;
    int cycle_check_started;

    if (!name) return NULL;
    cur = registered_fs;
    slow = registered_fs;
    fast = registered_fs;
    cycle_check_started = 0;
    while (cur) {
        if (!vfs_fs_type_valid(cur)) {
            printf("VFS: ERROR: Invalid fs_type at %p\n", (void*)cur);
            return NULL;
        }
        if (strcmp(cur->name, name) == 0) {
            return cur;
        }
        if (cycle_check_started && slow && fast && slow == fast) {
            printf("VFS: ERROR: vfs_find_fs loop detected\n");
            return NULL;
        }
        cur = cur->next;
        if (slow) {
            if (!vfs_fs_type_valid(slow)) return NULL;
            slow = slow->next;
        }
        if (fast) {
            if (!vfs_fs_type_valid(fast)) return NULL;
            fast = fast->next;
            if (fast) {
                if (!vfs_fs_type_valid(fast)) return NULL;
                fast = fast->next;
            }
        }
        cycle_check_started = 1;
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
    const char *end;
    const char *base;
    size_t n;
    char *parent_path;
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

    if (vfs_mount_set_strings(&mounts[slot], mountpoint, device) != 0) {
        return -1;
    }
    
    root = fs->mount(device, mountpoint);
    if (!root) {
        vfs_mount_clear_strings(&mounts[slot]);
        return -1;
    }
    
    existing = vfs_namei(mountpoint);
    if (existing) {
        root->flags |= VFS_MOUNTPOINT;
        root->ptr = existing;
    }

    mounts[slot].in_use = 1;
    mounts[slot].flags = flags;
    mounts[slot].root = root;
    mounts[slot].fs_type = fs;
    
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
            if (vfs_node_set_name_n(root, base, n) != 0) {
                if (fs->unmount) fs->unmount(root);
                mounts[slot].in_use = 0;
                mounts[slot].root = NULL;
                mounts[slot].fs_type = NULL;
                vfs_mount_clear_strings(&mounts[slot]);
                return -1;
            }
            
            n = (size_t)(base - mountpoint);
            if (n == 0) n = 1;
            parent_path = (char *)kmalloc(n + 1);
            if (!parent_path) {
                if (fs->unmount) fs->unmount(root);
                mounts[slot].in_use = 0;
                mounts[slot].root = NULL;
                mounts[slot].fs_type = NULL;
                vfs_mount_clear_strings(&mounts[slot]);
                return -1;
            }
            memcpy(parent_path, mountpoint, n);
            if (n > 1 && parent_path[n - 1] == '/')
                parent_path[n - 1] = '\0';
            else
                parent_path[n] = '\0';
            parent_node = vfs_namei(parent_path);
            kfree(parent_path);
            if (parent_node)
                root->parent = parent_node;
            
            existing = root_finddir(vfs_root, vfs_node_name(root));
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
    int ret;
    
    if (!mountpoint) return -1;
    
    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            copy_file_range_release_mount(mounts[i].root);
            vfs_reclaim_fds();
            overlay_flush_cache();
            squashfs_flush_cache();
            heap_reclaim_unused();
            if (mounts[i].fs_type && mounts[i].fs_type->unmount) {
                ret = mounts[i].fs_type->unmount(mounts[i].root);
                if (ret != 0) {
                    return ret;
                }
            }
            
            mounts[i].in_use = 0;
            vfs_mount_clear_strings(&mounts[i]);
            mounts[i].root = NULL;
            mounts[i].fs_type = NULL;
            
            printf("VFS: Unmounted %s\n", mountpoint);
            overlay_flush_cache();
            squashfs_flush_cache();
            vfs_reclaim_fds();
            kstack_reclaim_unused();
            heap_reclaim_unused();
            pfa_ref_gc();
            return 0;
        }
    }
    
    return -1; 
}

int KERNEL_INIT vfs_remove_mount(const char *mountpoint) {
    int i;

    if (!mountpoint) return -1;

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            mounts[i].in_use = 0;
            vfs_mount_clear_strings(&mounts[i]);
            mounts[i].root = NULL;
            mounts[i].fs_type = NULL;
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
    uint64_t written;

    if (!node || !buffer) return 0;
    if (node->write) {
        written = node->write(node, offset, size, buffer);
        if (written) inotify_notify(node, 0x00000002U, NULL);
        return written;
    }
    return 0;
}

uint64_t vfs_transfer_window_size(vfs_node_t *node) {
    vfs_node_t *backing;
    uint64_t size;

    if (!node) return 0;
    backing = overlay_get_backing_node(node);
    if (!backing) backing = node;
    size = squashfs_transfer_window_size(backing);
    if (size != 0) return size;
    return 65536;
}

int vfs_transfer_reuse_supported(vfs_node_t *node) {
    vfs_node_t *backing;

    if (!node) return 0;
    backing = overlay_get_backing_node(node);
    if (!backing) backing = node;
    return squashfs_transfer_window_size(backing) != 0;
}

uint64_t vfs_transfer_read(vfs_node_t *node, uint64_t offset, uint64_t size,
                           uint8_t *buffer, uint64_t capacity) {
    vfs_node_t *backing;
    uint64_t window_size;

    if (!node || !buffer || capacity == 0) return 0;
    if (size > capacity) size = capacity;
    backing = overlay_get_backing_node(node);
    if (!backing) backing = node;
    window_size = squashfs_transfer_window_size(backing);
    if (window_size != 0)
        return squashfs_transfer_read(backing, offset, size, buffer,
                                      capacity);
    return vfs_read(node, offset, size, buffer);
}

uint64_t vfs_transfer_read_view(vfs_node_t *node, uint64_t offset,
                                uint64_t size,
                                struct squashfs_transfer_cache *cache,
                                uint8_t **view) {
    vfs_node_t *backing;
    uint64_t window_size;

    if (!node || !cache || !view) return 0;
    backing = overlay_get_backing_node(node);
    if (!backing) backing = node;
    window_size = squashfs_transfer_window_size(backing);
    if (window_size == 0) return 0;
    if (size > cache->data_capacity) size = cache->data_capacity;
    return squashfs_transfer_read_view(backing, offset, size, cache, view);
}

uint64_t vfs_transfer_write(vfs_node_t *node, uint64_t offset, uint64_t size,
                            uint8_t *buffer, uint8_t *scratch,
                            uint64_t scratch_capacity) {
    uint64_t written;

    if (!node || !buffer) return 0;
    written = ext4_transfer_write(node, offset, size, buffer, scratch,
                                  scratch_capacity);
    if (written != UINT64_MAX) {
        if (written) inotify_notify(node, 0x00000002U, NULL);
        return written;
    }
    return vfs_write(node, offset, size, buffer);
}

void vfs_open(vfs_node_t *node, uint64_t flags) {
    uintptr_t open_addr;

    if (!node) return;
    __atomic_add_fetch(&node->ref_count, 1, __ATOMIC_ACQ_REL);
    vfs_lookup_hazard_clear(node);
    open_addr = (uintptr_t)node->open;
    if (node->open && open_addr >= KERNEL_VMA) node->open(node, flags);
}

void vfs_close(vfs_node_t *node) {
    int generic_owned;
    int callback_owns_node;
    uintptr_t close_addr;
    uint64_t refs;
    int decremented;

    if (!node) return;
    vfs_lookup_hazard_clear(node);

    decremented = 0;
    refs = __atomic_load_n(&node->ref_count, __ATOMIC_ACQUIRE);
    while (refs > 0) {
        if (__atomic_compare_exchange_n(&node->ref_count, &refs, refs - 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            refs--;
            decremented = 1;
            break;
        }
    }
    if (!decremented) return;

    generic_owned = (node->flags & VFS_DYNAMIC) && !(node->flags & VFS_EMBEDDED);
    callback_owns_node = (node->flags & VFS_EMBEDDED) != 0;

    close_addr = (uintptr_t)node->close;
    if (node->close && close_addr >= KERNEL_VMA) {
        node->close(node);
        if (callback_owns_node) return;
    }

    refs = __atomic_load_n(&node->ref_count, __ATOMIC_ACQUIRE);
    if (generic_owned && refs == 0 && node->private_data == NULL) {
        kfree(node);
    }
}

void vfs_lookup_hazard_set(vfs_node_t *node) {
    task_t *task;

    task = current_task;
    if (!task) return;
    __atomic_store_n(&task->vfs_lookup_node, node, __ATOMIC_RELEASE);
}

void vfs_lookup_hazard_clear(vfs_node_t *node) {
    task_t *task;
    void *expected;

    task = current_task;
    if (!task) return;
    expected = node;
    __atomic_compare_exchange_n(&task->vfs_lookup_node, &expected, NULL, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int vfs_lookup_hazard_contains(vfs_node_t *node) {
    task_t *task;
    int found;

    if (!node) return 0;
    found = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task) {
        if (__atomic_load_n(&task->vfs_lookup_node, __ATOMIC_ACQUIRE) == node) {
            found = 1;
            break;
        }
        task = task->all_next;
    }
    unlock_scheduler();
    return found;
}

static dirent_t mount_child_dirent;

#if 0
static int vfs_node_to_path(vfs_node_t *node, char *buf, size_t size) {
    int i;
    char temp[VFS_MAX_PATH];
    int pos;
    size_t len;
    size_t pathlen;
    vfs_node_t *cur;
    const char *cur_name;

    if (!node || !buf || size == 0)
        return -1;

    if (node == vfs_root) {
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && mounts[i].root == node) {
            len = vfs_bounded_strlen(mounts[i].path, VFS_MAX_PATH);
            if (len == VFS_MAX_PATH)
                return -1;
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
                len = vfs_bounded_strlen(mounts[i].path, VFS_MAX_PATH);
                if (len == VFS_MAX_PATH)
                    return -1;
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
        cur_name = vfs_node_name(cur);
        len = strlen(cur_name);
        pos -= (int)len;
        if (pos < 1)
            return -1;
        memcpy(&temp[pos], cur_name, len);
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
#endif

static dirent_t *vfs_readdir_mount_children(vfs_node_t *node, uint64_t mount_index) {
    char *dir_path;
    size_t dir_len;
    int i;
    uint64_t count;
    const char *child_name;
    size_t child_len;
    int is_dup;
    uint64_t fi;
    dirent_t *fs_entry;

    if (node == vfs_root)
        return NULL;

    dir_path = vfs_get_path_alloc(node);
    if (!dir_path) return NULL;

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
                if (strcmp(vfs_dirent_name(fs_entry), child_name) == 0) {
                    is_dup = 1;
                    break;
                }
            }
            if (is_dup)
                continue;
        }
        if (count == mount_index) {
            child_len = strlen(child_name);
            if (vfs_dirent_set_name_n(&mount_child_dirent, child_name, child_len) != 0) {
                kfree(dir_path);
                return NULL;
            }
            mount_child_dirent.inode = i;
            mount_child_dirent.type = VFS_DIRECTORY;
            kfree(dir_path);
            return &mount_child_dirent;
        }
        count++;
    }

    kfree(dir_path);
    return NULL;
}

static int vfs_has_mount_children(vfs_node_t *node) {
    char *dir_path;
    size_t dir_len;
    const char *child_name;
    int i;

    if (node == vfs_root)
        return 0;
    dir_path = vfs_get_path_alloc(node);
    if (!dir_path) return 0;
    dir_len = strlen(dir_path);
    if (dir_len > 1 && dir_path[dir_len - 1] == '/')
        dir_path[--dir_len] = '\0';
    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use)
            continue;
        if (strncmp(mounts[i].path, dir_path, dir_len) != 0)
            continue;
        if (mounts[i].path[dir_len] != '/')
            continue;
        child_name = mounts[i].path + dir_len + 1;
        if (*child_name != '\0' && strchr(child_name, '/') == NULL) {
            kfree(dir_path);
            return 1;
        }
    }
    kfree(dir_path);
    return 0;
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
        if (!vfs_has_mount_children(node))
            return NULL;
        for (fs_count = 0; fs_count < index; fs_count++) {
            if (!node->readdir(node, fs_count))
                break;
        }
        return vfs_readdir_mount_children(node, index - fs_count);
    }
    
    return vfs_readdir_mount_children(node, index);
}

int vfs_readdir_copy(vfs_node_t *node, uint64_t index, dirent_t *entry) {
    dirent_t *result;

    if (!node || !entry) return -1;
    mutex_lock(&vfs_lock);
    result = vfs_readdir(node, index);
    if (result)
        memcpy(entry, result, sizeof(dirent_t));
    mutex_unlock(&vfs_lock);
    return result ? 0 : -1;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    char *path;
    char *grown;
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

    path = vfs_get_path_alloc(node);
    if (!path) return NULL;

    dir_len = strlen(path);
    if (dir_len > 1 && path[dir_len - 1] == '/') {
        path[--dir_len] = '\0';
    }

    name_len = strlen(name);
    if (dir_len > SIZE_MAX - name_len - 2) {
        kfree(path);
        return NULL;
    }

    grown = (char *)krealloc(path, dir_len + name_len + 2);
    if (!grown) {
        kfree(path);
        return NULL;
    }
    path = grown;

    path[dir_len] = '/';
    memcpy(path + dir_len + 1, name, name_len);
    path[dir_len + 1 + name_len] = '\0';

    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use)
            continue;
        if (strcmp(mounts[i].path, path) == 0) {
            kfree(path);
            return mounts[i].root;
        }
    }

    kfree(path);
    return NULL;
}

int vfs_create(vfs_node_t *parent, const char *name, uint64_t flags) {
    int result;

    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->create) {
        result = parent->create(parent, name, flags);
        if (result == 0) inotify_notify(parent, 0x00000100U, name);
        return result;
    }
    return -1;
}

int vfs_unlink(vfs_node_t *parent, const char *name) {
    int result;

    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->unlink) {
        result = parent->unlink(parent, name);
        if (result == 0) inotify_notify(parent, 0x00000200U, name);
        return result;
    }
    return -1;
}

int vfs_unlink_checked(vfs_node_t *parent, const char *name, int remove_directory) {
    vfs_node_t *target;
    int is_directory;
    int result;

    if (!parent || !name) return -2;
    __atomic_add_fetch(&parent->ref_count, 1, __ATOMIC_ACQ_REL);
    target = vfs_finddir(parent, name);
    if (!target) {
        result = -2;
    } else {
        is_directory = VFS_GET_TYPE(target->flags) == VFS_DIRECTORY;
        vfs_release(target);
        if (remove_directory && !is_directory)
            result = -20;
        else if (!remove_directory && is_directory)
            result = -21;
        else
            result = vfs_unlink(parent, name);
    }
    vfs_lookup_hazard_set(parent);
    __atomic_sub_fetch(&parent->ref_count, 1, __ATOMIC_ACQ_REL);
    return result;
}

int vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
    int result;

    if (!parent || !name) return -1;
    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return -1;
    if (parent->mkdir) {
        result = parent->mkdir(parent, name, perms);
        if (result == 0)
            inotify_notify(parent, 0x40000100U, name);
        return result;
    }
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
        if (len <= best_len) continue;
        if (strncmp(path, mounts[i].path, len) == 0) {
            if (path[len] == '\0' || path[len] == '/' || 
                (len == 1 && mounts[i].path[0] == '/')) {
                best = &mounts[i];
                best_len = len;
            }
        }
    }
    return best;
}

static char *vfs_resolve_path(const char *path) {
    const char *cwd;
    size_t cwd_len;
    size_t path_len;
    size_t pos;
    char *resolved;

    if (!path) return NULL;
    path_len = strlen(path);
    if (path_len == SIZE_MAX) return NULL;
    if (path[0] == '/') {
        resolved = (char *)kmalloc(path_len + 1);
        if (!resolved) return NULL;
        memcpy(resolved, path, path_len + 1);
        return resolved;
    }
    cwd = "/";
    if (current_task && current_task->cwd && current_task->cwd[0])
        cwd = current_task->cwd;
    cwd_len = strlen(cwd);
    if (cwd_len > SIZE_MAX - path_len - 2) return NULL;
    resolved = (char *)kmalloc(cwd_len + path_len + 2);
    if (!resolved) return NULL;
    pos = 0;
    memcpy(resolved, cwd, cwd_len);
    pos += cwd_len;
    if (pos == 0 || resolved[pos - 1] != '/') resolved[pos++] = '/';
    memcpy(resolved + pos, path, path_len);
    pos += path_len;
    resolved[pos] = '\0';
    return resolved;
}

static void vfs_normalize_path(char *path) {
    char *src;
    char *dst;
    char *comp_start;
    size_t comp_len;
    
    if (!path || path[0] != '/') return;

    src = path;
    dst = path;
    *dst++ = '/';

    while (*src) {
        while (*src == '/') src++;
        if (*src == '\0') break;

        comp_start = src;
        while (*src && *src != '/') src++;
        comp_len = src - comp_start;

        if (comp_len == 1 && comp_start[0] == '.') {
            continue;
        } else if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            if (dst > path + 1) {
                while (dst > path + 1 && dst[-1] != '/') dst--;
                if (dst > path + 1) dst--;
            }
            continue;
        }

        if (dst > path + 1) *dst++ = '/';
        while (comp_len--) *dst++ = *comp_start++;
    }

    *dst = '\0';
}

static int vfs_apply_task_root(char **path_ptr, int redirected_before) {
    char *path;
    char *rooted;
    const char *root;
    size_t root_len;
    size_t path_len;
    size_t position;

    if (!path_ptr || !*path_ptr) return -1;
    if (!current_task || !current_task->is_user || !current_task->root)
        return 0;
    path = *path_ptr;
    root = current_task->root;
    root_len = strlen(root);
    if (redirected_before && strncmp(path, root, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/')) return 0;
    path_len = strlen(path);
    if (root_len > SIZE_MAX - path_len - 1) return -1;
    rooted = (char *)kmalloc(root_len + path_len + 1);
    if (!rooted) return -1;
    memcpy(rooted, root, root_len);
    position = root_len;
    if (position > 1 && rooted[position - 1] == '/') position--;
    memcpy(rooted + position, path, path_len + 1);
    vfs_normalize_path(rooted);
    kfree(path);
    *path_ptr = rooted;
    return 0;
}

static char *vfs_readlink_node(vfs_node_t *node) {
    uint64_t n;
    size_t size;
    char *buf;

    if (!node || VFS_GET_TYPE(node->flags) != VFS_SYMLINK) return NULL;
    if (node->length >= SIZE_MAX) return NULL;
    size = (size_t)node->length + 1;
    if (size < 2) size = 2;
    buf = (char *)kmalloc(size);
    if (!buf) return NULL;

    n = vfs_read(node, 0, (uint64_t)(size - 1), (uint8_t *)buf);
    if (n >= size) n = (uint64_t)(size - 1);
    buf[n] = '\0';
    if (n == 0) {
        kfree(buf);
        return NULL;
    }
    return buf;
}

static char *vfs_build_symlink_path(const char *base_dir,
                                    const char *target,
                                    const char *rest_raw) {
    size_t base_len;
    size_t target_len;
    size_t rest_len;
    size_t length;
    size_t position;
    char *path;

    if (!base_dir || !target || !rest_raw) return NULL;
    base_len = target[0] == '/' ? 0 : strlen(base_dir);
    target_len = strlen(target);
    rest_len = strlen(rest_raw);
    if (base_len > SIZE_MAX - target_len - 2 ||
        rest_len > SIZE_MAX - base_len - target_len - 2) return NULL;
    length = base_len + target_len + rest_len + 2;
    path = (char *)kmalloc(length);
    if (!path) return NULL;
    position = 0;
    if (base_len) {
        memcpy(path, base_dir, base_len);
        position = base_len;
        if (position == 0 || path[position - 1] != '/') path[position++] = '/';
    }
    memcpy(path + position, target, target_len);
    position += target_len;
    memcpy(path + position, rest_raw, rest_len + 1);
    vfs_normalize_path(path);
    return path;
}

#if 0
static vfs_node_t *vfs_namei_once(
                                  const char *in_path, int follow_final,
                                  int redirected_before, char *redirect,
                                  int *did_redirect) {
    char resolved[VFS_MAX_PATH];
    char prefix[VFS_MAX_PATH];
    char component[VFS_MAX_PATH];
    char target[VFS_MAX_PATH];
    char newpath[VFS_MAX_PATH];
    const char *path;
    const char *remaining;
    const char *rest_raw;
    const char *rest_non_slash;
    char *last;
    size_t plen;
    int i;
    int has_more;
    int node_is_transient;
    int next_ephemeral;
    vfs_mount_t *mount;
    vfs_node_t *node;
    vfs_node_t *next;
    vfs_node_t *parent;

    if (!in_path) return NULL;
    *did_redirect = 0;

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

    if (vfs_apply_task_root(resolved, sizeof(resolved), redirected_before) != 0)
        return NULL;
    path = resolved;

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
        plen = vfs_bounded_strlen(mount->path, VFS_MAX_PATH);
        remaining = path + plen;
        if (*remaining == '/') remaining++;
        memcpy(prefix, mount->path, plen + 1);
        if (plen > 1 && prefix[plen - 1] == '/') prefix[--plen] = '\0';
    } else {
        node = vfs_root;
        remaining = path + 1;
        prefix[0] = '/';
        prefix[1] = '\0';
        plen = 1;
    }

    if (*remaining == '\0') return node;

    node_is_transient = 0;
    while (*remaining) {
        while (*remaining == '/') remaining++;
        if (*remaining == '\0') break;

        i = 0;
        while (*remaining && *remaining != '/' && i < VFS_MAX_PATH - 1) {
            component[i++] = *remaining++;
        }
        component[i] = '\0';

        if (i == 0) continue;

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (node->parent) {
                parent = node->parent;
                if (node_is_transient) vfs_release(node);
                node = parent;
                node_is_transient = 0;
                if (strcmp(prefix, "/") != 0) {
                    last = strrchr(prefix, '/');
                    if (last) {
                        if (last == prefix) {
                            prefix[1] = '\0';
                            plen = 1;
                        } else {
                            *last = '\0';
                            plen = (size_t)(last - prefix);
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
        if (!next) {
            if (node_is_transient) vfs_release(node);
            return NULL;
        }
        vfs_lookup_hazard_set(next);

        if ((next->flags & VFS_MOUNTPOINT) && next->ptr) {
            if (next->flags & VFS_DYNAMIC) vfs_release(next);
            next = next->ptr;
            vfs_lookup_hazard_set(next);
        }

        if (VFS_GET_TYPE(next->flags) == VFS_SYMLINK && (has_more || follow_final)) {
            if (vfs_readlink_node(next, target, sizeof(target)) >= 0) {
                next_ephemeral = (next->flags & VFS_DYNAMIC);
                if (vfs_build_symlink_path(newpath, sizeof(newpath), prefix, target, rest_raw) < 0) {
                    if (node_is_transient) vfs_release(node);
                    if (next_ephemeral) vfs_release(next);
                    return NULL;
                }
                vfs_normalize_path(newpath);
                if (node_is_transient) vfs_release(node);
                if (next_ephemeral) vfs_release(next);
                strcpy(redirect, newpath);
                *did_redirect = 1;
                return NULL;
            }
        }

        if (node_is_transient) vfs_release(node);
        node = next;
        node_is_transient = (next->flags & VFS_DYNAMIC);

        if (plen > 1) {
            if (plen + 1 >= sizeof(prefix)) {
                if (node_is_transient) vfs_release(node);
                return NULL;
            }
            prefix[plen++] = '/';
        }
        if (plen + (size_t)i >= sizeof(prefix)) {
            if (node_is_transient) vfs_release(node);
            return NULL;
        }
        memcpy(prefix + plen, component, (size_t)i + 1);
        plen += (size_t)i;
    }

    return node;
}
#endif

static vfs_node_t *vfs_namei_once_dynamic(const char *in_path,
                                          int follow_final,
                                          int redirected_before,
                                          char **redirect,
                                          int *did_redirect) {
    char *resolved;
    char *prefix;
    char *component;
    char *target;
    char *newpath;
    char *grown;
    const char *remaining;
    const char *component_start;
    const char *rest_raw;
    const char *rest_non_slash;
    char *last;
    size_t plen;
    size_t component_len;
    size_t addition;
    int has_more;
    int node_is_transient;
    int next_ephemeral;
    vfs_mount_t *mount;
    vfs_node_t *node;
    vfs_node_t *next;
    vfs_node_t *parent;

    if (!in_path || !redirect || !did_redirect) return NULL;
    *redirect = NULL;
    *did_redirect = 0;
    resolved = vfs_resolve_path(in_path);
    if (!resolved) return NULL;
    vfs_normalize_path(resolved);
    if (vfs_apply_task_root(&resolved, redirected_before) != 0) {
        kfree(resolved);
        return NULL;
    }
    if (resolved[0] != '/') {
        kfree(resolved);
        return NULL;
    }
    if (squashfs_access_blocked &&
        strncmp(resolved, "/squashfs", 9) == 0 &&
        (resolved[9] == '\0' || resolved[9] == '/')) {
        kfree(resolved);
        return NULL;
    }
    if (resolved[1] == '\0') {
        kfree(resolved);
        return vfs_root;
    }

    mount = find_mount_for_path(resolved);
    if (mount && mount->root) {
        node = mount->root;
        plen = strlen(mount->path);
        prefix = (char *)kmalloc(plen + 1);
        if (!prefix) {
            kfree(resolved);
            return NULL;
        }
        memcpy(prefix, mount->path, plen + 1);
        remaining = resolved + plen;
        if (*remaining == '/') remaining++;
        if (plen > 1 && prefix[plen - 1] == '/') prefix[--plen] = '\0';
    } else {
        node = vfs_root;
        prefix = (char *)kmalloc(2);
        if (!prefix) {
            kfree(resolved);
            return NULL;
        }
        prefix[0] = '/';
        prefix[1] = '\0';
        plen = 1;
        remaining = resolved + 1;
    }
    if (*remaining == '\0') {
        kfree(prefix);
        kfree(resolved);
        return node;
    }

    node_is_transient = 0;
    while (*remaining) {
        while (*remaining == '/') remaining++;
        if (*remaining == '\0') break;
        component_start = remaining;
        while (*remaining && *remaining != '/') remaining++;
        component_len = (size_t)(remaining - component_start);
        if (component_len == SIZE_MAX) break;
        component = (char *)kmalloc(component_len + 1);
        if (!component) {
            if (node_is_transient) vfs_release(node);
            kfree(prefix);
            kfree(resolved);
            return NULL;
        }
        memcpy(component, component_start, component_len);
        component[component_len] = '\0';

        if (strcmp(component, ".") == 0) {
            kfree(component);
            continue;
        }
        if (strcmp(component, "..") == 0) {
            kfree(component);
            if (node->parent) {
                parent = node->parent;
                if (node_is_transient) vfs_release(node);
                node = parent;
                node_is_transient = 0;
                if (strcmp(prefix, "/") != 0) {
                    last = strrchr(prefix, '/');
                    if (last == prefix) {
                        prefix[1] = '\0';
                        plen = 1;
                    } else if (last) {
                        *last = '\0';
                        plen = (size_t)(last - prefix);
                    }
                }
            }
            continue;
        }

        rest_raw = remaining;
        rest_non_slash = rest_raw;
        while (*rest_non_slash == '/') rest_non_slash++;
        has_more = *rest_non_slash != '\0';
        next = vfs_finddir(node, component);
        if (!next) {
            kfree(component);
            if (node_is_transient) vfs_release(node);
            kfree(prefix);
            kfree(resolved);
            return NULL;
        }
        vfs_lookup_hazard_set(next);
        if ((next->flags & VFS_MOUNTPOINT) && next->ptr) {
            if (next->flags & VFS_DYNAMIC) vfs_release(next);
            next = next->ptr;
            vfs_lookup_hazard_set(next);
        }

        if (VFS_GET_TYPE(next->flags) == VFS_SYMLINK &&
            (has_more || follow_final)) {
            target = vfs_readlink_node(next);
            if (target) {
                next_ephemeral = (next->flags & VFS_DYNAMIC) != 0;
                newpath = vfs_build_symlink_path(prefix, target, rest_raw);
                kfree(target);
                kfree(component);
                if (node_is_transient) vfs_release(node);
                if (next_ephemeral) vfs_release(next);
                kfree(prefix);
                kfree(resolved);
                if (!newpath) return NULL;
                *redirect = newpath;
                *did_redirect = 1;
                return NULL;
            }
        }

        if (node_is_transient) vfs_release(node);
        node = next;
        node_is_transient = (next->flags & VFS_DYNAMIC) != 0;
        addition = component_len + (plen > 1 ? 1 : 0);
        if (addition > SIZE_MAX - plen - 1) {
            kfree(component);
            if (node_is_transient) vfs_release(node);
            kfree(prefix);
            kfree(resolved);
            return NULL;
        }
        grown = (char *)krealloc(prefix, plen + addition + 1);
        if (!grown) {
            kfree(component);
            if (node_is_transient) vfs_release(node);
            kfree(prefix);
            kfree(resolved);
            return NULL;
        }
        prefix = grown;
        if (plen > 1) prefix[plen++] = '/';
        memcpy(prefix + plen, component, component_len + 1);
        plen += component_len;
        kfree(component);
    }

    kfree(prefix);
    kfree(resolved);
    return node;
}

static void vfs_free_redirects(char **redirects, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) kfree(redirects[i]);
    kfree(redirects);
}

static vfs_node_t *vfs_namei_internal(const char *path, int follow_final) {
    char *redirect;
    char **redirects;
    char **grown;
    const char *current;
    vfs_node_t *result;
    size_t count;
    size_t i;
    int did_redirect;

    if (!path) return NULL;
    redirects = NULL;
    count = 0;
    current = path;
    for (;;) {
        redirect = NULL;
        result = vfs_namei_once_dynamic(current, follow_final, count != 0,
                                        &redirect, &did_redirect);
        if (!did_redirect) {
            if (redirect) kfree(redirect);
            vfs_free_redirects(redirects, count);
            return result;
        }
        for (i = 0; i < count; i++) {
            if (strcmp(redirects[i], redirect) == 0) {
                vfs_free_redirects(redirects, count);
                kfree(redirect);
                return NULL;
            }
        }
        if (count == SIZE_MAX / sizeof(char *)) {
            kfree(redirect);
            vfs_free_redirects(redirects, count);
            return NULL;
        }
        grown = (char **)krealloc(redirects, (count + 1) * sizeof(char *));
        if (!grown) {
            kfree(redirect);
            vfs_free_redirects(redirects, count);
            return NULL;
        }
        redirects = grown;
        redirects[count++] = redirect;
        current = redirect;
    }
}

vfs_node_t *vfs_namei(const char *path) {
    vfs_node_t *result;

    result = vfs_namei_internal(path, 1);
    return result;
}

void KERNEL_INIT vfs_block_squashfs_access(void) {
    squashfs_access_blocked = 1;
    squashfs_set_access_blocked(1);
}

vfs_node_t *vfs_namei_nofollow(const char *path) {
    return vfs_namei_internal(path, 0);
}

vfs_node_t *vfs_lookup(const char *path) {
    return vfs_namei(path);
}

void vfs_release(vfs_node_t *node) {
    int generic_owned;
    int callback_owns_node;
    uintptr_t close_addr;
    uint64_t refs;

    if (!node) return;
    vfs_lookup_hazard_clear(node);
    refs = __atomic_load_n(&node->ref_count, __ATOMIC_ACQUIRE);
    if (refs == 0) {
        generic_owned = (node->flags & VFS_DYNAMIC) && !(node->flags & VFS_EMBEDDED);
        callback_owns_node = (node->flags & VFS_EMBEDDED) != 0;
        close_addr = (uintptr_t)node->close;
        if (node->close && close_addr >= KERNEL_VMA) {
            node->close(node);
            if (callback_owns_node) return;
        }
        if (generic_owned && node->private_data == NULL) {
            vfs_node_release_name(node);
            kfree(node);
        }
    }
}

char *vfs_get_path_alloc(vfs_node_t *node) {
    vfs_node_t *inline_nodes[8];
    vfs_node_t **nodes;
    vfs_node_t **resized;
    vfs_node_t *cur;
    size_t count;
    size_t capacity;
    size_t total;
    size_t index;
    size_t check;
    size_t len;
    size_t position;
    const char *cur_name;
    char *path;

    if (!node) return NULL;
    nodes = inline_nodes;
    count = 0;
    capacity = sizeof(inline_nodes) / sizeof(inline_nodes[0]);
    total = 2;
    cur = node;
    while (cur && cur != vfs_root) {
        for (check = 0; check < count; check++) {
            if (nodes[check] == cur) {
                if (nodes != inline_nodes) kfree(nodes);
                return NULL;
            }
        }
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2 / sizeof(*nodes)) {
                if (nodes != inline_nodes) kfree(nodes);
                return NULL;
            }
            capacity *= 2;
            resized = (vfs_node_t **)kmalloc(capacity * sizeof(*nodes));
            if (!resized) {
                if (nodes != inline_nodes) kfree(nodes);
                return NULL;
            }
            memcpy(resized, nodes, count * sizeof(*nodes));
            if (nodes != inline_nodes) kfree(nodes);
            nodes = resized;
        }
        nodes[count++] = cur;
        cur_name = vfs_node_name(cur);
        len = strlen(cur_name);
        if (len > SIZE_MAX - total - 1) {
            if (nodes != inline_nodes) kfree(nodes);
            return NULL;
        }
        total += len + 1;
        cur = cur->parent;
    }
    path = (char *)kmalloc(total);
    if (!path) {
        if (nodes != inline_nodes) kfree(nodes);
        return NULL;
    }
    position = 0;
    path[position++] = '/';
    for (index = count; index > 0; index--) {
        cur_name = vfs_node_name(nodes[index - 1]);
        len = strlen(cur_name);
        if (!len) continue;
        if (position > 1) path[position++] = '/';
        memcpy(path + position, cur_name, len);
        position += len;
    }
    path[position] = '\0';
    if (nodes != inline_nodes) kfree(nodes);
    return path;
}

char *vfs_get_path(vfs_node_t *node, char *buf, size_t size) {
    char *path;
    size_t length;

    if (!node || !buf || size == 0) return NULL;
    path = vfs_get_path_alloc(node);
    if (!path) return NULL;
    length = strlen(path);
    if (length >= size) {
        kfree(path);
        return NULL;
    }
    memcpy(buf, path, length + 1);
    kfree(path);
    return buf;
}

static int vfs_split_path_alloc(const char *path, char **parent_out,
                                char **name_out) {
    const char *slash;
    const char *name_start;
    size_t parent_len;
    size_t name_len;
    char *parent;
    char *name;

    if (!path || !parent_out || !name_out) return -1;
    *parent_out = NULL;
    *name_out = NULL;
    slash = strrchr(path, '/');
    if (!slash || slash == path) {
        parent_len = 1;
        name_start = slash ? slash + 1 : path;
    } else {
        parent_len = (size_t)(slash - path);
        name_start = slash + 1;
    }
    name_len = strlen(name_start);
    if (name_len == 0 || parent_len == SIZE_MAX || name_len == SIZE_MAX)
        return -1;
    parent = (char *)kmalloc(parent_len + 1);
    if (!parent) return -1;
    name = (char *)kmalloc(name_len + 1);
    if (!name) {
        kfree(parent);
        return -1;
    }
    if (!slash || slash == path) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
    }
    memcpy(name, name_start, name_len + 1);
    *parent_out = parent;
    *name_out = name;
    return 0;
}

int vfs_open_path(const char *path, int flags) {
    char *parent_path;
    char *filename;
    int ret;
    int fd;
    int i;
    vfs_node_t *node;
    vfs_node_t *parent;

    if (!path) return -1;
    
    node = vfs_namei(path);
    
    if (!node && (flags & VFS_O_CREAT)) {
        if (vfs_split_path_alloc(path, &parent_path, &filename) < 0) {
            return -1;
        }
        
        parent = vfs_namei(parent_path);
        kfree(parent_path);
        if (!parent) {
            kfree(filename);
            return -1;
        }
        
        ret = vfs_create(parent, filename, VFS_FILE);
        kfree(filename);
        vfs_release(parent);
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
        vfs_release(node);
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
    vfs_node_t *node;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    
    mutex_lock(&vfs_lock);
    if (!fd_table[fd].in_use) {
        mutex_unlock(&vfs_lock);
        return -1;
    }
    
    node = fd_table[fd].node;
    
    fd_table[fd].in_use = 0;
    fd_table[fd].node = NULL;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = 0;
    mutex_unlock(&vfs_lock);
    
    if (node) vfs_close(node);
    vfs_reclaim_fds();
    
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
    
    
    return 0;
}

int vfs_readdir_fd(int fd, dirent_t *entry, uint64_t index) {
    vfs_node_t *node;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table[fd].in_use || !entry) return -1;
    
    node = fd_table[fd].node;
    if (!node) return -1;

    return vfs_readdir_copy(node, index, entry);
}

vfs_mount_t *vfs_get_mount_for_node(vfs_node_t *node) {
    vfs_node_t *ancestor;
    int i;

    ancestor = node;
    while (ancestor) {
        for (i = 0; i < mounts_capacity; i++) {
            if (mounts[i].in_use && mounts[i].root == ancestor)
                return &mounts[i];
        }
        ancestor = ancestor->parent;
    }
    return NULL;
}

uint64_t vfs_get_mount_flags_for_node(vfs_node_t *node) {
    vfs_mount_t *mount;

    mount = vfs_get_mount_for_node(node);
    return mount ? mount->flags : 0;
}

int vfs_sync_node(vfs_node_t *node, int data_only) {
    vfs_mount_t *mount;

    if (!node) return -1;
    mount = vfs_get_mount_for_node(node);
    if (!mount || !mount->fs_type || !mount->fs_type->sync) return 0;
    return mount->fs_type->sync(node, data_only != 0);
}

int vfs_sync_all(int data_only) {
    int i;
    int result;

    result = 0;
    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use || !mounts[i].fs_type ||
            !mounts[i].fs_type->sync)
            continue;
        if (mounts[i].fs_type->sync(mounts[i].root, data_only != 0) != 0)
            result = -1;
    }
    return result;
}

int vfs_set_times(vfs_node_t *node, uint64_t atime, uint64_t mtime,
                  uint64_t ctime) {
    vfs_mount_t *mount;

    if (!node) return -1;
    mount = vfs_get_mount_for_node(node);
    if (!mount || !mount->fs_type || !mount->fs_type->name) return -1;
    if (strcmp(mount->fs_type->name, "ramfs") == 0 ||
        strcmp(mount->fs_type->name, "tmpfs") == 0)
        return ramfs_set_times_node(node, atime, mtime, ctime);
    if (strcmp(mount->fs_type->name, "ext4") == 0)
        return ext4_set_times_node(node, atime, mtime, ctime);
    return -1;
}

int vfs_mknod(vfs_node_t *parent, const char *name, uint64_t mode) {
    vfs_mount_t *mount;

    if (!parent || !name) return -1;
    mount = vfs_get_mount_for_node(parent);
    if (!mount || !mount->fs_type || !mount->fs_type->name) return -1;
    if (mount->flags & VFS_MS_RDONLY) return -1;
    if (strcmp(mount->fs_type->name, "ramfs") == 0 ||
        strcmp(mount->fs_type->name, "tmpfs") == 0)
        return ramfs_mknod_node(parent, name, mode);
    if (strcmp(mount->fs_type->name, "ext4") == 0)
        return ext4_mknod_node(parent, name, mode);
    return -1;
}

int vfs_exchange(vfs_node_t *old_parent, const char *old_name,
                 vfs_node_t *new_parent, const char *new_name) {
    vfs_mount_t *old_mount;
    vfs_mount_t *new_mount;
    vfs_node_t *old_node;
    vfs_node_t *new_node;
    vfs_node_t *ancestor;
    int result;

    old_mount = vfs_get_mount_for_node(old_parent);
    new_mount = vfs_get_mount_for_node(new_parent);
    if (!old_mount || old_mount != new_mount || !old_mount->fs_type ||
        !old_mount->fs_type->name || (old_mount->flags & VFS_MS_RDONLY))
        return -1;
    old_node = vfs_finddir(old_parent, old_name);
    new_node = vfs_finddir(new_parent, new_name);
    if (!old_node || !new_node) {
        if (old_node) vfs_release(old_node);
        if (new_node) vfs_release(new_node);
        return -2;
    }
    if (VFS_GET_TYPE(old_node->flags) == VFS_DIRECTORY) {
        ancestor = new_parent;
        while (ancestor) {
            if (ancestor == old_node) {
                vfs_release(old_node);
                vfs_release(new_node);
                return -2;
            }
            ancestor = ancestor->parent;
        }
    }
    if (VFS_GET_TYPE(new_node->flags) == VFS_DIRECTORY) {
        ancestor = old_parent;
        while (ancestor) {
            if (ancestor == new_node) {
                vfs_release(old_node);
                vfs_release(new_node);
                return -2;
            }
            ancestor = ancestor->parent;
        }
    }
    vfs_release(old_node);
    vfs_release(new_node);
    result = -1;
    if (strcmp(old_mount->fs_type->name, "ramfs") == 0 ||
        strcmp(old_mount->fs_type->name, "tmpfs") == 0)
        result = ramfs_exchange_nodes(old_parent, old_name, new_parent,
                                      new_name);
    else if (strcmp(old_mount->fs_type->name, "ext4") == 0)
        result = ext4_exchange_nodes(old_parent, old_name, new_parent,
                                     new_name);
    return result;
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

int KERNEL_INIT vfs_replace_mount_root(const char *mountpoint,
                                       vfs_node_t *new_root,
                                       const char *device,
                                       const char *fs_name) {
    int i;
    vfs_fs_type_t *fs;

    if (!mountpoint || !new_root) return -1;

    fs = fs_name ? vfs_find_fs(fs_name) : NULL;

    for (i = 0; i < mounts_capacity; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, mountpoint) == 0) {
            if (device && vfs_mount_set_strings(&mounts[i], mounts[i].path, device) != 0)
                return -1;
            mounts[i].root = new_root;
            new_root->parent = vfs_root;
            if (strcmp(mountpoint, "/") == 0) {
                vfs_node_release_name(new_root);
            }
            new_root->flags |= VFS_MOUNTPOINT;
            if (fs)
                mounts[i].fs_type = fs;
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
    uint64_t ramfs_idx;
    uint64_t ramfs_count;
    uint64_t target;
    int i;
    int is_dup;
    int k;
    const char *path;
    dirent_t *entry;
    size_t entry_len;
    
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
                if (vfs_dirent_set_name(&root_dirent, path) != 0)
                    return NULL;
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
                target = index - count;
                ramfs_count = 0;
                for (ramfs_idx = 0; ; ramfs_idx++) {
                    entry = mounts[i].root->readdir(mounts[i].root, ramfs_idx);
                    if (!entry) return NULL;

                    path = vfs_dirent_name(entry);
                    entry_len = strlen(path);

                    is_dup = 0;
                    for (k = 0; k < mounts_capacity; k++) {
                        if (mounts[k].in_use && mounts[k].path[0] == '/' &&
                            strlen(mounts[k].path + 1) == entry_len &&
                            memcmp(mounts[k].path + 1, path, entry_len) == 0) {
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
    size_t name_len;
    int i;
    vfs_node_t *found;
    vfs_node_t *root;
    
    (void)node;
    
    if (strcmp(name, "ro") == 0) {
        return NULL;
    }
    if (strcmp(name, "squashfs") == 0) {
        return NULL;
    }
    
    name_len = strlen(name);
    
    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use) continue;
        
        if (strcmp(mounts[i].path, "/ro") == 0) {
            continue;
        }
        if (strcmp(mounts[i].path, "/squashfs") == 0) {
            continue;
        }
        
        if (mounts[i].path[0] == '/' &&
            strlen(mounts[i].path + 1) == name_len &&
            memcmp(mounts[i].path + 1, name, name_len) == 0) {
            return mounts[i].root;
        }
    }
    
    for (i = 0; i < mounts_capacity; i++) {
        if (!mounts[i].in_use) continue;
        if (strcmp(mounts[i].path, "/") == 0 && mounts[i].root) {
            root = mounts[i].root;
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

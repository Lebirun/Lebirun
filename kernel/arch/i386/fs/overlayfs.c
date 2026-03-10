#include <kernel/overlayfs.h>
#include <kernel/vfs.h>
#include <kernel/ramfs.h>
#include <kernel/mem_map.h>
#include <kernel/debug.h>
#include <string.h>
#include <stdio.h>

static overlay_context_t overlay_ctx;
static int overlay_initialized = 0;
static vfs_fs_type_t overlay_fs_type;
static dirent_t overlay_dirent;

static uint64_t overlay_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static uint64_t overlay_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static void overlay_vfs_open(vfs_node_t *node, uint32_t flags);
static void overlay_vfs_close(vfs_node_t *node);
static dirent_t *overlay_vfs_readdir(vfs_node_t *node, uint32_t index);
static vfs_node_t *overlay_vfs_finddir(vfs_node_t *node, const char *name);
static int overlay_vfs_create(vfs_node_t *parent, const char *name, uint32_t flags);
static int overlay_vfs_mkdir(vfs_node_t *parent, const char *name, uint32_t perms);
static int overlay_vfs_unlink(vfs_node_t *parent, const char *name);
static int overlay_vfs_truncate(vfs_node_t *node, uint64_t length);

static int overlay_is_whiteout(const char *name) {
    return strncmp(name, OVERLAY_WHITEOUT_PREFIX, 4) == 0;
}

static int overlay_check_whiteout(vfs_node_t *upper_dir, const char *name) {
    char wh_name[VFS_MAX_NAME];
    vfs_node_t *wh_node;

    if (!upper_dir || !name) return 0;
    
    snprintf(wh_name, sizeof(wh_name), "%s%s", OVERLAY_WHITEOUT_PREFIX, name);
    wh_node = vfs_finddir(upper_dir, wh_name);
    
    return wh_node != NULL;
}

static overlay_node_t *overlay_alloc_node(void) {
    overlay_node_t *node;

    node = (overlay_node_t *)kmalloc(sizeof(overlay_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(overlay_node_t));
    node->ctx = &overlay_ctx;
    return node;
}

static vfs_node_t *overlay_wrap_node(vfs_node_t *lower, vfs_node_t *upper, const char *name) {
    overlay_node_t *onode;
    vfs_node_t *effective;
    size_t name_len;

    onode = overlay_alloc_node();
    if (!onode) return NULL;
    
    onode->lower_node = lower;
    onode->upper_node = upper;
    
    effective = upper ? upper : lower;
    if (!effective) {
        kfree(onode);
        return NULL;
    }
    
    name_len = strlen(name);
    if (name_len >= VFS_MAX_NAME) name_len = VFS_MAX_NAME - 1;
    memcpy(onode->vfs.name, name, name_len);
    onode->vfs.name[name_len] = '\0';
    
    onode->vfs.flags = effective->flags;
    onode->vfs.mask = effective->mask;
    onode->vfs.uid = effective->uid;
    onode->vfs.gid = effective->gid;
    onode->vfs.length = effective->length;
    onode->vfs.inode = effective->inode;
    onode->vfs.mtime = effective->mtime;
    onode->vfs.private_data = onode;
    
    onode->vfs.open = overlay_vfs_open;
    onode->vfs.close = overlay_vfs_close;
    onode->vfs.read = overlay_vfs_read;
    onode->vfs.write = overlay_vfs_write;
    onode->vfs.truncate = overlay_vfs_truncate;
    
    if (VFS_GET_TYPE(effective->flags) == VFS_DIRECTORY) {
        onode->vfs.readdir = overlay_vfs_readdir;
        onode->vfs.finddir = overlay_vfs_finddir;
        onode->vfs.create = overlay_vfs_create;
        onode->vfs.mkdir = overlay_vfs_mkdir;
        onode->vfs.unlink = overlay_vfs_unlink;
    }
    
    return &onode->vfs;
}

static int overlay_copy_up(overlay_node_t *onode, const char *path) {
    vfs_node_t *lower;
    uint8_t *data;
    uint32_t size;
    uint32_t read_bytes;
    int ret;
    char parent_path[VFS_MAX_PATH];
    char name[VFS_MAX_NAME];
    int last_slash;
    int i;
    ramfs_node_t *new_node;

    lower = onode->lower_node;
    if (!lower) return -1;
    if (onode->upper_node) return 0;
    
    last_slash = -1;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash <= 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        strcpy(name, path + 1);
    } else {
        memcpy(parent_path, path, last_slash);
        parent_path[last_slash] = '\0';
        strcpy(name, path + last_slash + 1);
    }
    
    if (VFS_GET_TYPE(lower->flags) == VFS_DIRECTORY) {
        ret = ramfs_create_dir(path, lower->mask);
        if (ret != 0 && ret != RAMFS_ERR_EXIST) return ret;
        
        new_node = ramfs_find_node(path);
        if (new_node) {
            onode->upper_node = new_node->vfs_node;
        }
        return 0;
    }
    
    size = lower->length;
    if (size > 0) {
        data = (uint8_t *)kmalloc(size);
        if (!data) return -1;
        
        read_bytes = vfs_read(lower, 0, size, data);
        
        ret = ramfs_create_file(path, lower->mask);
        if (ret != 0 && ret != RAMFS_ERR_EXIST) {
            kfree(data);
            return ret;
        }
        
        if (read_bytes > 0) {
            ramfs_write(path, 0, data, read_bytes);
        }
        kfree(data);
    } else {
        ret = ramfs_create_file(path, lower->mask);
        if (ret != 0 && ret != RAMFS_ERR_EXIST) return ret;
    }
    
    new_node = ramfs_find_node(path);
    if (new_node) {
        onode->upper_node = new_node->vfs_node;
    }
    
    return 0;
}

static uint64_t overlay_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    overlay_node_t *onode;
    vfs_node_t *effective;

    if (!node || !buffer) return 0;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return 0;
    
    effective = onode->upper_node ? onode->upper_node : onode->lower_node;
    if (!effective) return 0;
    
    return vfs_read(effective, offset, size, buffer);
}

static uint64_t overlay_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    overlay_node_t *onode;
    char path[VFS_MAX_PATH];

    if (!node || !buffer) return 0;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return 0;
    
    if (!onode->upper_node && onode->lower_node) {
        vfs_get_path(node, path, sizeof(path));
        if (overlay_copy_up(onode, path) != 0) {
            return 0;
        }
    }
    
    if (!onode->upper_node) return 0;
    
    return vfs_write(onode->upper_node, offset, size, buffer);
}

static void overlay_vfs_open(vfs_node_t *node, uint32_t flags) {
    overlay_node_t *onode;
    vfs_node_t *effective;

    if (!node) return;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return;
    
    effective = onode->upper_node ? onode->upper_node : onode->lower_node;
    if (effective) vfs_open(effective, flags);
}

static void overlay_vfs_close(vfs_node_t *node) {
    overlay_node_t *onode;
    vfs_node_t *effective;

    if (!node) return;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return;
    
    effective = onode->upper_node ? onode->upper_node : onode->lower_node;
    if (effective) vfs_close(effective);
}

static int overlay_vfs_truncate(vfs_node_t *node, uint64_t length) {
    overlay_node_t *onode;
    char path[VFS_MAX_PATH];

    if (!node) return -1;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return -1;
    
    if (!onode->upper_node && onode->lower_node) {
        vfs_get_path(node, path, sizeof(path));
        if (overlay_copy_up(onode, path) != 0) {
            return -1;
        }
    }
    
    if (!onode->upper_node) return -1;
    
    if (onode->upper_node->truncate) {
        return onode->upper_node->truncate(onode->upper_node, length);
    }
    return -1;
}

static dirent_t *overlay_vfs_readdir(vfs_node_t *node, uint32_t index) {
    overlay_node_t *onode;
    dirent_t *entry;
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    uint32_t upper_count;
    uint32_t visible_count;
    uint32_t i;

    if (!node) return NULL;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return NULL;
    
    upper_dir = onode->upper_node;
    lower_dir = onode->lower_node;
    
    upper_count = 0;
    if (upper_dir) {
        for (i = 0; ; i++) {
            entry = vfs_readdir(upper_dir, i);
            if (!entry) break;
            if (overlay_is_whiteout(entry->name)) continue;
            if (upper_count == index) {
                memcpy(&overlay_dirent, entry, sizeof(dirent_t));
                return &overlay_dirent;
            }
            upper_count++;
        }
    }
    
    if (lower_dir) {
        visible_count = 0;
        for (i = 0; ; i++) {
            entry = vfs_readdir(lower_dir, i);
            if (!entry) break;
            
            if (upper_dir && overlay_check_whiteout(upper_dir, entry->name)) {
                continue;
            }
            if (upper_dir && vfs_finddir(upper_dir, entry->name)) {
                continue;
            }
            
            if (upper_count + visible_count == index) {
                memcpy(&overlay_dirent, entry, sizeof(dirent_t));
                return &overlay_dirent;
            }
            visible_count++;
        }
    }
    
    return NULL;
}

static vfs_node_t *overlay_vfs_finddir(vfs_node_t *node, const char *name) {
    overlay_node_t *onode;
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    vfs_node_t *upper_result;
    vfs_node_t *lower_result;
    vfs_node_t *result;

    if (!node || !name) return NULL;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return NULL;
    
    upper_dir = onode->upper_node;
    lower_dir = onode->lower_node;
    upper_result = NULL;
    lower_result = NULL;
    
    if (upper_dir && overlay_check_whiteout(upper_dir, name)) {
        return NULL;
    }
    
    if (upper_dir) {
        upper_result = vfs_finddir(upper_dir, name);
    }
    
    if (lower_dir) {
        lower_result = vfs_finddir(lower_dir, name);
    }
    
    if (!upper_result && !lower_result) {
        return NULL;
    }
    
    result = overlay_wrap_node(lower_result, upper_result, name);
    if (result) {
        result->parent = node;
    }
    
    return result;
}

static void overlay_ensure_upper_dirs(const char *path) {
    char tmp[VFS_MAX_PATH];
    int i;

    strncpy(tmp, path, VFS_MAX_PATH - 1);
    tmp[VFS_MAX_PATH - 1] = '\0';
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            ramfs_create_dir(tmp, VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
            tmp[i] = '/';
        }
    }
}

static int overlay_vfs_create(vfs_node_t *parent, const char *name, uint32_t flags) {
    overlay_node_t *onode;
    char path[VFS_MAX_PATH];
    char parent_path[VFS_MAX_PATH];
    int ret;
    ramfs_node_t *pnode;

    (void)flags;
    if (!parent || !name) return -1;
    
    onode = (overlay_node_t *)parent->private_data;
    if (!onode) return -1;
    
    vfs_get_path(parent, parent_path, sizeof(parent_path));
    snprintf(path, sizeof(path), "%s/%s", parent_path, name);

    overlay_ensure_upper_dirs(path);
    
    ret = ramfs_create_file(path, VFS_PERM_READ | VFS_PERM_WRITE);
    if (ret == 0 && !onode->upper_node) {
        pnode = ramfs_find_node(parent_path);
        if (pnode) onode->upper_node = pnode->vfs_node;
    }
    return ret;
}

static int overlay_vfs_mkdir(vfs_node_t *parent, const char *name, uint32_t perms) {
    overlay_node_t *onode;
    char path[VFS_MAX_PATH];
    char parent_path[VFS_MAX_PATH];
    int ret;
    ramfs_node_t *pnode;

    if (!parent || !name) return -1;
    
    onode = (overlay_node_t *)parent->private_data;
    if (!onode) return -1;
    
    vfs_get_path(parent, parent_path, sizeof(parent_path));
    snprintf(path, sizeof(path), "%s/%s", parent_path, name);

    overlay_ensure_upper_dirs(path);
    
    ret = ramfs_create_dir(path, perms);
    if (ret == 0 && !onode->upper_node) {
        pnode = ramfs_find_node(parent_path);
        if (pnode) onode->upper_node = pnode->vfs_node;
    }
    return ret;
}

static int overlay_vfs_unlink(vfs_node_t *parent, const char *name) {
    overlay_node_t *onode;
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    vfs_node_t *in_upper;
    vfs_node_t *in_lower;
    char wh_path[VFS_MAX_PATH];
    char parent_path[VFS_MAX_PATH];
    int ret;

    if (!parent || !name) return -1;
    
    onode = (overlay_node_t *)parent->private_data;
    if (!onode) return -1;
    
    upper_dir = onode->upper_node;
    lower_dir = onode->lower_node;
    
    in_upper = upper_dir ? vfs_finddir(upper_dir, name) : NULL;
    in_lower = lower_dir ? vfs_finddir(lower_dir, name) : NULL;
    
    if (in_upper && upper_dir->unlink) {
        ret = upper_dir->unlink(upper_dir, name);
        if (ret != 0) return ret;
    }
    
    if (in_lower) {
        vfs_get_path(parent, parent_path, sizeof(parent_path));
        snprintf(wh_path, sizeof(wh_path), "%s/%s%s", parent_path, OVERLAY_WHITEOUT_PREFIX, name);
        ramfs_create_file(wh_path, VFS_PERM_READ);
    }
    
    return 0;
}

static vfs_node_t *overlay_vfs_do_mount(const char *device, const char *mountpoint) {
    (void)device;
    (void)mountpoint;
    
    if (!overlay_initialized || !overlay_ctx.merged_root) {
        printf("OVERLAYFS: Not initialized\n");
        return NULL;
    }
    
    printf("OVERLAYFS: Mounted on %s\n", mountpoint);
    return overlay_ctx.merged_root;
}

int overlayfs_init(void) {
    memset(&overlay_ctx, 0, sizeof(overlay_ctx));
    overlay_initialized = 0;
    return 0;
}

void overlayfs_vfs_register(void) {
    overlay_fs_type.name = "overlay";
    overlay_fs_type.mount = overlay_vfs_do_mount;
    overlay_fs_type.unmount = NULL;
    overlay_fs_type.next = NULL;
    
    vfs_register_fs(&overlay_fs_type);
}

overlay_context_t *overlayfs_create(vfs_node_t *lower_root, vfs_node_t *upper_root) {
    if (!lower_root) {
        printf("OVERLAYFS: Lower layer required\n");
        return NULL;
    }
    
    overlay_ctx.lower[0].root = lower_root;
    overlay_ctx.lower[0].writable = 0;
    overlay_ctx.lower_count = 1;
    
    overlay_ctx.upper.root = upper_root;
    overlay_ctx.upper.writable = 1;
    
    overlay_ctx.merged_root = overlay_wrap_node(lower_root, upper_root, "");
    if (!overlay_ctx.merged_root) {
        printf("OVERLAYFS: Failed to create merged root\n");
        return NULL;
    }
    
    overlay_initialized = 1;
    printf("OVERLAYFS: Created overlay with lower=%p upper=%p\n", 
           (void*)lower_root, (void*)upper_root);
    
    return &overlay_ctx;
}

vfs_node_t *overlayfs_mount(overlay_context_t *ctx, const char *mountpoint) {
    (void)mountpoint;
    
    if (!ctx || !ctx->merged_root) {
        return NULL;
    }
    
    return ctx->merged_root;
}

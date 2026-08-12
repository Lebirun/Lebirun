#include <lebirun/overlayfs.h>
#include <lebirun/vfs.h>
#include <lebirun/ramfs.h>
#include <lebirun/mem_map.h>
#include <lebirun/mutex.h>
#include <lebirun/common.h>
#include <string.h>
#include <stdio.h>

static overlay_context_t overlay_ctx;
static int overlay_initialized = 0;
static vfs_fs_type_t overlay_fs_type;
static dirent_t overlay_dirent;

typedef struct {
    vfs_node_t *parent;
    char *name;
    overlay_node_t *onode;
} ov_node_cache_entry_t;

static ov_node_cache_entry_t *ov_node_cache;
static uint64_t ov_node_cache_count = 0;
static uint64_t ov_node_cache_capacity = 0;
static mutex_t overlay_node_lock;

static void overlay_try_free_node(overlay_node_t *onode);
static uint64_t overlay_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);

static vfs_node_t *overlay_backing_node(vfs_node_t *node) {
    overlay_node_t *onode;

    if (!node) return NULL;
    if (node->read != overlay_vfs_read) return NULL;
    onode = (overlay_node_t *)node->private_data;
    if (!onode || &onode->vfs != node || onode->ctx != &overlay_ctx) return NULL;
    return onode->upper_node ? onode->upper_node : onode->lower_node;
}

int overlay_same_file(vfs_node_t *first, vfs_node_t *second) {
    vfs_node_t *first_backing;
    vfs_node_t *second_backing;

    if (first == second) return 1;
    first_backing = overlay_backing_node(first);
    second_backing = overlay_backing_node(second);
    if (!first_backing || !second_backing) return 0;
    if (first_backing == second_backing) return 1;
    return first_backing->read == second_backing->read &&
           first_backing->inode == second_backing->inode &&
           first_backing->length == second_backing->length &&
           VFS_GET_TYPE(first_backing->flags) == VFS_GET_TYPE(second_backing->flags);
}

static void overlay_drop_node_refs(overlay_node_t *onode) {
    if (!onode) return;
    if (onode->lower_node) {
        vfs_close(onode->lower_node);
        onode->lower_node = NULL;
    }
    if (onode->upper_node) {
        vfs_close(onode->upper_node);
        onode->upper_node = NULL;
    }
}

static void overlay_release_parent_pin(overlay_node_t *onode) {
    overlay_node_t *parent_onode;
    vfs_node_t *parent;

    if (!onode || !onode->parent_pinned || !onode->vfs.parent) return;

    parent = onode->vfs.parent;
    onode->vfs.parent = NULL;
    onode->parent_pinned = 0;
    parent_onode = (overlay_node_t *)parent->private_data;
    if (parent_onode && parent_onode->refcount > 0) {
        parent_onode->refcount--;
        overlay_try_free_node(parent_onode);
    }
}

static void overlay_try_free_node(overlay_node_t *onode) {
    if (!onode) return;
    if (&onode->vfs == overlay_ctx.merged_root) return;
    if (onode->refcount > 0) return;
    if (onode->vfs.ref_count > 0) return;
    if (onode->open_count > 0) return;
    if (vfs_lookup_hazard_contains(&onode->vfs)) return;
    overlay_release_parent_pin(onode);
    overlay_drop_node_refs(onode);
    onode->vfs.private_data = NULL;
    vfs_node_release_name(&onode->vfs);
    kfree(onode);
}

static void overlay_set_parent(vfs_node_t *child, vfs_node_t *parent) {
    overlay_node_t *child_onode;
    overlay_node_t *parent_onode;

    if (!child) return;

    child_onode = (overlay_node_t *)child->private_data;
    if (!child_onode) {
        child->parent = parent;
        return;
    }

    if (!parent) {
        child->parent = NULL;
        return;
    }

    parent_onode = (overlay_node_t *)parent->private_data;
    if (parent_onode && !child_onode->parent_pinned) {
        parent_onode->refcount++;
        child_onode->parent_pinned = 1;
    }
    child->parent = parent;
}

static int ov_cache_ensure_space(void) {
    ov_node_cache_entry_t *new_cache;

    if (ov_node_cache_count == UINT64_MAX ||
        ov_node_cache_count + 1 >
        SIZE_MAX / sizeof(ov_node_cache_entry_t)) return -1;
    new_cache = (ov_node_cache_entry_t *)krealloc(
        ov_node_cache,
        (ov_node_cache_count + 1) * sizeof(ov_node_cache_entry_t));
    if (!new_cache)
        return -1;
    ov_node_cache = new_cache;
    ov_node_cache_capacity = ov_node_cache_count + 1;
    return 0;
}

void overlay_flush_cache(void) {
    uint64_t i;
    overlay_node_t *onode;

    mutex_lock(&overlay_node_lock);
    for (i = 0; i < ov_node_cache_count; i++) {
        onode = ov_node_cache[i].onode;
        kfree(ov_node_cache[i].name);
        if (!onode) continue;
        onode->refcount--;
        overlay_try_free_node(onode);
    }
    ov_node_cache_count = 0;
    if (ov_node_cache) {
        kfree(ov_node_cache);
        ov_node_cache = NULL;
        ov_node_cache_capacity = 0;
    }
    mutex_unlock(&overlay_node_lock);
}

void overlay_cache_stats(uint64_t *nodes, uint64_t *capacity, uint64_t *bytes) {
    uint64_t i;
    uint64_t total;

    mutex_lock(&overlay_node_lock);
    if (nodes) *nodes = ov_node_cache_count;
    if (capacity) *capacity = ov_node_cache_capacity;
    if (bytes) {
        total = ov_node_cache_capacity * sizeof(ov_node_cache_entry_t) +
                ov_node_cache_count * sizeof(overlay_node_t);
        for (i = 0; i < ov_node_cache_count; i++) {
            if (ov_node_cache[i].name)
                total += strlen(ov_node_cache[i].name) + 1;
        }
        *bytes = total;
    }
    mutex_unlock(&overlay_node_lock);
}

static overlay_node_t *ov_cache_lookup(vfs_node_t *parent, const char *name) {
    uint64_t i;

    if (!ov_node_cache)
        return NULL;

    for (i = 0; i < ov_node_cache_count; i++) {
        if (ov_node_cache[i].parent == parent &&
            strcmp(ov_node_cache[i].name, name) == 0) {
            return ov_node_cache[i].onode;
        }
    }
    return NULL;
}

static void ov_cache_remove(overlay_node_t *onode) {
    uint64_t i;
    ov_node_cache_entry_t *new_cache;

    for (i = 0; i < ov_node_cache_count; i++) {
        if (ov_node_cache[i].onode == onode) {
            kfree(ov_node_cache[i].name);
            ov_node_cache_count--;
            if (i < ov_node_cache_count) {
                memmove(&ov_node_cache[i], &ov_node_cache[i + 1],
                        (ov_node_cache_count - i) * sizeof(ov_node_cache[0]));
            }
            if (ov_node_cache_count == 0) {
                kfree(ov_node_cache);
                ov_node_cache = NULL;
                ov_node_cache_capacity = 0;
            } else {
                new_cache = (ov_node_cache_entry_t *)krealloc(
                    ov_node_cache,
                    ov_node_cache_count * sizeof(ov_node_cache_entry_t));
                if (new_cache) {
                    ov_node_cache = new_cache;
                    ov_node_cache_capacity = ov_node_cache_count;
                }
            }
            return;
        }
    }
}

static void ov_cache_invalidate(vfs_node_t *parent, const char *name) {
    overlay_node_t *cached;

    mutex_lock(&overlay_node_lock);
    cached = ov_cache_lookup(parent, name);
    if (cached) {
        ov_cache_remove(cached);
        if (cached->refcount > 0) cached->refcount--;
        overlay_try_free_node(cached);
    }
    mutex_unlock(&overlay_node_lock);
}

static int ov_cache_insert(vfs_node_t *parent, const char *name, overlay_node_t *onode) {
    size_t nlen;
    char *name_copy;

    if (!name) return 0;
    nlen = strlen(name);
    if (nlen == SIZE_MAX) return 0;
    name_copy = (char *)kmalloc(nlen + 1);
    if (!name_copy) return 0;
    memcpy(name_copy, name, nlen + 1);
    if (ov_cache_ensure_space() < 0) {
        kfree(name_copy);
        return 0;
    }

    if (ov_node_cache_count < ov_node_cache_capacity) {
        onode->refcount++;
        ov_node_cache[ov_node_cache_count].parent = parent;
        ov_node_cache[ov_node_cache_count].name = name_copy;
        ov_node_cache[ov_node_cache_count].onode = onode;
        ov_node_cache_count++;
        return 1;
    }

    kfree(name_copy);
    return 0;
}

static uint64_t overlay_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static uint64_t overlay_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static void overlay_vfs_open(vfs_node_t *node, uint64_t flags);
static void overlay_vfs_close(vfs_node_t *node);
static dirent_t *overlay_vfs_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *overlay_vfs_finddir(vfs_node_t *node, const char *name);
static int overlay_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags);
static int overlay_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms);
static int overlay_vfs_unlink(vfs_node_t *parent, const char *name);
static int overlay_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                              vfs_node_t *new_parent, const char *new_name);
static int overlay_vfs_truncate(vfs_node_t *node, uint64_t length);
static int overlay_vfs_chmod(vfs_node_t *node, uint64_t mode);
static int overlay_vfs_chown(vfs_node_t *node, uint64_t uid, uint64_t gid);
static void overlay_ensure_upper_dirs(const char *path);

static int overlay_ramfs_result(int result) {
    switch (result) {
        case RAMFS_ERR_NOMEM: return -12;
        case RAMFS_ERR_NOENT: return -2;
        case RAMFS_ERR_EXIST: return -17;
        case RAMFS_ERR_NOTDIR: return -20;
        case RAMFS_ERR_ISDIR: return -21;
        case RAMFS_ERR_NOTEMPTY: return -39;
        case RAMFS_ERR_NOSPC: return -28;
        case RAMFS_ERR_INVAL: return -22;
        case RAMFS_ERR_NAMETOOLONG: return -36;
        case RAMFS_ERR_PERM: return -1;
        case RAMFS_ERR_BUSY: return -16;
        default: return result;
    }
}

static int overlay_is_whiteout(const char *name) {
    return strncmp(name, OVERLAY_WHITEOUT_PREFIX, 4) == 0;
}

static void overlay_reset_readdir(overlay_node_t *onode) {
    if (!onode) return;
    onode->rd_last_index = 0;
    onode->rd_upper_count = 0;
    onode->rd_lower_index = 0;
    onode->rd_visible_count = 0;
    onode->rd_phase = 0;
}

static int overlay_check_whiteout(vfs_node_t *upper_dir, const char *name) {
    char *wh_name;
    vfs_node_t *wh_node;
    int found;
    size_t length;

    if (!upper_dir || !name) return 0;
    length = strlen(name);
    if (length > SIZE_MAX - 5) return 0;
    wh_name = (char *)kmalloc(length + 5);
    if (!wh_name) return 0;
    memcpy(wh_name, OVERLAY_WHITEOUT_PREFIX, 4);
    memcpy(wh_name + 4, name, length + 1);
    wh_node = vfs_finddir(upper_dir, wh_name);
    found = wh_node != NULL;
    if (wh_node) vfs_release(wh_node);
    kfree(wh_name);
    return found;
}

static overlay_node_t *overlay_alloc_node(void) {
    overlay_node_t *node;

    node = (overlay_node_t *)kmalloc(sizeof(overlay_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(overlay_node_t));
    node->ctx = &overlay_ctx;
    node->refcount = 0;
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

    if (lower) __atomic_add_fetch(&lower->ref_count, 1, __ATOMIC_ACQ_REL);
    if (upper) __atomic_add_fetch(&upper->ref_count, 1, __ATOMIC_ACQ_REL);
    
    effective = upper ? upper : lower;
    if (!effective) {
        kfree(onode);
        return NULL;
    }
    
    onode->vfs.flags = effective->flags & ~(VFS_DYNAMIC | VFS_NAME_DYNAMIC);
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
    onode->vfs.chmod = overlay_vfs_chmod;
    onode->vfs.chown = overlay_vfs_chown;
    
    if (VFS_GET_TYPE(effective->flags) == VFS_DIRECTORY) {
        onode->vfs.readdir = overlay_vfs_readdir;
        onode->vfs.finddir = overlay_vfs_finddir;
        onode->vfs.create = overlay_vfs_create;
        onode->vfs.mkdir = overlay_vfs_mkdir;
        onode->vfs.unlink = overlay_vfs_unlink;
        onode->vfs.rename = overlay_vfs_rename;
    }

    name_len = strlen(name);
    if (vfs_node_set_name_n(&onode->vfs, name, name_len) != 0) {
        overlay_drop_node_refs(onode);
        kfree(onode);
        return NULL;
    }
    
    return &onode->vfs;
}

static int overlay_copy_up(overlay_node_t *onode, const char *path) {
    vfs_node_t *lower;
    uint8_t *data;
    uint64_t size;
    uint64_t read_bytes;
    int ret;
    ramfs_node_t *new_node;

    lower = onode->lower_node;
    if (!lower) return -1;
    if (onode->upper_node) return 0;
    
    overlay_ensure_upper_dirs(path);

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
    char *path;
    uint64_t written;

    if (!node || !buffer) return 0;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return 0;
    
    if (!onode->upper_node && onode->lower_node) {
        path = vfs_get_path_alloc(node);
        if (!path) return 0;
        if (overlay_copy_up(onode, path) != 0) {
            kfree(path);
            return 0;
        }
        kfree(path);
    }
    
    if (!onode->upper_node) return 0;
    
    written = vfs_write(onode->upper_node, offset, size, buffer);
    if (written > 0)
        node->length = onode->upper_node->length;
    return written;
}

static void overlay_vfs_open(vfs_node_t *node, uint64_t flags) {
    overlay_node_t *onode;
    vfs_node_t *effective;

    if (!node) return;

    mutex_lock(&overlay_node_lock);
    onode = (overlay_node_t *)node->private_data;
    if (!onode) {
        mutex_unlock(&overlay_node_lock);
        return;
    }

    onode->open_count++;
    onode->refcount++;

    effective = onode->upper_node ? onode->upper_node : onode->lower_node;
    if (effective) vfs_open(effective, flags);
    mutex_unlock(&overlay_node_lock);
}

static void overlay_vfs_close(vfs_node_t *node) {
    overlay_node_t *onode;
    vfs_node_t *effective;

    if (!node) return;

    mutex_lock(&overlay_node_lock);
    onode = (overlay_node_t *)node->private_data;
    if (!onode) {
        mutex_unlock(&overlay_node_lock);
        return;
    }

    if (node == overlay_ctx.merged_root) {
        mutex_unlock(&overlay_node_lock);
        return;
    }

    if (onode->open_count > 0) {
        effective = onode->upper_node ? onode->upper_node : onode->lower_node;
        if (effective) vfs_close(effective);
        onode->open_count--;
        if (onode->refcount > 0) {
            onode->refcount--;
        }
    }

    if (onode->refcount > 0) {
        mutex_unlock(&overlay_node_lock);
        return;
    }

    ov_cache_remove(onode);
    overlay_try_free_node(onode);
    mutex_unlock(&overlay_node_lock);
}

static int overlay_vfs_truncate(vfs_node_t *node, uint64_t length) {
    overlay_node_t *onode;
    char *path;
    int ret;

    if (!node) return -1;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return -1;
    
    if (!onode->upper_node && onode->lower_node) {
        path = vfs_get_path_alloc(node);
        if (!path) return -1;
        if (overlay_copy_up(onode, path) != 0) {
            kfree(path);
            return -1;
        }
        kfree(path);
    }
    
    if (!onode->upper_node) return -1;
    
    if (onode->upper_node->truncate) {
        ret = onode->upper_node->truncate(onode->upper_node, length);
        if (ret == 0)
            node->length = onode->upper_node->length;
        return ret;
    }
    return -1;
}

static int overlay_vfs_chmod(vfs_node_t *node, uint64_t mode) {
    overlay_node_t *onode;
    char *path;
    vfs_node_t *target;
    int ret;

    if (!node) return -1;

    onode = (overlay_node_t *)node->private_data;
    if (!onode) return -1;

    if (!onode->upper_node && onode->lower_node) {
        path = vfs_get_path_alloc(node);
        if (!path) return -1;
        if (overlay_copy_up(onode, path) != 0) {
            kfree(path);
            return -1;
        }
        kfree(path);
    }

    target = onode->upper_node ? onode->upper_node : onode->lower_node;
    if (!target) return -1;

    if (target->chmod) {
        ret = target->chmod(target, mode);
        if (ret == 0) node->mask = mode;
        return ret;
    }
    target->mask = mode;
    node->mask = mode;
    return 0;
}

static int overlay_vfs_chown(vfs_node_t *node, uint64_t uid, uint64_t gid) {
    overlay_node_t *onode;
    char *path;
    vfs_node_t *target;
    int ret;

    if (!node) return -1;

    onode = (overlay_node_t *)node->private_data;
    if (!onode) return -1;

    if (!onode->upper_node && onode->lower_node) {
        path = vfs_get_path_alloc(node);
        if (!path) return -1;
        if (overlay_copy_up(onode, path) != 0) {
            kfree(path);
            return -1;
        }
        kfree(path);
    }

    target = onode->upper_node ? onode->upper_node : onode->lower_node;
    if (!target) return -1;

    if (target->chown) {
        ret = target->chown(target, uid, gid);
        if (ret == 0) {
            node->uid = target->uid;
            node->gid = target->gid;
        }
        return ret;
    }
    if ((int)uid != -1) { target->uid = uid; node->uid = uid; }
    if ((int)gid != -1) { target->gid = gid; node->gid = gid; }
    return 0;
}

static dirent_t *overlay_vfs_readdir(vfs_node_t *node, uint64_t index) {
    overlay_node_t *onode;
    dirent_t *entry;
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    vfs_node_t *upper_match;
    uint64_t upper_count;
    uint64_t visible_count;
    uint64_t i;

    if (!node) return NULL;
    
    onode = (overlay_node_t *)node->private_data;
    if (!onode) return NULL;
    
    upper_dir = onode->upper_node;
    lower_dir = onode->lower_node;

    if (index == 0) {
        overlay_reset_readdir(onode);
    }

    if (upper_dir && onode->rd_phase == 1 && index == onode->rd_last_index + 1) {
        visible_count = onode->rd_visible_count;
        for (i = onode->rd_lower_index; ; i++) {
            entry = vfs_readdir(upper_dir, i);
            if (!entry) break;
            if (overlay_is_whiteout(vfs_dirent_name(entry))) continue;
            if (visible_count == index) {
                memcpy(&overlay_dirent, entry, sizeof(dirent_t));
                onode->rd_last_index = index;
                onode->rd_upper_count = 0;
                onode->rd_lower_index = i + 1;
                onode->rd_visible_count = visible_count + 1;
                onode->rd_phase = 1;
                return &overlay_dirent;
            }
            visible_count++;
        }
        upper_count = visible_count;
    } else {
        upper_count = 0;
    }

    if (lower_dir && onode->rd_phase == 2 && index == onode->rd_last_index + 1) {
        upper_count = onode->rd_upper_count;
        visible_count = onode->rd_visible_count;
        for (i = onode->rd_lower_index; ; i++) {
            entry = vfs_readdir(lower_dir, i);
            if (!entry) break;

            if (upper_dir && overlay_check_whiteout(upper_dir, vfs_dirent_name(entry))) {
                continue;
            }
            upper_match = upper_dir ? vfs_finddir(upper_dir, vfs_dirent_name(entry)) : NULL;
            if (upper_match) {
                vfs_release(upper_match);
                continue;
            }

            if (upper_count + visible_count == index) {
                memcpy(&overlay_dirent, entry, sizeof(dirent_t));
                onode->rd_last_index = index;
                onode->rd_upper_count = upper_count;
                onode->rd_lower_index = i + 1;
                onode->rd_visible_count = visible_count + 1;
                onode->rd_phase = 2;
                return &overlay_dirent;
            }
            visible_count++;
        }
        overlay_reset_readdir(onode);
        return NULL;
    }

    if (upper_dir && upper_count == 0) {
        for (i = 0; ; i++) {
            entry = vfs_readdir(upper_dir, i);
            if (!entry) break;
            if (overlay_is_whiteout(vfs_dirent_name(entry))) continue;
            if (upper_count == index) {
                memcpy(&overlay_dirent, entry, sizeof(dirent_t));
                onode->rd_last_index = index;
                onode->rd_upper_count = 0;
                onode->rd_lower_index = i + 1;
                onode->rd_visible_count = upper_count + 1;
                onode->rd_phase = 1;
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
            
            if (upper_dir && overlay_check_whiteout(upper_dir, vfs_dirent_name(entry))) {
                continue;
            }
            upper_match = upper_dir ? vfs_finddir(upper_dir, vfs_dirent_name(entry)) : NULL;
            if (upper_match) {
                vfs_release(upper_match);
                continue;
            }
            
            if (upper_count + visible_count == index) {
                memcpy(&overlay_dirent, entry, sizeof(dirent_t));
                onode->rd_last_index = index;
                onode->rd_upper_count = upper_count;
                onode->rd_lower_index = i + 1;
                onode->rd_visible_count = visible_count + 1;
                onode->rd_phase = 2;
                return &overlay_dirent;
            }
            visible_count++;
        }
    }
    
    overlay_reset_readdir(onode);
    return NULL;
}

static vfs_node_t *overlay_vfs_finddir(vfs_node_t *node, const char *name) {
    overlay_node_t *onode;
    overlay_node_t *cached;
    overlay_node_t *discarded;
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    vfs_node_t *upper_result;
    vfs_node_t *lower_result;
    vfs_node_t *result;
    vfs_node_t *cached_result;

    if (!node || !name) return NULL;

    mutex_lock(&overlay_node_lock);
    onode = (overlay_node_t *)node->private_data;
    if (!onode) {
        mutex_unlock(&overlay_node_lock);
        return NULL;
    }

    cached = ov_cache_lookup(node, name);
    if (cached) {
        result = &cached->vfs;
        vfs_lookup_hazard_set(result);
        mutex_unlock(&overlay_node_lock);
        return result;
    }

    upper_dir = onode->upper_node;
    lower_dir = onode->lower_node;
    upper_result = NULL;
    lower_result = NULL;
    mutex_unlock(&overlay_node_lock);

    if (upper_dir && overlay_check_whiteout(upper_dir, name)) {
        return NULL;
    }

    if (upper_dir) {
        upper_result = vfs_finddir(upper_dir, name);
        if (upper_result)
            vfs_open(upper_result, 0);
    }

    if (lower_dir) {
        lower_result = vfs_finddir(lower_dir, name);
        if (lower_result)
            vfs_open(lower_result, 0);
    }

    if (!upper_result && !lower_result) {
        return NULL;
    }

    result = overlay_wrap_node(lower_result, upper_result, name);
    if (upper_result) vfs_close(upper_result);
    if (lower_result) vfs_close(lower_result);
    if (!result) return NULL;

    mutex_lock(&overlay_node_lock);
    cached = ov_cache_lookup(node, name);
    if (cached) {
        cached_result = &cached->vfs;
        vfs_lookup_hazard_set(cached_result);
        mutex_unlock(&overlay_node_lock);
        discarded = (overlay_node_t *)result->private_data;
        overlay_drop_node_refs(discarded);
        discarded->vfs.private_data = NULL;
        kfree(discarded);
        return cached_result;
    }

    overlay_set_parent(result, node);
    vfs_lookup_hazard_set(result);
    if (!ov_cache_insert(node, name,
                         (overlay_node_t *)result->private_data)) {
        result->flags |= VFS_DYNAMIC | VFS_EMBEDDED;
    }
    mutex_unlock(&overlay_node_lock);
    return result;
}

static void overlay_ensure_upper_dirs(const char *path) {
    char *tmp;
    size_t length;
    size_t i;

    if (!path) return;
    length = strlen(path);
    tmp = (char *)kmalloc(length + 1);
    if (!tmp) return;
    memcpy(tmp, path, length + 1);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            ramfs_create_dir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    kfree(tmp);
}

static char *overlay_join_path(const char *parent, const char *prefix,
                               const char *name) {
    size_t parent_length;
    size_t prefix_length;
    size_t name_length;
    size_t position;
    char *path;

    if (!parent || !prefix || !name) return NULL;
    parent_length = strlen(parent);
    prefix_length = strlen(prefix);
    name_length = strlen(name);
    if (prefix_length > SIZE_MAX - name_length ||
        prefix_length + name_length > SIZE_MAX - 2 ||
        parent_length > SIZE_MAX - prefix_length - name_length - 2)
        return NULL;
    path = (char *)kmalloc(parent_length + prefix_length + name_length + 2);
    if (!path) return NULL;
    memcpy(path, parent, parent_length);
    position = parent_length;
    if (position == 0 || path[position - 1] != '/') path[position++] = '/';
    memcpy(path + position, prefix, prefix_length);
    position += prefix_length;
    memcpy(path + position, name, name_length + 1);
    return path;
}

static int overlay_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags) {
    overlay_node_t *onode;
    char *path;
    char *parent_path;
    int ret;
    ramfs_node_t *pnode;

    (void)flags;
    if (!parent || !name) return -1;
    
    onode = (overlay_node_t *)parent->private_data;
    if (!onode) return -1;
    
    parent_path = vfs_get_path_alloc(parent);
    if (!parent_path) return -1;
    path = overlay_join_path(parent_path, "", name);
    if (!path) {
        kfree(parent_path);
        return -1;
    }

    overlay_ensure_upper_dirs(path);
    
    ret = ramfs_create_file(path, 0644);
    if (ret == 0 && !onode->upper_node) {
        pnode = ramfs_find_node(parent_path);
        if (pnode) onode->upper_node = pnode->vfs_node;
    }
    if (ret == 0) overlay_reset_readdir(onode);
    kfree(path);
    kfree(parent_path);
    return overlay_ramfs_result(ret);
}

static int overlay_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
    overlay_node_t *onode;
    char *path;
    char *parent_path;
    int ret;
    ramfs_node_t *pnode;

    if (!parent || !name) return -1;
    
    onode = (overlay_node_t *)parent->private_data;
    if (!onode) return -1;
    
    parent_path = vfs_get_path_alloc(parent);
    if (!parent_path) return -1;
    path = overlay_join_path(parent_path, "", name);
    if (!path) {
        kfree(parent_path);
        return -1;
    }

    overlay_ensure_upper_dirs(path);
    
    ret = ramfs_create_dir(path, perms);
    if (ret == 0 && !onode->upper_node) {
        pnode = ramfs_find_node(parent_path);
        if (pnode) onode->upper_node = pnode->vfs_node;
    }
    if (ret == 0) overlay_reset_readdir(onode);
    kfree(path);
    kfree(parent_path);
    return overlay_ramfs_result(ret);
}

static int overlay_vfs_unlink(vfs_node_t *parent, const char *name) {
    overlay_node_t *onode;
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    vfs_node_t *in_upper;
    vfs_node_t *in_lower;
    char *wh_path;
    char *parent_path;
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
        if (ret != 0) {
            vfs_release(in_upper);
            if (in_lower) vfs_release(in_lower);
            return overlay_ramfs_result(ret);
        }
    }
    
    if (in_lower) {
        parent_path = vfs_get_path_alloc(parent);
        if (!parent_path) {
            if (in_upper) vfs_release(in_upper);
            vfs_release(in_lower);
            return -1;
        }
        wh_path = overlay_join_path(parent_path, OVERLAY_WHITEOUT_PREFIX,
                                    name);
        kfree(parent_path);
        if (!wh_path) {
            if (in_upper) vfs_release(in_upper);
            vfs_release(in_lower);
            return -1;
        }
        ret = ramfs_create_file(wh_path, 0644);
        kfree(wh_path);
        if (ret != 0 && ret != RAMFS_ERR_EXIST) {
            if (in_upper) vfs_release(in_upper);
            vfs_release(in_lower);
            return overlay_ramfs_result(ret);
        }
    }

    if (in_upper) vfs_release(in_upper);
    if (in_lower) vfs_release(in_lower);
    ov_cache_invalidate(parent, name);
    overlay_reset_readdir(onode);
    return 0;
}

static int overlay_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                              vfs_node_t *new_parent, const char *new_name) {
    overlay_node_t *old_parent_node;
    overlay_node_t *new_parent_node;
    overlay_node_t *source_node;
    vfs_node_t *source;
    char *old_parent_path;
    char *new_parent_path;
    char *old_path;
    char *new_path;
    char *whiteout_path;
    int has_lower;
    int whiteout_created;
    int ret;

    if (!old_parent || !old_name || !new_parent || !new_name) return -1;
    old_parent_node = (overlay_node_t *)old_parent->private_data;
    new_parent_node = (overlay_node_t *)new_parent->private_data;
    if (!old_parent_node || !new_parent_node) return -1;

    source = vfs_finddir(old_parent, old_name);
    if (!source) return -2;
    source_node = (overlay_node_t *)source->private_data;
    if (!source_node) {
        vfs_release(source);
        return -1;
    }

    old_parent_path = vfs_get_path_alloc(old_parent);
    new_parent_path = vfs_get_path_alloc(new_parent);
    old_path = old_parent_path ?
               overlay_join_path(old_parent_path, "", old_name) : NULL;
    new_path = new_parent_path ?
               overlay_join_path(new_parent_path, "", new_name) : NULL;
    whiteout_path = old_parent_path ?
                    overlay_join_path(old_parent_path,
                                      OVERLAY_WHITEOUT_PREFIX, old_name) :
                    NULL;
    if (!old_parent_path || !new_parent_path || !old_path || !new_path ||
        !whiteout_path) {
        ret = -1;
        goto rename_free_paths;
    }
    has_lower = source_node->lower_node != NULL;

    if (!source_node->upper_node &&
        overlay_copy_up(source_node, old_path) != 0) {
        ret = -1;
        goto rename_free_paths;
    }
    overlay_ensure_upper_dirs(new_path);
    whiteout_created = 0;
    if (has_lower) {
        ret = ramfs_create_file(whiteout_path, 0644);
        if (ret != 0 && ret != RAMFS_ERR_EXIST) {
            goto rename_free_paths;
        }
        whiteout_created = ret == 0;
    }

    ret = ramfs_rename(old_path, new_path);
    if (ret != 0) {
        if (whiteout_created) ramfs_unlink(whiteout_path);
        ret = overlay_ramfs_result(ret);
        goto rename_free_paths;
    }

    ov_cache_invalidate(old_parent, old_name);
    ov_cache_invalidate(new_parent, new_name);
    overlay_reset_readdir(old_parent_node);
    if (new_parent_node != old_parent_node)
        overlay_reset_readdir(new_parent_node);
    ret = 0;

rename_free_paths:
    if (old_parent_path) kfree(old_parent_path);
    if (new_parent_path) kfree(new_parent_path);
    if (old_path) kfree(old_path);
    if (new_path) kfree(new_path);
    if (whiteout_path) kfree(whiteout_path);
    vfs_release(source);
    return ret;
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
    mutex_init(&overlay_node_lock);
    memset(&overlay_ctx, 0, sizeof(overlay_ctx));
    overlay_initialized = 0;
    return 0;
}

void KERNEL_INIT overlayfs_vfs_register(void) {
    overlay_fs_type.name = "overlay";
    overlay_fs_type.mount = overlay_vfs_do_mount;
    overlay_fs_type.unmount = NULL;
    overlay_fs_type.next = NULL;
    
    vfs_register_fs(&overlay_fs_type);
}

overlay_context_t *KERNEL_INIT overlayfs_create(vfs_node_t *lower_root,
                                                vfs_node_t *upper_root) {
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

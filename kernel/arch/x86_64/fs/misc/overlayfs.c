#include <lebirun/overlayfs.h>
#include <lebirun/vfs.h>
#include <lebirun/ramfs.h>
#include <lebirun/mem_map.h>
#include <lebirun/mutex.h>
#include <string.h>
#include <stdio.h>

static overlay_context_t overlay_ctx;
static int overlay_initialized = 0;
static vfs_fs_type_t overlay_fs_type;
static dirent_t overlay_dirent;

#define OVERLAY_NODE_CACHE_SIZE 64
typedef struct {
    vfs_node_t *parent;
    char name[64];
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
    uint64_t new_cap;
    ov_node_cache_entry_t *new_cache;

    if (ov_node_cache_count < ov_node_cache_capacity)
        return 0;
    if (ov_node_cache_capacity >= OVERLAY_NODE_CACHE_SIZE)
        return 0;

    new_cap = ov_node_cache_capacity ? ov_node_cache_capacity * 2 : 8;
    if (new_cap > OVERLAY_NODE_CACHE_SIZE)
        new_cap = OVERLAY_NODE_CACHE_SIZE;
    new_cache = (ov_node_cache_entry_t *)krealloc(ov_node_cache, new_cap * sizeof(ov_node_cache_entry_t));
    if (!new_cache)
        return -1;
    ov_node_cache = new_cache;
    ov_node_cache_capacity = new_cap;
    return 0;
}

void overlay_flush_cache(void) {
    uint64_t i;
    overlay_node_t *onode;

    mutex_lock(&overlay_node_lock);
    for (i = 0; i < ov_node_cache_count; i++) {
        onode = ov_node_cache[i].onode;
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
    mutex_lock(&overlay_node_lock);
    if (nodes) *nodes = ov_node_cache_count;
    if (capacity) *capacity = ov_node_cache_capacity;
    if (bytes) {
        *bytes = ov_node_cache_capacity * sizeof(ov_node_cache_entry_t) +
                 ov_node_cache_count * sizeof(overlay_node_t);
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
            ov_node_cache_count--;
            if (i < ov_node_cache_count) {
                memmove(&ov_node_cache[i], &ov_node_cache[i + 1],
                        (ov_node_cache_count - i) * sizeof(ov_node_cache[0]));
            }
            if (ov_node_cache_count == 0) {
                kfree(ov_node_cache);
                ov_node_cache = NULL;
                ov_node_cache_capacity = 0;
            } else if (ov_node_cache_capacity > ov_node_cache_count * 2) {
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

static int ov_cache_insert(vfs_node_t *parent, const char *name, overlay_node_t *onode) {
    size_t nlen;

    if (ov_cache_ensure_space() < 0)
        return 0;

    if (ov_node_cache_count < ov_node_cache_capacity) {
        onode->refcount++;
        ov_node_cache[ov_node_cache_count].parent = parent;
        nlen = strlen(name);
        if (nlen >= 64) nlen = 63;
        memcpy(ov_node_cache[ov_node_cache_count].name, name, nlen);
        ov_node_cache[ov_node_cache_count].name[nlen] = '\0';
        ov_node_cache[ov_node_cache_count].onode = onode;
        ov_node_cache_count++;
        return 1;
    }

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
static int overlay_vfs_truncate(vfs_node_t *node, uint64_t length);
static int overlay_vfs_chmod(vfs_node_t *node, uint64_t mode);
static int overlay_vfs_chown(vfs_node_t *node, uint64_t uid, uint64_t gid);
static void overlay_ensure_upper_dirs(const char *path);

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
    char wh_name[VFS_MAX_NAME];
    vfs_node_t *wh_node;
    int found;

    if (!upper_dir || !name) return 0;
    
    snprintf(wh_name, sizeof(wh_name), "%s%s", OVERLAY_WHITEOUT_PREFIX, name);
    wh_node = vfs_finddir(upper_dir, wh_name);
    found = wh_node != NULL;
    if (wh_node) vfs_release(wh_node);
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
    
    name_len = strlen(name);
    if (name_len >= VFS_MAX_NAME) name_len = VFS_MAX_NAME - 1;
    memcpy(onode->vfs.name, name, name_len);
    onode->vfs.name[name_len] = '\0';
    
    onode->vfs.flags = effective->flags & ~VFS_DYNAMIC;
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
    char path[VFS_MAX_PATH];
    uint64_t written;

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
        int ret = onode->upper_node->truncate(onode->upper_node, length);
        if (ret == 0)
            node->length = onode->upper_node->length;
        return ret;
    }
    return -1;
}

static int overlay_vfs_chmod(vfs_node_t *node, uint64_t mode) {
    overlay_node_t *onode;
    char path[VFS_MAX_PATH];
    vfs_node_t *target;
    int ret;

    if (!node) return -1;

    onode = (overlay_node_t *)node->private_data;
    if (!onode) return -1;

    if (!onode->upper_node && onode->lower_node) {
        vfs_get_path(node, path, sizeof(path));
        if (overlay_copy_up(onode, path) != 0) {
            return -1;
        }
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
    char path[VFS_MAX_PATH];
    vfs_node_t *target;
    int ret;

    if (!node) return -1;

    onode = (overlay_node_t *)node->private_data;
    if (!onode) return -1;

    if (!onode->upper_node && onode->lower_node) {
        vfs_get_path(node, path, sizeof(path));
        if (overlay_copy_up(onode, path) != 0) {
            return -1;
        }
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
            if (overlay_is_whiteout(entry->name)) continue;
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

            if (upper_dir && overlay_check_whiteout(upper_dir, entry->name)) {
                continue;
            }
            upper_match = upper_dir ? vfs_finddir(upper_dir, entry->name) : NULL;
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
            if (overlay_is_whiteout(entry->name)) continue;
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
            
            if (upper_dir && overlay_check_whiteout(upper_dir, entry->name)) {
                continue;
            }
            upper_match = upper_dir ? vfs_finddir(upper_dir, entry->name) : NULL;
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
    vfs_node_t *upper_dir;
    vfs_node_t *lower_dir;
    vfs_node_t *upper_result;
    vfs_node_t *lower_result;
    vfs_node_t *result;

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

    if (upper_dir && overlay_check_whiteout(upper_dir, name)) {
        mutex_unlock(&overlay_node_lock);
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
        mutex_unlock(&overlay_node_lock);
        return NULL;
    }

    result = overlay_wrap_node(lower_result, upper_result, name);
    if (result) {
        if (upper_result)
            vfs_close(upper_result);
        if (lower_result)
            vfs_close(lower_result);
        overlay_set_parent(result, node);
        vfs_lookup_hazard_set(result);
        if (!ov_cache_insert(node, name,
                             (overlay_node_t *)result->private_data)) {
            result->flags |= VFS_DYNAMIC | VFS_EMBEDDED;
        }
    } else {
        if (upper_result) vfs_close(upper_result);
        if (lower_result) vfs_close(lower_result);
    }

    mutex_unlock(&overlay_node_lock);
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
            ramfs_create_dir(tmp, 0755);
            tmp[i] = '/';
        }
    }
}

static int overlay_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags) {
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
    
    ret = ramfs_create_file(path, 0644);
    if (ret == 0 && !onode->upper_node) {
        pnode = ramfs_find_node(parent_path);
        if (pnode) onode->upper_node = pnode->vfs_node;
    }
    if (ret == 0) overlay_reset_readdir(onode);
    return ret;
}

static int overlay_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
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
    if (ret == 0) overlay_reset_readdir(onode);
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
        if (ret != 0) {
            vfs_release(in_upper);
            if (in_lower) vfs_release(in_lower);
            return ret;
        }
    }
    
    if (in_lower) {
        vfs_get_path(parent, parent_path, sizeof(parent_path));
        snprintf(wh_path, sizeof(wh_path), "%s/%s%s", parent_path, OVERLAY_WHITEOUT_PREFIX, name);
        ramfs_create_file(wh_path, 0644);
    }

    if (in_upper) vfs_release(in_upper);
    if (in_lower) vfs_release(in_lower);
    overlay_reset_readdir(onode);
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
    mutex_init(&overlay_node_lock);
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

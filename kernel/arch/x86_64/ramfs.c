#include <lebirun/ramfs.h>
#include <lebirun/vfs.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/idt.h>
#include <lebirun/task.h>
#include <string.h>
#include <stdio.h>

extern task_t *current_task;

#define RAMFS_NODE_FILE    0
#define RAMFS_NODE_DIR     1
#define RAMFS_NODE_SYMLINK 2

static ramfs_node_t *ramfs_root = NULL;
static vfs_node_t *ramfs_vfs_root = NULL;
static dirent_t ramfs_dirent;
static uint64_t ramfs_next_inode = 1;

static ramfs_stats_t ramfs_stats;
static int ramfs_stats_initialized = 0;

void ramfs_debug_check_root(const char *location) {
    (void)location;
}

static uint64_t ramfs_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static uint64_t ramfs_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static void ramfs_vfs_open(vfs_node_t *node, uint64_t flags);
static void ramfs_vfs_close(vfs_node_t *node);
static int ramfs_vfs_truncate(vfs_node_t *node, uint64_t length);
static int ramfs_vfs_chmod(vfs_node_t *node, uint64_t mode);
static int ramfs_vfs_chown(vfs_node_t *node, uint64_t uid, uint64_t gid);
static dirent_t *ramfs_vfs_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *ramfs_vfs_finddir(vfs_node_t *node, const char *name);
static int ramfs_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags);
static int ramfs_vfs_unlink(vfs_node_t *parent, const char *name);
static int ramfs_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms);
static int ramfs_vfs_rename(vfs_node_t *old_parent, const char *old_name, 
                            vfs_node_t *new_parent, const char *new_name);
static void ramfs_setup_vfs_file_callbacks(vfs_node_t *vn);
static void ramfs_setup_vfs_dir_callbacks(vfs_node_t *vn);

static void ramfs_init_stats(void) {
    uint64_t ram_kb;
    if (!ramfs_stats_initialized) {
        ram_kb = (uint64_t)pfa_get_usable_ram_kb();
        ramfs_stats.total_size = ram_kb * 1024;
        ramfs_stats.used_size = 0;
        ramfs_stats.file_count = 0;
        ramfs_stats.dir_count = 0;
        mutex_init(&ramfs_stats.global_lock);
        ramfs_stats_initialized = 1;
    }
}

uint64_t ramfs_get_time(void) {
    return tick_count;
}

static inline void ramfs_lock(void) {
    if (!ramfs_stats_initialized) {
        ramfs_init_stats();
    }
    mutex_lock(&ramfs_stats.global_lock);
}

static inline void ramfs_unlock(void) {
    mutex_unlock(&ramfs_stats.global_lock);
}

static inline void ramfs_node_lock(ramfs_node_t *node) {
    if (node) mutex_lock(&node->lock);
}

static inline void ramfs_node_unlock(ramfs_node_t *node) {
    if (node) mutex_unlock(&node->lock);
}

static char *ramfs_strdup_name(const char *name) {
    size_t len;
    char *copy;

    if (!name) return NULL;
    len = strlen(name);
    if (len >= VFS_MAX_NAME) return NULL;
    copy = (char *)kmalloc((uint64_t)len + 1);
    if (!copy) return NULL;
    memcpy(copy, name, len + 1);
    return copy;
}

static int ramfs_set_node_name(ramfs_node_t *node, const char *name) {
    char *copy;

    if (!node || !name) return RAMFS_ERR_INVAL;
    copy = ramfs_strdup_name(name);
    if (!copy) return RAMFS_ERR_NOMEM;
    if (node->name) kfree(node->name);
    node->name = copy;
    return RAMFS_ERR_OK;
}

static void ramfs_free_node_name(ramfs_node_t *node) {
    if (node && node->name) {
        kfree(node->name);
        node->name = NULL;
    }
}

static ramfs_node_t *ramfs_alloc_node(void) {
    ramfs_node_t *node;
    uint64_t now;

    node = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(ramfs_node_t));
    mutex_init(&node->lock);
    
    now = ramfs_get_time();
    node->atime = now;
    node->mtime = now;
    node->ctime = now;
    
    return node;
}

static vfs_node_t *ramfs_get_vfs_node(ramfs_node_t *rn, vfs_node_t *parent_vn) {
    vfs_node_t *vn;

    if (!rn) return NULL;
    if (rn->vfs_node) {
        rn->vfs_node->length = rn->backing_data ? rn->backing_length : rn->length;
        rn->vfs_node->atime = rn->atime;
        rn->vfs_node->mtime = rn->mtime;
        rn->vfs_node->ctime = rn->ctime;
        if (parent_vn) rn->vfs_node->parent = parent_vn;
        return rn->vfs_node;
    }

    vn = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(vfs_node_t));

    strncpy(vn->name, rn->name, VFS_MAX_NAME - 1);
    vn->name[VFS_MAX_NAME - 1] = '\0';
    if (rn->type == RAMFS_NODE_DIR) {
        vn->flags = VFS_DIRECTORY;
    } else if (rn->type == RAMFS_NODE_SYMLINK) {
        vn->flags = VFS_SYMLINK;
    } else {
        vn->flags = VFS_FILE;
    }
    vn->inode = ramfs_next_inode++;
    vn->length = rn->backing_data ? rn->backing_length : rn->length;
    vn->mask = rn->permissions;
    vn->uid = rn->uid;
    vn->gid = rn->gid;
    vn->atime = rn->atime;
    vn->mtime = rn->mtime;
    vn->ctime = rn->ctime;
    if (parent_vn) {
        vn->parent = parent_vn;
    } else if (rn->parent) {
        vn->parent = rn->parent->vfs_node;
    }
    vn->private_data = rn;

    if (rn->type == RAMFS_NODE_DIR) {
        ramfs_setup_vfs_dir_callbacks(vn);
    } else if (rn->type == RAMFS_NODE_SYMLINK) {
        vn->read = ramfs_vfs_read;
        vn->open = ramfs_vfs_open;
        vn->close = ramfs_vfs_close;
        vn->chmod = ramfs_vfs_chmod;
        vn->chown = ramfs_vfs_chown;
    } else {
        ramfs_setup_vfs_file_callbacks(vn);
    }

    rn->vfs_node = vn;
    return vn;
}

static void ramfs_free_node_data(ramfs_node_t *node) {
    if (node && node->data) {
        ramfs_lock();
        ramfs_stats.used_size -= node->length;
        ramfs_unlock();
        
        kfree(node->data);
        node->data = NULL;
        node->data_capacity = 0;
        node->length = 0;
    }
}

void ramfs_init(void) {
    ramfs_init_stats();
    
    ramfs_root = ramfs_alloc_node();
    if (!ramfs_root) {
        return;
    }
    
    if (ramfs_set_node_name(ramfs_root, "/") != RAMFS_ERR_OK) {
        kfree(ramfs_root);
        ramfs_root = NULL;
        return;
    }
    ramfs_root->type = 1;
    ramfs_root->permissions = 0755;
    ramfs_root->uid = 0;
    ramfs_root->gid = 0;
    ramfs_root->parent = NULL;
    ramfs_root->children = NULL;
    ramfs_root->next_sibling = NULL;
    
    ramfs_stats.dir_count = 1;
}

static ramfs_node_t *ramfs_find_child(ramfs_node_t *parent, const char *name) {
    if (!parent || !name) return NULL;
    ramfs_node_t *child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return child;
        child = child->next_sibling;
    }
    return NULL;
}

ramfs_node_t *ramfs_find_node(const char *path) {
    if (!path || !ramfs_root) return NULL;
    
    while (*path == '/') path++;
    if (*path == '\0') return ramfs_root;
    
    ramfs_node_t *current = ramfs_root;
    char component[VFS_MAX_NAME];
    
    while (*path) {
        while (*path == '/') path++;
        if (*path == '\0') break;
        
        int len = 0;
        while (path[len] && path[len] != '/' && len < (int)sizeof(component) - 1) {
            component[len] = path[len];
            len++;
        }
        component[len] = '\0';
        path += len;
        
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (current->parent) current = current->parent;
            continue;
        }
        
        ramfs_node_t *child = ramfs_find_child(current, component);
        if (!child) return NULL;
        current = child;
    }
    
    return current;
}

static ramfs_node_t *ramfs_find_parent(const char *path, char *out_name, size_t name_size) {
    if (!path || !out_name || name_size == 0) return NULL;
    
    while (*path == '/') path++;
    if (*path == '\0') return NULL;
    
    char temp_path[VFS_MAX_PATH];
    strncpy(temp_path, path, VFS_MAX_PATH - 1);
    temp_path[VFS_MAX_PATH - 1] = '\0';
    
    char *last_slash = NULL;
    for (char *p = temp_path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    
    if (!last_slash) {
        strncpy(out_name, temp_path, name_size - 1);
        out_name[name_size - 1] = '\0';
        return ramfs_root;
    }
    
    *last_slash = '\0';
    strncpy(out_name, last_slash + 1, name_size - 1);
    out_name[name_size - 1] = '\0';
    
    if (temp_path[0] == '\0') return ramfs_root;
    
    return ramfs_find_node(temp_path);
}

static int ramfs_check_space(uint64_t size) {
    return (ramfs_stats.used_size + size <= ramfs_stats.total_size);
}

int ramfs_get_stats(ramfs_stats_t *stats) {
    if (!stats) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    stats->total_size = ramfs_stats.total_size;
    stats->used_size = ramfs_stats.used_size;
    stats->file_count = ramfs_stats.file_count;
    stats->dir_count = ramfs_stats.dir_count;
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_create_file(const char *path, uint16_t permissions) {
    if (!path) return RAMFS_ERR_INVAL;
    
    char name[VFS_MAX_NAME];
    
    ramfs_lock();
    ramfs_node_t *parent = ramfs_find_parent(path, name, sizeof(name));
    if (!parent || parent->type != RAMFS_NODE_DIR) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(parent);
    
    if (ramfs_find_child(parent, name)) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_EXIST;
    }
    
    ramfs_node_t *node = ramfs_alloc_node();
    if (!node) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    
    if (ramfs_set_node_name(node, name) != RAMFS_ERR_OK) {
        kfree(node);
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    node->type = RAMFS_NODE_FILE;
    node->permissions = permissions ? permissions : 0644;
    node->uid = current_task ? current_task->euid : 0;
    node->gid = current_task ? current_task->egid : 0;
    node->parent = parent;
    node->data = NULL;
    node->data_capacity = 0;
    node->length = 0;
    
    node->next_sibling = parent->children;
    parent->children = node;
    parent->mtime = ramfs_get_time();
    
    ramfs_stats.file_count++;
    
    ramfs_node_unlock(parent);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_create_symlink(const char *path, const char *target, uint16_t permissions) {
    if (!path || !target) return RAMFS_ERR_INVAL;

    char name[VFS_MAX_NAME];

    ramfs_lock();
    ramfs_node_t *parent = ramfs_find_parent(path, name, sizeof(name));
    if (!parent || parent->type != RAMFS_NODE_DIR) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }

    ramfs_node_lock(parent);

    if (ramfs_find_child(parent, name)) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_EXIST;
    }

    size_t tlen = strlen(target);

    if (!ramfs_check_space((uint64_t)tlen)) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOSPC;
    }

    ramfs_node_t *node = ramfs_alloc_node();
    if (!node) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }

    if (ramfs_set_node_name(node, name) != RAMFS_ERR_OK) {
        kfree(node);
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    node->type = RAMFS_NODE_SYMLINK;
    node->permissions = permissions ? permissions : 0777;
    node->uid = current_task ? current_task->euid : 0;
    node->gid = current_task ? current_task->egid : 0;
    node->parent = parent;
    node->children = NULL;

    node->data = (uint8_t *)kmalloc((uint64_t)tlen + 1);
    if (!node->data) {
        ramfs_free_node_name(node);
        kfree(node);
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    memcpy(node->data, target, tlen);
    node->data[tlen] = '\0';
    node->data_capacity = (uint64_t)tlen + 1;
    node->length = (uint64_t)tlen;

    node->next_sibling = parent->children;
    parent->children = node;
    parent->mtime = ramfs_get_time();

    ramfs_stats.used_size += node->length;
    ramfs_stats.file_count++;

    ramfs_node_unlock(parent);
    ramfs_unlock();

    return RAMFS_ERR_OK;
}

int ramfs_create_dir(const char *path, uint16_t permissions) {
    if (!path) return RAMFS_ERR_INVAL;
    
    char name[VFS_MAX_NAME];
    
    ramfs_lock();
    ramfs_node_t *parent = ramfs_find_parent(path, name, sizeof(name));
    if (!parent || parent->type != RAMFS_NODE_DIR) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(parent);
    
    if (ramfs_find_child(parent, name)) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_EXIST;
    }
    
    ramfs_node_t *node = ramfs_alloc_node();
    if (!node) {
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    
    if (ramfs_set_node_name(node, name) != RAMFS_ERR_OK) {
        kfree(node);
        ramfs_node_unlock(parent);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    node->type = RAMFS_NODE_DIR;
    node->permissions = permissions ? permissions : 0755;
    node->uid = current_task ? current_task->euid : 0;
    node->gid = current_task ? current_task->egid : 0;
    node->parent = parent;
    node->children = NULL;
    
    node->next_sibling = parent->children;
    parent->children = node;
    parent->mtime = ramfs_get_time();
    
    ramfs_stats.dir_count++;
    
    ramfs_node_unlock(parent);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_unlink(const char *path) {
    if (!path) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node || node == ramfs_root) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    if (node->type == RAMFS_NODE_DIR && node->children) {
        ramfs_node_unlock(node);
        ramfs_unlock();
        return RAMFS_ERR_NOTEMPTY;
    }
    
    ramfs_node_t *parent = node->parent;
    if (!parent) {
        ramfs_node_unlock(node);
        ramfs_unlock();
        return RAMFS_ERR_INVAL;
    }
    
    ramfs_node_lock(parent);
    
    ramfs_node_t **pp = &parent->children;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next_sibling;
            break;
        }
        pp = &(*pp)->next_sibling;
    }
    parent->mtime = ramfs_get_time();
    
    if (node->type != RAMFS_NODE_DIR) {
        ramfs_stats.file_count--;
    } else {
        ramfs_stats.dir_count--;
    }
    
    ramfs_node_unlock(parent);
    ramfs_node_unlock(node);
    
    ramfs_free_node_data(node);
    ramfs_free_node_name(node);
    kfree(node);
    
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_truncate(const char *path, uint64_t length) {
    uint64_t extra;
    uint64_t new_cap;
    uint8_t *new_data;

    if (!path) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    if (node->type != 0) {
        ramfs_unlock();
        return RAMFS_ERR_ISDIR;
    }
    
    ramfs_node_lock(node);
    
    if (length == 0) {
        if (node->data) {
            ramfs_stats.used_size -= node->length;
            kfree(node->data);
            node->data = NULL;
            node->data_capacity = 0;
            node->length = 0;
        }
    } else if (length < node->length) {
        ramfs_stats.used_size -= (node->length - length);
        node->length = length;
    } else if (length > node->length) {
        extra = length - node->length;
        if (!ramfs_check_space(extra)) {
            ramfs_node_unlock(node);
            ramfs_unlock();
            return RAMFS_ERR_NOSPC;
        }
        
        if (length > node->data_capacity) {
            new_cap = (length + RAMFS_BLOCK_SIZE - 1) & ~((uint64_t)(RAMFS_BLOCK_SIZE - 1));
            
            new_data = (uint8_t *)kmalloc(new_cap);
            if (!new_data) {
                ramfs_node_unlock(node);
                ramfs_unlock();
                return RAMFS_ERR_NOMEM;
            }
            
            memset(new_data, 0, new_cap);
            if (node->data && node->length > 0) {
                memcpy(new_data, node->data, node->length);
                kfree(node->data);
            }
            node->data = new_data;
            node->data_capacity = new_cap;
        } else {
            memset(node->data + node->length, 0, length - node->length);
        }
        
        ramfs_stats.used_size += extra;
        node->length = length;
    }
    
    node->mtime = ramfs_get_time();
    node->ctime = node->mtime;
    
    if (node->vfs_node) {
        node->vfs_node->length = node->length;
        node->vfs_node->mtime = node->mtime;
        node->vfs_node->ctime = node->ctime;
    }
    
    ramfs_node_unlock(node);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_rename(const char *old_path, const char *new_path) {
    char old_name[VFS_MAX_NAME];
    char new_name[VFS_MAX_NAME];
    ramfs_node_t *node;
    ramfs_node_t *old_parent;
    ramfs_node_t *new_parent;
    ramfs_node_t *existing;
    ramfs_node_t **pp;
    char *new_name_copy;
    uint64_t now;

    if (!old_path || !new_path) return RAMFS_ERR_INVAL;

    ramfs_lock();

    node = ramfs_find_node(old_path);
    if (!node || node == ramfs_root) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }

    old_parent = ramfs_find_parent(old_path, old_name, sizeof(old_name));
    new_parent = ramfs_find_parent(new_path, new_name, sizeof(new_name));

    if (!old_parent || !new_parent) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    if (new_parent->type != 1) {
        ramfs_unlock();
        return RAMFS_ERR_NOTDIR;
    }

    new_name_copy = ramfs_strdup_name(new_name);
    if (!new_name_copy) {
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }

    existing = ramfs_find_child(new_parent, new_name);
    if (existing) {
        if (existing->type == 1 && existing->children) {
            kfree(new_name_copy);
            ramfs_unlock();
            return RAMFS_ERR_NOTEMPTY;
        }

        pp = &new_parent->children;
        while (*pp) {
            if (*pp == existing) {
                *pp = existing->next_sibling;
                break;
            }
            pp = &(*pp)->next_sibling;
        }

        if (existing->type == 0) {
            ramfs_stats.file_count--;
            ramfs_stats.used_size -= existing->length;
        } else {
            ramfs_stats.dir_count--;
        }

        if (existing->vfs_node) kfree(existing->vfs_node);
        ramfs_free_node_data(existing);
        ramfs_free_node_name(existing);
        kfree(existing);
    }

    pp = &old_parent->children;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next_sibling;
            break;
        }
        pp = &(*pp)->next_sibling;
    }

    if (node->name) kfree(node->name);
    node->name = new_name_copy;
    node->parent = new_parent;
    node->next_sibling = new_parent->children;
    new_parent->children = node;

    now = ramfs_get_time();
    old_parent->mtime = now;
    new_parent->mtime = now;
    node->ctime = now;

    if (node->vfs_node) {
        strncpy(node->vfs_node->name, new_name, VFS_MAX_NAME - 1);
        node->vfs_node->name[VFS_MAX_NAME - 1] = '\0';
        node->vfs_node->parent = new_parent->vfs_node;
        node->vfs_node->ctime = now;
    }

    ramfs_unlock();

    return RAMFS_ERR_OK;
}

int ramfs_chmod(const char *path, uint64_t mode) {
    if (!path) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    node->permissions = (uint16_t)(mode & 0x1FF);
    node->ctime = ramfs_get_time();
    
    if (node->vfs_node) {
        node->vfs_node->mask = mode;
        node->vfs_node->ctime = node->ctime;
    }
    
    ramfs_node_unlock(node);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_chown(const char *path, uint64_t uid, uint64_t gid) {
    if (!path) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    node->uid = uid;
    node->gid = gid;
    node->ctime = ramfs_get_time();
    
    if (node->vfs_node) {
        node->vfs_node->uid = uid;
        node->vfs_node->gid = gid;
        node->vfs_node->ctime = node->ctime;
    }
    
    ramfs_node_unlock(node);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_set_backing(const char *path, const uint8_t *data, uint64_t length) {
    ramfs_node_t *node;
    
    if (!path) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    node = ramfs_find_node(path);
    if (!node || node->type != RAMFS_NODE_FILE) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    node->backing_data = data;
    node->backing_length = length;
    node->length = length;
    
    if (node->vfs_node) {
        node->vfs_node->length = length;
    }
    
    ramfs_node_unlock(node);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

int ramfs_write(const char *path, uint64_t offset, const uint8_t *data, uint64_t size) {
    uint64_t needed;
    uint64_t old_len;
    uint64_t new_len;
    uint64_t extra;
    uint64_t new_cap;
    uint8_t *new_data;

    if (!path || !data || size == 0) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node || node->type != RAMFS_NODE_FILE) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    needed = offset + size;
    
    old_len = node->length;
    new_len = (needed > old_len) ? needed : old_len;
    extra = (new_len > old_len) ? (new_len - old_len) : 0;
    
    if (extra > 0 && !ramfs_check_space(extra)) {
        ramfs_node_unlock(node);
        ramfs_unlock();
        return RAMFS_ERR_NOSPC;
    }
    
    if (needed > node->data_capacity) {
        new_cap = (needed + RAMFS_BLOCK_SIZE - 1) & ~((uint64_t)(RAMFS_BLOCK_SIZE - 1));
        
        new_data = (uint8_t *)kmalloc(new_cap);
        if (!new_data) {
            ramfs_node_unlock(node);
            ramfs_unlock();
            return RAMFS_ERR_NOMEM;
        }
        
        memset(new_data, 0, new_cap);
        if (node->data && node->length > 0) {
            memcpy(new_data, node->data, node->length);
            kfree(node->data);
        }
        node->data = new_data;
        node->data_capacity = new_cap;
    }
    
    memcpy(node->data + offset, data, size);
    if (offset + size > node->length) {
        ramfs_stats.used_size += (offset + size - node->length);
        node->length = offset + size;
    }
    
    node->mtime = ramfs_get_time();
    
    if (node->vfs_node) {
        node->vfs_node->length = node->length;
        node->vfs_node->mtime = node->mtime;
    }
    
    ramfs_node_unlock(node);
    ramfs_unlock();
    
    return (int)size;
}

int ramfs_read(const char *path, uint64_t offset, uint8_t *buffer, uint64_t size) {
    uint64_t avail;
    uint64_t to_read;

    if (!path || !buffer || size == 0) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node || (node->type != RAMFS_NODE_FILE && node->type != RAMFS_NODE_SYMLINK)) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    if (offset >= node->length) {
        ramfs_node_unlock(node);
        ramfs_unlock();
        return 0;
    }
    
    avail = node->length - offset;
    to_read = (size < avail) ? size : avail;
    
    if (node->data) {
        memcpy(buffer, node->data + offset, to_read);
    } else {
        memset(buffer, 0, to_read);
    }
    
    node->atime = ramfs_get_time();
    if (node->vfs_node) {
        node->vfs_node->atime = node->atime;
    }
    
    ramfs_node_unlock(node);
    ramfs_unlock();
    
    return (int)to_read;
}

int ramfs_stat(const char *path, uint64_t *size, uint8_t *type, uint8_t *perms) {
    ramfs_lock();
    ramfs_node_t *node = ramfs_find_node(path);
    if (!node) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    if (size) *size = node->length;
    if (type) *type = node->type;
    if (perms) *perms = node->permissions;
    
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

ramfs_node_t *ramfs_get_root(void) {
    return ramfs_root;
}

static uint64_t ramfs_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ramfs_node_t *rn;
    uint64_t actual_len;
    uint64_t avail;
    uint64_t to_read;
    
    if (!node || !buffer) return 0;
    
    rn = (ramfs_node_t *)node->private_data;
    if (!rn || (rn->type != RAMFS_NODE_FILE && rn->type != RAMFS_NODE_SYMLINK)) return 0;
    
    ramfs_lock();
    ramfs_node_lock(rn);
    
    if (rn->data) {
        actual_len = rn->length;
    } else if (rn->backing_data) {
        actual_len = rn->backing_length;
    } else {
        actual_len = rn->length;
    }
    
    if (offset >= actual_len) {
        ramfs_node_unlock(rn);
        ramfs_unlock();
        return 0;
    }
    
    avail = actual_len - offset;
    to_read = (size < avail) ? size : avail;
    
    if (rn->data) {
        memcpy(buffer, rn->data + offset, to_read);
    } else if (rn->backing_data) {
        memcpy(buffer, rn->backing_data + offset, to_read);
    } else {
        memset(buffer, 0, to_read);
    }
    
    rn->atime = ramfs_get_time();
    node->atime = rn->atime;
    
    ramfs_node_unlock(rn);
    ramfs_unlock();
    
    return to_read;
}

static uint64_t ramfs_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ramfs_node_t *rn;
    uint64_t needed;
    uint64_t old_len;
    uint64_t new_len;
    uint64_t extra;
    uint64_t new_cap;
    uint8_t *new_data;
    uint64_t backing_to_copy;
    
    if (!node || !buffer || size == 0) return 0;
    
    rn = (ramfs_node_t *)node->private_data;
    if (!rn || rn->type != RAMFS_NODE_FILE) return 0;
    
    ramfs_lock();
    ramfs_node_lock(rn);
    
    if (!rn->data && rn->backing_data && rn->backing_length > 0) {
        backing_to_copy = rn->backing_length;
        if (!ramfs_check_space(backing_to_copy)) {
            ramfs_node_unlock(rn);
            ramfs_unlock();
            return 0;
        }
        new_cap = (backing_to_copy + RAMFS_BLOCK_SIZE - 1) & ~(RAMFS_BLOCK_SIZE - 1);
        new_data = (uint8_t *)kmalloc(new_cap);
        if (!new_data) {
            ramfs_node_unlock(rn);
            ramfs_unlock();
            return 0;
        }
        memcpy(new_data, rn->backing_data, backing_to_copy);
        rn->data = new_data;
        rn->data_capacity = new_cap;
        rn->length = backing_to_copy;
        ramfs_stats.used_size += backing_to_copy;
        rn->backing_data = NULL;
        rn->backing_length = 0;
    }
    
    needed = offset + size;
    
    old_len = rn->length;
    new_len = (needed > old_len) ? needed : old_len;
    extra = (new_len > old_len) ? (new_len - old_len) : 0;
    
    if (extra > 0 && !ramfs_check_space(extra)) {
        ramfs_node_unlock(rn);
        ramfs_unlock();
        return 0;
    }
    
    if (needed > rn->data_capacity) {
        new_cap = (needed + RAMFS_BLOCK_SIZE - 1) & ~((uint64_t)(RAMFS_BLOCK_SIZE - 1));
        
        new_data = (uint8_t *)kmalloc(new_cap);
        if (!new_data) {
            ramfs_node_unlock(rn);
            ramfs_unlock();
            return 0;
        }
        
        memset(new_data, 0, new_cap);
        if (rn->data && rn->length > 0) {
            memcpy(new_data, rn->data, rn->length);
            kfree(rn->data);
        }
        rn->data = new_data;
        rn->data_capacity = new_cap;
    }
    
    memcpy(rn->data + offset, buffer, size);
    if (offset + size > rn->length) {
        ramfs_stats.used_size += (offset + size - rn->length);
        rn->length = offset + size;
        node->length = rn->length;
    }
    
    rn->mtime = ramfs_get_time();
    node->mtime = rn->mtime;
    
    ramfs_node_unlock(rn);
    ramfs_unlock();
    
    return size;
}

static void ramfs_vfs_open(vfs_node_t *node, uint64_t flags) {
    if (!node) return;
    
    ramfs_node_t *rn = (ramfs_node_t *)node->private_data;
    if (!rn) return;
    
    if ((flags & VFS_O_TRUNC) && rn->type == RAMFS_NODE_FILE) {
        ramfs_lock();
        ramfs_node_lock(rn);
        
        if (rn->data) {
            ramfs_stats.used_size -= rn->length;
            kfree(rn->data);
            rn->data = NULL;
            rn->data_capacity = 0;
            rn->length = 0;
            node->length = 0;
        }
        
        rn->mtime = ramfs_get_time();
        node->mtime = rn->mtime;
        
        ramfs_node_unlock(rn);
        ramfs_unlock();
    }
}

static void ramfs_vfs_close(vfs_node_t *node) {
    (void)node;
}

static int ramfs_vfs_truncate(vfs_node_t *node, uint64_t length) {
    ramfs_node_t *rn;
    uint64_t extra;
    uint64_t new_cap;
    uint8_t *new_data;

    if (!node) return RAMFS_ERR_INVAL;
    
    rn = (ramfs_node_t *)node->private_data;
    if (!rn || rn->type != RAMFS_NODE_FILE) return RAMFS_ERR_ISDIR;
    
    ramfs_lock();
    ramfs_node_lock(rn);
    
    if (length == 0) {
        if (rn->data) {
            ramfs_stats.used_size -= rn->length;
            kfree(rn->data);
            rn->data = NULL;
            rn->data_capacity = 0;
            rn->length = 0;
        }
    } else if (length < rn->length) {
        ramfs_stats.used_size -= (rn->length - length);
        rn->length = length;
    } else if (length > rn->length) {
        extra = length - rn->length;
        if (!ramfs_check_space(extra)) {
            ramfs_node_unlock(rn);
            ramfs_unlock();
            return RAMFS_ERR_NOSPC;
        }
        
        if (length > rn->data_capacity) {
            new_cap = (length + RAMFS_BLOCK_SIZE - 1) & ~((uint64_t)(RAMFS_BLOCK_SIZE - 1));
            
            new_data = (uint8_t *)kmalloc(new_cap);
            if (!new_data) {
                ramfs_node_unlock(rn);
                ramfs_unlock();
                return RAMFS_ERR_NOMEM;
            }
            
            memset(new_data, 0, new_cap);
            if (rn->data && rn->length > 0) {
                memcpy(new_data, rn->data, rn->length);
                kfree(rn->data);
            }
            rn->data = new_data;
            rn->data_capacity = new_cap;
        } else {
            memset(rn->data + rn->length, 0, length - rn->length);
        }
        
        ramfs_stats.used_size += extra;
        rn->length = length;
    }
    
    rn->mtime = ramfs_get_time();
    rn->ctime = rn->mtime;
    node->length = rn->length;
    node->mtime = rn->mtime;
    node->ctime = rn->ctime;
    
    ramfs_node_unlock(rn);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

static int ramfs_vfs_chmod(vfs_node_t *node, uint64_t mode) {
    if (!node) return RAMFS_ERR_INVAL;
    
    ramfs_node_t *rn = (ramfs_node_t *)node->private_data;
    if (!rn) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_lock(rn);
    
    rn->permissions = (uint8_t)(mode & 0xFF);
    rn->ctime = ramfs_get_time();
    node->mask = mode;
    node->ctime = rn->ctime;
    
    ramfs_node_unlock(rn);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

static int ramfs_vfs_chown(vfs_node_t *node, uint64_t uid, uint64_t gid) {
    if (!node) return RAMFS_ERR_INVAL;
    
    ramfs_node_t *rn = (ramfs_node_t *)node->private_data;
    if (!rn) return RAMFS_ERR_INVAL;
    
    ramfs_lock();
    ramfs_node_lock(rn);
    
    rn->uid = uid;
    rn->gid = gid;
    rn->ctime = ramfs_get_time();
    node->uid = uid;
    node->gid = gid;
    node->ctime = rn->ctime;
    
    ramfs_node_unlock(rn);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

static dirent_t *ramfs_vfs_readdir(vfs_node_t *node, uint64_t index) {
    uint64_t adjusted_index;
    ramfs_node_t *rn;
    ramfs_node_t *child;
    uint64_t count;
    
    if (!node || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    
    rn = (ramfs_node_t *)node->private_data;
    if (!rn || rn->type != 1) return NULL;
    
    adjusted_index = index;

    ramfs_lock();
    ramfs_node_lock(rn);

    child = rn->children;
    count = 0;

    while (child) {
        if (count == adjusted_index) {
            strncpy(ramfs_dirent.name, child->name, VFS_MAX_NAME - 1);
            ramfs_dirent.name[VFS_MAX_NAME - 1] = '\0';
            ramfs_dirent.inode = (uint64_t)(uintptr_t)child;
            if (child->type == RAMFS_NODE_DIR) {
                ramfs_dirent.type = VFS_DIRECTORY;
            } else if (child->type == RAMFS_NODE_SYMLINK) {
                ramfs_dirent.type = VFS_SYMLINK;
            } else {
                ramfs_dirent.type = VFS_FILE;
            }

            ramfs_node_unlock(rn);
            ramfs_unlock();
            return &ramfs_dirent;
        }
        count++;
        child = child->next_sibling;
    }

    ramfs_node_unlock(rn);
    ramfs_unlock();

    return NULL;
}

static vfs_node_t *ramfs_vfs_finddir(vfs_node_t *node, const char *name) {
    ramfs_node_t *rn;
    ramfs_node_t *child;
    vfs_node_t *result;

    if (!node || !name || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        return NULL;
    }
    
    rn = (ramfs_node_t *)node->private_data;
    if (!rn || rn->type != 1) {
        return NULL;
    }
    
    ramfs_lock();
    ramfs_node_lock(rn);
    
    child = rn->children;
    while (child) {
        if (strcmp(child->name, name) == 0) {
            result = ramfs_get_vfs_node(child, node);
            if (result) {
                result->length = child->length;
                result->atime = child->atime;
                result->mtime = child->mtime;
                result->ctime = child->ctime;
            }
            ramfs_node_unlock(rn);
            ramfs_unlock();
            return result;
        }
        child = child->next_sibling;
    }
    
    ramfs_node_unlock(rn);
    ramfs_unlock();
    
    return NULL;
}

static void ramfs_setup_vfs_file_callbacks(vfs_node_t *vn) {
    vn->read = ramfs_vfs_read;
    vn->write = ramfs_vfs_write;
    vn->open = ramfs_vfs_open;
    vn->close = ramfs_vfs_close;
    vn->truncate = ramfs_vfs_truncate;
    vn->chmod = ramfs_vfs_chmod;
    vn->chown = ramfs_vfs_chown;
}

static void ramfs_setup_vfs_dir_callbacks(vfs_node_t *vn);

static int ramfs_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags) {
    if (!parent || !name || VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return RAMFS_ERR_INVAL;
    
    ramfs_node_t *prn = (ramfs_node_t *)parent->private_data;
    if (!prn || prn->type != RAMFS_NODE_DIR) return RAMFS_ERR_NOTDIR;
    
    ramfs_lock();
    ramfs_node_lock(prn);
    
    ramfs_node_t *existing = ramfs_find_child(prn, name);
    
    if (existing && (flags & VFS_O_EXCL)) {
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_EXIST;
    }
    
    if (existing) {
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_OK;
    }
    
    ramfs_node_t *node = ramfs_alloc_node();
    if (!node) {
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    
    if (ramfs_set_node_name(node, name) != RAMFS_ERR_OK) {
        kfree(node);
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    node->type = RAMFS_NODE_FILE;
    node->permissions = (uint16_t)(flags & 0x1FF);
    if (node->permissions == 0) node->permissions = 0644;
    node->uid = current_task ? current_task->euid : 0;
    node->gid = current_task ? current_task->egid : 0;
    node->parent = prn;
    node->data = NULL;
    node->data_capacity = 0;
    node->length = 0;
    
    node->next_sibling = prn->children;
    prn->children = node;
    prn->mtime = ramfs_get_time();
    
    ramfs_stats.file_count++;
    
    ramfs_node_unlock(prn);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

static int ramfs_vfs_unlink(vfs_node_t *parent, const char *name) {
    if (!parent || !name || VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return RAMFS_ERR_INVAL;
    
    ramfs_node_t *prn = (ramfs_node_t *)parent->private_data;
    if (!prn || prn->type != 1) return RAMFS_ERR_NOTDIR;
    
    ramfs_lock();
    ramfs_node_lock(prn);
    
    ramfs_node_t *node = ramfs_find_child(prn, name);
    if (!node) {
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }
    
    ramfs_node_lock(node);
    
    if (node->type == 1 && node->children) {
        ramfs_node_unlock(node);
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_NOTEMPTY;
    }
    
    if (node->vfs_node && node->vfs_node->ref_count > 0) {
        ramfs_node_unlock(node);
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_BUSY;
    }
    
    ramfs_node_t **pp = &prn->children;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next_sibling;
            break;
        }
        pp = &(*pp)->next_sibling;
    }
    prn->mtime = ramfs_get_time();
    
    if (node->type == 0) {
        ramfs_stats.file_count--;
        if (node->length > 0) {
            ramfs_stats.used_size -= node->length;
        }
    } else {
        ramfs_stats.dir_count--;
    }
    
    ramfs_node_unlock(node);
    ramfs_node_unlock(prn);
    
    if (node->vfs_node) {
        kfree(node->vfs_node);
    }
    if (node->data) {
        kfree(node->data);
    }
    ramfs_free_node_name(node);
    kfree(node);
    
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

static int ramfs_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
    if (!parent || !name || VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) return RAMFS_ERR_INVAL;
    
    ramfs_node_t *prn = (ramfs_node_t *)parent->private_data;
    if (!prn || prn->type != 1) return RAMFS_ERR_NOTDIR;
    
    ramfs_lock();
    ramfs_node_lock(prn);
    
    if (ramfs_find_child(prn, name)) {
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_EXIST;
    }
    
    ramfs_node_t *node = ramfs_alloc_node();
    if (!node) {
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    
    if (ramfs_set_node_name(node, name) != RAMFS_ERR_OK) {
        kfree(node);
        ramfs_node_unlock(prn);
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }
    node->type = 1;
    node->permissions = (uint16_t)(perms & 0x1FF);
    if (node->permissions == 0) node->permissions = 0755;
    node->uid = current_task ? current_task->euid : 0;
    node->gid = current_task ? current_task->egid : 0;
    node->parent = prn;
    node->children = NULL;
    
    node->next_sibling = prn->children;
    prn->children = node;
    prn->mtime = ramfs_get_time();
    
    ramfs_stats.dir_count++;
    
    ramfs_node_unlock(prn);
    ramfs_unlock();
    
    return RAMFS_ERR_OK;
}

static int ramfs_vfs_rename(vfs_node_t *old_parent, const char *old_name, 
                            vfs_node_t *new_parent, const char *new_name) {
    ramfs_node_t *old_prn;
    ramfs_node_t *new_prn;
    ramfs_node_t *node;
    ramfs_node_t *existing;
    ramfs_node_t **pp;
    char *new_name_copy;
    uint64_t now;

    if (!old_parent || !old_name || !new_parent || !new_name) return RAMFS_ERR_INVAL;

    old_prn = (ramfs_node_t *)old_parent->private_data;
    new_prn = (ramfs_node_t *)new_parent->private_data;

    if (!old_prn || !new_prn || old_prn->type != 1 || new_prn->type != 1) {
        return RAMFS_ERR_NOTDIR;
    }

    ramfs_lock();

    node = ramfs_find_child(old_prn, old_name);
    if (!node) {
        ramfs_unlock();
        return RAMFS_ERR_NOENT;
    }

    new_name_copy = ramfs_strdup_name(new_name);
    if (!new_name_copy) {
        ramfs_unlock();
        return RAMFS_ERR_NOMEM;
    }

    existing = ramfs_find_child(new_prn, new_name);
    if (existing) {
        if (existing->type == 1 && existing->children) {
            kfree(new_name_copy);
            ramfs_unlock();
            return RAMFS_ERR_NOTEMPTY;
        }

        pp = &new_prn->children;
        while (*pp) {
            if (*pp == existing) {
                *pp = existing->next_sibling;
                break;
            }
            pp = &(*pp)->next_sibling;
        }

        if (existing->type == 0) {
            ramfs_stats.file_count--;
            ramfs_stats.used_size -= existing->length;
        } else {
            ramfs_stats.dir_count--;
        }

        if (existing->vfs_node) kfree(existing->vfs_node);
        if (existing->data) kfree(existing->data);
        ramfs_free_node_name(existing);
        kfree(existing);
    }

    pp = &old_prn->children;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next_sibling;
            break;
        }
        pp = &(*pp)->next_sibling;
    }

    if (node->name) kfree(node->name);
    node->name = new_name_copy;
    node->parent = new_prn;
    node->next_sibling = new_prn->children;
    new_prn->children = node;

    now = ramfs_get_time();
    old_prn->mtime = now;
    new_prn->mtime = now;
    node->ctime = now;

    if (node->vfs_node) {
        strncpy(node->vfs_node->name, new_name, VFS_MAX_NAME - 1);
        node->vfs_node->name[VFS_MAX_NAME - 1] = '\0';
        node->vfs_node->parent = new_parent;
        node->vfs_node->ctime = now;
    }

    ramfs_unlock();

    return RAMFS_ERR_OK;
}

static void ramfs_setup_vfs_dir_callbacks(vfs_node_t *vn) {
    vn->readdir = ramfs_vfs_readdir;
    vn->finddir = ramfs_vfs_finddir;
    vn->create = ramfs_vfs_create;
    vn->unlink = ramfs_vfs_unlink;
    vn->mkdir = ramfs_vfs_mkdir;
    vn->rename = ramfs_vfs_rename;
    vn->open = ramfs_vfs_open;
    vn->close = ramfs_vfs_close;
    vn->chmod = ramfs_vfs_chmod;
    vn->chown = ramfs_vfs_chown;
}

static vfs_node_t *ramfs_vfs_do_mount(const char *device, const char *mountpoint) {
    (void)device;
    (void)mountpoint;
    
    if (!ramfs_root) {
        ramfs_init();
    }
    
    if (!ramfs_root) {
        return NULL;
    }
    
    ramfs_vfs_root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!ramfs_vfs_root) {
        return NULL;
    }
    
    memset(ramfs_vfs_root, 0, sizeof(vfs_node_t));
    
    ramfs_vfs_root->name[0] = '/';
    ramfs_vfs_root->name[1] = '\0';
    ramfs_vfs_root->flags = VFS_DIRECTORY;
    ramfs_vfs_root->inode = ramfs_next_inode++;
    ramfs_vfs_root->length = 0;
    ramfs_vfs_root->mask = 0755;
    ramfs_vfs_root->uid = 0;
    ramfs_vfs_root->gid = 0;
    ramfs_vfs_root->atime = ramfs_root->atime;
    ramfs_vfs_root->mtime = ramfs_root->mtime;
    ramfs_vfs_root->ctime = ramfs_root->ctime;
    
    ramfs_setup_vfs_dir_callbacks(ramfs_vfs_root);
    ramfs_vfs_root->parent = NULL;
    ramfs_vfs_root->private_data = ramfs_root;
    
    ramfs_root->vfs_node = ramfs_vfs_root;
    
    return ramfs_vfs_root;
}

static vfs_fs_type_t ramfs_fs_type;

void ramfs_vfs_register(void) {
    ramfs_fs_type.name = "ramfs";
    ramfs_fs_type.mount = ramfs_vfs_do_mount;
    ramfs_fs_type.unmount = NULL;
    ramfs_fs_type.next = NULL;

    vfs_register_fs(&ramfs_fs_type);
}

static vfs_node_t *tmpfs_vfs_do_mount(const char *device, const char *mountpoint) {
    ramfs_node_t *rn;
    vfs_node_t *vn;

    (void)device;
    (void)mountpoint;

    rn = ramfs_alloc_node();
    if (!rn) return NULL;

    if (ramfs_set_node_name(rn, "tmpfs") != RAMFS_ERR_OK) {
        kfree(rn);
        return NULL;
    }
    rn->type = RAMFS_NODE_DIR;
    rn->permissions = 0777;
    rn->uid = 0;
    rn->gid = 0;
    rn->parent = NULL;
    rn->children = NULL;
    rn->next_sibling = NULL;

    vn = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!vn) {
        ramfs_free_node_name(rn);
        kfree(rn);
        return NULL;
    }
    memset(vn, 0, sizeof(vfs_node_t));

    strcpy(vn->name, "tmp");
    vn->flags = VFS_DIRECTORY;
    vn->inode = ramfs_next_inode++;
    vn->length = 0;
    vn->mask = 0777;
    vn->uid = 0;
    vn->gid = 0;
    vn->atime = rn->atime;
    vn->mtime = rn->mtime;
    vn->ctime = rn->ctime;

    ramfs_setup_vfs_dir_callbacks(vn);
    vn->parent = NULL;
    vn->private_data = rn;

    rn->vfs_node = vn;

    ramfs_lock();
    ramfs_stats.dir_count++;
    ramfs_unlock();

    return vn;
}

static vfs_fs_type_t tmpfs_fs_type;

void tmpfs_vfs_register(void) {
    tmpfs_fs_type.name = "tmpfs";
    tmpfs_fs_type.mount = tmpfs_vfs_do_mount;
    tmpfs_fs_type.unmount = NULL;
    tmpfs_fs_type.next = NULL;

    vfs_register_fs(&tmpfs_fs_type);
}

static void ramfs_internalize_node(ramfs_node_t *node) {
    uint8_t *copy;
    ramfs_node_t *child;

    if (!node) return;

    if (node->backing_data && node->backing_length > 0 && !node->data) {
        copy = (uint8_t *)kmalloc(node->backing_length);
        if (copy) {
            memcpy(copy, node->backing_data, node->backing_length);
            node->data = copy;
            node->data_capacity = node->backing_length;
            node->backing_data = NULL;
            node->backing_length = 0;
        }
    }

    child = node->children;
    while (child) {
        ramfs_internalize_node(child);
        child = child->next_sibling;
    }
}

void ramfs_internalize_all(void) {
    if (!ramfs_root) return;
    ramfs_lock();
    ramfs_internalize_node(ramfs_root);
    ramfs_unlock();
}

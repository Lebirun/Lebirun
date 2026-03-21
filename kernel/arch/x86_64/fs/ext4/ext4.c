#include <kernel/fs/ext4/ext4.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

extern int ext4_load_group_descs(ext4_fs_t *fs);
extern void ext4_flush_cache(ext4_fs_t *fs);
extern void ext4_mark_inode_dirty(ext4_inode_cache_t *ic);
extern int ext4_dir_is_empty(ext4_fs_t *fs, uint32_t dir_ino);
extern int ext4_create_file(ext4_fs_t *fs, uint32_t parent_ino, const char *name, uint16_t mode);
extern int ext4_unlink_file(ext4_fs_t *fs, uint32_t parent_ino, const char *name);
extern int ext4_rename_file(ext4_fs_t *fs, uint32_t old_parent_ino, const char *old_name,
                            uint32_t new_parent_ino, const char *new_name);

static ext4_fs_t *mounted_fs = NULL;
static vfs_fs_type_t ext4_fs_type;

typedef struct {
    ext4_fs_t *fs;
    uint32_t ino;
} ext4_vfs_private_t;

static dirent_t ext4_dirent;

static uint64_t ext4_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static uint64_t ext4_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static void ext4_vfs_open(vfs_node_t *node, uint64_t flags);
static void ext4_vfs_close(vfs_node_t *node);
static dirent_t *ext4_vfs_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *ext4_vfs_finddir(vfs_node_t *node, const char *name);
static int ext4_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags);
static int ext4_vfs_unlink(vfs_node_t *parent, const char *name);
static int ext4_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms);
static int ext4_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                           vfs_node_t *new_parent, const char *new_name);

static vfs_node_t *ext4_create_vfs_node(ext4_fs_t *fs, uint32_t ino, const char *name) {
    ext4_inode_cache_t *ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return NULL;
    }

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        ext4_release_inode(ic);
        return NULL;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)kmalloc(sizeof(ext4_vfs_private_t));
    if (!priv) {
        kfree(node);
        ext4_release_inode(ic);
        return NULL;
    }

    memset(node, 0, sizeof(vfs_node_t));
    priv->fs = fs;
    priv->ino = ino;

    size_t name_len = strlen(name);
    if (name_len >= VFS_MAX_NAME) {
        name_len = VFS_MAX_NAME - 1;
    }
    memcpy(node->name, name, name_len);
    node->name[name_len] = '\0';

    node->inode = ino;
    node->length = ext4_inode_get_size(&ic->inode);
    node->uid = ic->inode.i_uid;
    node->gid = ic->inode.i_gid;
    node->atime = ic->inode.i_atime;
    node->mtime = ic->inode.i_mtime;
    node->ctime = ic->inode.i_ctime;

    uint16_t mode = ic->inode.i_mode;
    node->mask = mode & 0777;

    if ((mode & EXT4_S_IFDIR) == EXT4_S_IFDIR) {
        node->flags = VFS_DIRECTORY;
    } else if ((mode & EXT4_S_IFREG) == EXT4_S_IFREG) {
        node->flags = VFS_FILE;
    } else if ((mode & EXT4_S_IFLNK) == EXT4_S_IFLNK) {
        node->flags = VFS_SYMLINK;
    } else if ((mode & EXT4_S_IFCHR) == EXT4_S_IFCHR) {
        node->flags = VFS_CHARDEVICE;
    } else if ((mode & EXT4_S_IFBLK) == EXT4_S_IFBLK) {
        node->flags = VFS_BLOCKDEVICE;
    } else {
        node->flags = VFS_FILE;
    }

    node->read = ext4_vfs_read;
    node->write = ext4_vfs_write;
    node->open = ext4_vfs_open;
    node->close = ext4_vfs_close;
    node->readdir = ext4_vfs_readdir;
    node->finddir = ext4_vfs_finddir;
    node->create = ext4_vfs_create;
    node->unlink = ext4_vfs_unlink;
    node->mkdir = ext4_vfs_mkdir;
    node->rename = ext4_vfs_rename;

    node->private_data = priv;
    node->ref_count = 0;

    ic->vfs_node = node;
    ext4_release_inode(ic);

    return node;
}

static uint64_t ext4_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !node->private_data || !buffer) {
        return 0;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)node->private_data;
    return ext4_file_read(priv->fs, priv->ino, offset, size, buffer);
}

static uint64_t ext4_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !node->private_data || !buffer) {
        return 0;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)node->private_data;
    uint32_t written = ext4_file_write(priv->fs, priv->ino, offset, size, buffer);

    if (written > 0) {
        ext4_inode_cache_t *ic = ext4_get_inode(priv->fs, priv->ino);
        if (ic) {
            node->length = ext4_inode_get_size(&ic->inode);
            ext4_release_inode(ic);
        }
    }

    return written;
}

static void ext4_vfs_open(vfs_node_t *node, uint64_t flags) {
    (void)node;
    (void)flags;
}

static void ext4_vfs_close(vfs_node_t *node) {
    if (!node || !node->private_data) {
        return;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)node->private_data;
    ext4_sync_blocks(priv->fs);
}

static dirent_t *ext4_vfs_readdir(vfs_node_t *node, uint64_t index) {
    if (!node || !node->private_data) {
        return NULL;
    }

    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        return NULL;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)node->private_data;
    
    ext4_dir_entry_t entry;
    if (ext4_dir_get_entry(priv->fs, priv->ino, index, &entry) != 0) {
        return NULL;
    }

    size_t name_len = entry.name_len;
    if (name_len >= VFS_MAX_NAME) {
        name_len = VFS_MAX_NAME - 1;
    }
    memcpy(ext4_dirent.name, entry.name, name_len);
    ext4_dirent.name[name_len] = '\0';
    ext4_dirent.inode = entry.inode;
    ext4_dirent.type = ext4_type_to_vfs(entry.file_type);

    return &ext4_dirent;
}

static vfs_node_t *ext4_vfs_finddir(vfs_node_t *node, const char *name) {
    if (!node || !node->private_data || !name) {
        return NULL;
    }

    if (VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) {
        return NULL;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)node->private_data;
    
    uint32_t ino;
    if (ext4_dir_lookup(priv->fs, priv->ino, name, &ino) != 0) {
        return NULL;
    }

    vfs_node_t *child = ext4_create_vfs_node(priv->fs, ino, name);
    if (child) {
        child->parent = node;
    }

    return child;
}

static int ext4_vfs_create(vfs_node_t *parent, const char *name, uint64_t flags) {
    if (!parent || !parent->private_data || !name) {
        return -1;
    }

    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) {
        return -1;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)parent->private_data;
    
    uint16_t mode = EXT4_S_IFREG | (flags & 0777);
    if ((flags & 0777) == 0) {
        mode |= 0644;
    }

    int ino = ext4_create_file(priv->fs, priv->ino, name, mode);
    if (ino < 0) {
        return -1;
    }

    return 0;
}

static int ext4_vfs_unlink(vfs_node_t *parent, const char *name) {
    if (!parent || !parent->private_data || !name) {
        return -1;
    }

    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) {
        return -1;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)parent->private_data;
    return ext4_unlink_file(priv->fs, priv->ino, name);
}

static int ext4_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                           vfs_node_t *new_parent, const char *new_name) {
    ext4_vfs_private_t *old_priv;
    ext4_vfs_private_t *new_priv;

    if (!old_parent || !old_parent->private_data || !old_name) {
        return -1;
    }
    if (!new_parent || !new_parent->private_data || !new_name) {
        return -1;
    }
    if (VFS_GET_TYPE(old_parent->flags) != VFS_DIRECTORY) {
        return -1;
    }
    if (VFS_GET_TYPE(new_parent->flags) != VFS_DIRECTORY) {
        return -1;
    }

    old_priv = (ext4_vfs_private_t *)old_parent->private_data;
    new_priv = (ext4_vfs_private_t *)new_parent->private_data;

    if (old_priv->fs != new_priv->fs) {
        return -1;
    }

    return ext4_rename_file(old_priv->fs, old_priv->ino, old_name,
                            new_priv->ino, new_name);
}

static int ext4_vfs_mkdir(vfs_node_t *parent, const char *name, uint64_t perms) {
    if (!parent || !parent->private_data || !name) {
        return -1;
    }

    if (VFS_GET_TYPE(parent->flags) != VFS_DIRECTORY) {
        return -1;
    }

    ext4_vfs_private_t *priv = (ext4_vfs_private_t *)parent->private_data;
    ext4_fs_t *fs = priv->fs;

    uint32_t existing;
    if (ext4_dir_lookup(fs, priv->ino, name, &existing) == 0) {
        return -1;
    }

    uint16_t mode = EXT4_S_IFDIR | (perms & 0777);
    if ((perms & 0777) == 0) {
        mode |= 0755;
    }

    int new_ino = ext4_alloc_inode(fs, mode);
    if (new_ino < 0) {
        return -1;
    }

    ext4_inode_cache_t *ic = ext4_get_inode(fs, new_ino);
    if (!ic) {
        ext4_free_inode(fs, new_ino);
        return -1;
    }

    int new_block = ext4_alloc_block(fs, 0);
    if (new_block < 0) {
        ext4_release_inode(ic);
        ext4_free_inode(fs, new_ino);
        return -1;
    }

    ic->inode.i_block[0] = new_block;
    ic->inode.i_blocks_lo = fs->sectors_per_block;
    ext4_inode_set_size(&ic->inode, fs->block_size);
    ic->inode.i_links_count = 2;

    uint8_t *dir_block = (uint8_t *)kmalloc(fs->block_size);
    if (!dir_block) {
        ext4_free_block(fs, new_block);
        ext4_release_inode(ic);
        ext4_free_inode(fs, new_ino);
        return -1;
    }

    memset(dir_block, 0, fs->block_size);

    ext4_dir_entry_t *dot = (ext4_dir_entry_t *)dir_block;
    dot->inode = new_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->name[0] = '.';

    ext4_dir_entry_t *dotdot = (ext4_dir_entry_t *)(dir_block + 12);
    dotdot->inode = priv->ino;
    dotdot->rec_len = fs->block_size - 12;
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    ext4_write_block(fs, new_block, dir_block);
    kfree(dir_block);

    ext4_mark_inode_dirty(ic);
    ext4_release_inode(ic);

    if (ext4_dir_add_entry(fs, priv->ino, name, new_ino, EXT4_FT_DIR) != 0) {
        ext4_free_block(fs, new_block);
        ext4_free_inode(fs, new_ino);
        return -1;
    }

    ext4_inode_cache_t *parent_ic = ext4_get_inode(fs, priv->ino);
    if (parent_ic) {
        parent_ic->inode.i_links_count++;
        ext4_mark_inode_dirty(parent_ic);
        ext4_release_inode(parent_ic);
    }

    return 0;
}

static vfs_node_t *ext4_do_mount(const char *device, const char *mountpoint) {
    vfs_node_t *dev_node;
    uint32_t port_idx;
    uint64_t part_start;
    ahci_port_t *port;
    ext4_fs_t *fs;
    size_t mp_len;
    vfs_node_t *root;

    extern uint64_t devfs_get_partition_start(vfs_node_t *node);

    port_idx = 0;
    part_start = 0;
    if (device && device[0] != '\0') {
        dev_node = vfs_namei(device);
        if (!dev_node) {
            printf("EXT4: Device not found: %s\n", device);
            return NULL;
        }
        if ((dev_node->flags & VFS_TYPE_MASK) != VFS_BLOCKDEVICE) {
            printf("EXT4: Not a block device: %s\n", device);
            return NULL;
        }
        port_idx = dev_node->inode;
        part_start = devfs_get_partition_start(dev_node);
    }

    port = ahci_get_port(port_idx);
    if (!port) {
        printf("EXT4: No AHCI port available\n");
        return NULL;
    }

    fs = (ext4_fs_t *)kmalloc(sizeof(ext4_fs_t));
    if (!fs) {
        printf("EXT4: Failed to allocate filesystem structure\n");
        return NULL;
    }

    memset(fs, 0, sizeof(ext4_fs_t));
    fs->port_index = port_idx;
    fs->partition_start_lba = part_start;
    mutex_init(&fs->lock);

    if (ext4_read_superblock(fs) != 0) {
        printf("EXT4: Failed to read superblock\n");
        kfree(fs);
        return NULL;
    }

    if (ext4_validate_superblock(&fs->sb) != 0) {
        printf("EXT4: Invalid superblock\n");
        kfree(fs);
        return NULL;
    }

    ext4_print_superblock(&fs->sb);

    if (ext4_load_group_descs(fs) != 0) {
        printf("EXT4: Failed to load group descriptors\n");
        kfree(fs);
        return NULL;
    }

    mp_len = strlen(mountpoint);
    if (mp_len >= VFS_MAX_PATH) {
        mp_len = VFS_MAX_PATH - 1;
    }
    memcpy(fs->mountpoint, mountpoint, mp_len);
    fs->mountpoint[mp_len] = '\0';

    root = ext4_create_vfs_node(fs, EXT4_ROOT_INO, "/");
    if (!root) {
        printf("EXT4: Failed to create root node\n");
        kfree(fs->group_descs);
        kfree(fs);
        return NULL;
    }

    fs->root_node = root;
    mounted_fs = fs;

    printf("EXT4: Mounted successfully on %s\n", mountpoint);
    return root;
}

static int ext4_do_unmount(vfs_node_t *mountpoint) {
    if (!mounted_fs) {
        return -1;
    }

    ext4_sync_inodes(mounted_fs);
    ext4_sync_blocks(mounted_fs);
    ext4_flush_cache(mounted_fs);

    if (mounted_fs->group_descs) {
        kfree(mounted_fs->group_descs);
    }

    for (int i = 0; i < EXT4_MAX_OPEN_INODES; i++) {
        if (mounted_fs->inode_cache[i].vfs_node) {
            ext4_vfs_private_t *priv = mounted_fs->inode_cache[i].vfs_node->private_data;
            if (priv) {
                kfree(priv);
            }
            kfree(mounted_fs->inode_cache[i].vfs_node);
        }
    }

    kfree(mounted_fs);
    mounted_fs = NULL;

    (void)mountpoint;
    return 0;
}

void ext4_init(void) {
    printf("EXT4: Initializing ext4 filesystem driver\n");
}

int ext4_get_stats(uint64_t *total_blocks, uint64_t *free_blocks, uint32_t *block_size) {
    if (!mounted_fs) return -1;
    if (total_blocks) *total_blocks = mounted_fs->total_blocks;
    if (free_blocks) *free_blocks = mounted_fs->sb.s_free_blocks_count_lo;
    if (block_size) *block_size = mounted_fs->block_size;
    return 0;
}

void ext4_vfs_register(void) {
    ext4_fs_type.name = "ext4";
    ext4_fs_type.mount = ext4_do_mount;
    ext4_fs_type.unmount = ext4_do_unmount;
    ext4_fs_type.next = NULL;

    vfs_register_fs(&ext4_fs_type);
}

ext4_fs_t *ext4_mount_disk(uint32_t port_index, const char *mountpoint) {
    ahci_port_t *port;
    ext4_fs_t *fs;
    size_t mp_len;
    vfs_node_t *root;

    port = ahci_get_port(port_index);
    if (!port) {
        return NULL;
    }

    fs = (ext4_fs_t *)kmalloc(sizeof(ext4_fs_t));
    if (!fs) {
        return NULL;
    }

    memset(fs, 0, sizeof(ext4_fs_t));
    fs->port_index = port_index;
    mutex_init(&fs->lock);

    if (ext4_read_superblock(fs) != 0) {
        kfree(fs);
        return NULL;
    }

    if (ext4_validate_superblock(&fs->sb) != 0) {
        kfree(fs);
        return NULL;
    }

    if (ext4_load_group_descs(fs) != 0) {
        kfree(fs);
        return NULL;
    }

    mp_len = strlen(mountpoint);
    if (mp_len >= VFS_MAX_PATH) {
        mp_len = VFS_MAX_PATH - 1;
    }
    memcpy(fs->mountpoint, mountpoint, mp_len);
    fs->mountpoint[mp_len] = '\0';

    root = ext4_create_vfs_node(fs, EXT4_ROOT_INO, "/");
    if (!root) {
        kfree(fs->group_descs);
        kfree(fs);
        return NULL;
    }

    fs->root_node = root;
    mounted_fs = fs;

    return fs;
}

int ext4_unmount(ext4_fs_t *fs) {
    if (!fs) {
        return -1;
    }

    ext4_sync_inodes(fs);
    ext4_sync_blocks(fs);
    ext4_flush_cache(fs);

    if (fs->group_descs) {
        kfree(fs->group_descs);
    }

    if (fs == mounted_fs) {
        mounted_fs = NULL;
    }

    kfree(fs);
    return 0;
}

int ext4_sync(ext4_fs_t *fs) {
    if (!fs) {
        return -1;
    }

    ext4_sync_inodes(fs);
    ext4_sync_blocks(fs);
    ext4_write_superblock(fs);

    return 0;
}

ext4_fs_t *ext4_get_mounted_fs(void) {
    return mounted_fs;
}

#include <lebirun/iso9660.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/vfs.h>
#include <string.h>

static iso9660_context_t iso_ctx;
static int iso_initialized = 0;
static vfs_fs_type_t iso_fs_type;
static dirent_t iso_dirent;

static uint64_t iso_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static void iso_vfs_open(vfs_node_t *node, uint64_t flags);
static void iso_vfs_close(vfs_node_t *node);
static dirent_t *iso_vfs_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *iso_vfs_finddir(vfs_node_t *node, const char *name);

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void iso_name_to_lower(const uint8_t *src, uint8_t src_len, char *dst, size_t dst_size) {
    uint8_t i;
    uint8_t out_len;
    char c;

    out_len = 0;
    for (i = 0; i < src_len && out_len < dst_size - 1; i++) {
        c = (char)src[i];
        if (c == ';')
            break;
        if (c >= 'A' && c <= 'Z')
            c = c + ('a' - 'A');
        dst[out_len++] = c;
    }
    if (out_len > 1 && dst[out_len - 1] == '.')
        out_len--;
    dst[out_len] = '\0';
}

void iso9660_init(uint64_t mod_start, uint64_t mod_end) {
    uint8_t *base;
    uint64_t mod_size;
    uint64_t offset;
    iso9660_pvd_t *pvd;
    iso9660_dirent_t *root_de;

    mod_size = mod_end - mod_start;
    base = (uint8_t *)(mod_start + KERNEL_VMA);

    if (mod_size < ISO9660_SYSTEM_AREA_SIZE + ISO9660_SECTOR_SIZE)
        return;

    pvd = NULL;
    offset = ISO9660_SYSTEM_AREA_SIZE;
    while (offset + ISO9660_SECTOR_SIZE <= mod_size) {
        uint8_t *vd = base + offset;
        if (memcmp(vd + 1, "CD001", 5) != 0)
            break;
        if (vd[0] == ISO9660_VD_PRIMARY) {
            pvd = (iso9660_pvd_t *)vd;
            break;
        }
        if (vd[0] == ISO9660_VD_TERMINATOR)
            break;
        offset += ISO9660_SECTOR_SIZE;
    }

    if (!pvd)
        return;

    iso_ctx.base = base;
    iso_ctx.size = mod_size;
    iso_ctx.pvd = pvd;
    iso_ctx.block_size = read_le16(pvd->logical_block_size);
    if (iso_ctx.block_size == 0)
        iso_ctx.block_size = ISO9660_SECTOR_SIZE;
    iso_ctx.read_fn = NULL;
    iso_ctx.read_priv = NULL;

    root_de = (iso9660_dirent_t *)pvd->root_dir_entry;
    iso_ctx.root_extent = read_le32(root_de->extent_location.le);
    iso_ctx.root_length = read_le32(root_de->data_length.le);

    iso_initialized = 1;
}

static int iso_read_data(iso9660_context_t *ctx, uint64_t byte_offset, uint64_t len, uint8_t *buf) {
    uint64_t lba;
    uint64_t off_in_sec;
    uint64_t chunk;
    uint64_t done;
    uint8_t *tmp;

    if (ctx->base) {
        if (byte_offset + len > ctx->size)
            return -1;
        memcpy(buf, ctx->base + byte_offset, len);
        return 0;
    }
    if (!ctx->read_fn)
        return -1;

    tmp = kmalloc(ISO9660_SECTOR_SIZE);
    if (!tmp)
        return -1;

    done = 0;
    while (done < len) {
        lba = (byte_offset + done) / ISO9660_SECTOR_SIZE;
        off_in_sec = (byte_offset + done) % ISO9660_SECTOR_SIZE;
        chunk = ISO9660_SECTOR_SIZE - off_in_sec;
        if (chunk > len - done)
            chunk = len - done;
        if (ctx->read_fn(lba, 1, tmp, ctx->read_priv) < 0) {
            kfree(tmp);
            return -1;
        }
        memcpy(buf + done, tmp + off_in_sec, chunk);
        done += chunk;
    }
    kfree(tmp);
    return 0;
}

static int iso9660_init_ctx_device(iso9660_context_t *ctx, iso9660_read_fn read_fn, void *priv) {
    uint8_t *sector;
    uint64_t lba;
    iso9660_pvd_t *pvd;
    iso9660_dirent_t *root_de;

    sector = kmalloc(ISO9660_SECTOR_SIZE);
    if (!sector)
        return -1;

    pvd = NULL;
    lba = ISO9660_SYSTEM_AREA_SIZE / ISO9660_SECTOR_SIZE;

    while (1) {
        if (read_fn(lba, 1, sector, priv) < 0)
            break;
        if (memcmp(sector + 1, "CD001", 5) != 0)
            break;
        if (sector[0] == ISO9660_VD_PRIMARY) {
            pvd = (iso9660_pvd_t *)kmalloc(ISO9660_SECTOR_SIZE);
            if (pvd)
                memcpy(pvd, sector, ISO9660_SECTOR_SIZE);
            break;
        }
        if (sector[0] == ISO9660_VD_TERMINATOR)
            break;
        lba++;
    }
    kfree(sector);

    if (!pvd)
        return -1;

    ctx->base = NULL;
    ctx->size = 0;
    ctx->pvd = pvd;
    ctx->block_size = read_le16(pvd->logical_block_size);
    if (ctx->block_size == 0)
        ctx->block_size = ISO9660_SECTOR_SIZE;
    ctx->read_fn = read_fn;
    ctx->read_priv = priv;

    root_de = (iso9660_dirent_t *)pvd->root_dir_entry;
    ctx->root_extent = read_le32(root_de->extent_location.le);
    ctx->root_length = read_le32(root_de->data_length.le);

    return 0;
}

int iso9660_init_device(iso9660_read_fn read_fn, void *priv) {
    int ret;

    ret = iso9660_init_ctx_device(&iso_ctx, read_fn, priv);
    if (ret != 0)
        return ret;
    iso_initialized = 1;
    return 0;
}

static vfs_node_t *iso_create_vfs_node(iso9660_context_t *ctx, const char *name,
                                        uint32_t extent_lba,
                                        uint32_t data_length, uint8_t flags) {
    iso9660_vfs_node_t *inode;
    size_t name_len;

    inode = (iso9660_vfs_node_t *)kmalloc(sizeof(iso9660_vfs_node_t));
    if (!inode) return NULL;
    memset(inode, 0, sizeof(iso9660_vfs_node_t));

    inode->extent_lba = extent_lba;
    inode->data_length = data_length;
    inode->ctx = ctx;

    name_len = strlen(name);
    if (name_len >= VFS_MAX_NAME)
        name_len = VFS_MAX_NAME - 1;
    memcpy(inode->vfs.name, name, name_len);
    inode->vfs.name[name_len] = '\0';

    inode->vfs.mask = 0555;
    inode->vfs.uid = 0;
    inode->vfs.gid = 0;
    inode->vfs.inode = extent_lba;
    inode->vfs.length = data_length;
    inode->vfs.private_data = inode;
    inode->vfs.open = iso_vfs_open;
    inode->vfs.close = iso_vfs_close;

    if (flags & ISO9660_DE_FLAG_DIRECTORY) {
        inode->vfs.flags = VFS_DIRECTORY;
        inode->vfs.readdir = iso_vfs_readdir;
        inode->vfs.finddir = iso_vfs_finddir;
    } else {
        inode->vfs.flags = VFS_FILE;
        inode->vfs.read = iso_vfs_read;
    }

    return &inode->vfs;
}

static uint64_t iso_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    iso9660_vfs_node_t *inode;
    iso9660_context_t *ctx;
    uint64_t file_offset;
    uint64_t to_read;

    if (!node || !buffer) return 0;
    inode = (iso9660_vfs_node_t *)node->private_data;
    if (!inode || !inode->ctx) return 0;
    ctx = inode->ctx;

    if (offset >= inode->data_length) return 0;

    to_read = size;
    if (offset + to_read > inode->data_length)
        to_read = inode->data_length - offset;

    file_offset = (uint64_t)inode->extent_lba * ISO9660_SECTOR_SIZE + offset;
    if (ctx->base && file_offset + to_read > ctx->size)
        return 0;

    if (iso_read_data(ctx, file_offset, to_read, buffer) < 0)
        return 0;
    return to_read;
}

static void iso_vfs_open(vfs_node_t *node, uint64_t flags) {
    (void)node;
    (void)flags;
}

static void iso_vfs_close(vfs_node_t *node) {
    (void)node;
}

static dirent_t *iso_vfs_readdir(vfs_node_t *node, uint64_t index) {
    iso9660_vfs_node_t *inode;
    iso9660_context_t *ctx;
    uint8_t *dir_data;
    uint64_t dir_offset;
    uint64_t dir_size;
    uint64_t byte_off;
    iso9660_dirent_t *de;
    uint64_t cur_index;

    if (!node) return NULL;
    inode = (iso9660_vfs_node_t *)node->private_data;
    if (!inode || !inode->ctx) return NULL;
    ctx = inode->ctx;

    dir_size = inode->data_length;
    byte_off = (uint64_t)inode->extent_lba * ISO9660_SECTOR_SIZE;

    dir_data = kmalloc(dir_size);
    if (!dir_data) return NULL;

    if (iso_read_data(ctx, byte_off, dir_size, dir_data) < 0) {
        kfree(dir_data);
        return NULL;
    }

    cur_index = 0;
    dir_offset = 0;

    while (dir_offset < dir_size) {
        de = (iso9660_dirent_t *)(dir_data + dir_offset);

        if (de->length == 0) {
            uint64_t next_sector;
            next_sector = ((dir_offset / ISO9660_SECTOR_SIZE) + 1) * ISO9660_SECTOR_SIZE;
            if (next_sector >= dir_size) break;
            dir_offset = next_sector;
            continue;
        }

        if (dir_offset + de->length > dir_size) break;

        if (de->name_length == 1 && (de->name[0] == 0 || de->name[0] == 1)) {
            dir_offset += de->length;
            continue;
        }

        if (cur_index == index) {
            iso_name_to_lower(de->name, de->name_length, iso_dirent.name, VFS_MAX_NAME);
            iso_dirent.inode = read_le32(de->extent_location.le);
            iso_dirent.type = (de->file_flags & ISO9660_DE_FLAG_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
            kfree(dir_data);
            return &iso_dirent;
        }

        cur_index++;
        dir_offset += de->length;
    }

    kfree(dir_data);
    return NULL;
}

static vfs_node_t *iso_vfs_finddir(vfs_node_t *node, const char *name) {
    iso9660_vfs_node_t *inode;
    iso9660_context_t *ctx;
    uint8_t *dir_data;
    uint64_t dir_offset;
    uint64_t dir_size;
    iso9660_dirent_t *de;
    char entry_name[VFS_MAX_NAME];
    uint32_t extent;
    uint32_t length;

    if (!node || !name) return NULL;
    inode = (iso9660_vfs_node_t *)node->private_data;
    if (!inode || !inode->ctx) return NULL;
    ctx = inode->ctx;

    dir_size = inode->data_length;

    dir_data = kmalloc(dir_size);
    if (!dir_data) return NULL;

    if (iso_read_data(ctx, (uint64_t)inode->extent_lba * ISO9660_SECTOR_SIZE, dir_size, dir_data) < 0) {
        kfree(dir_data);
        return NULL;
    }

    dir_offset = 0;

    while (dir_offset < dir_size) {
        de = (iso9660_dirent_t *)(dir_data + dir_offset);

        if (de->length == 0) {
            uint64_t next_sector;
            next_sector = ((dir_offset / ISO9660_SECTOR_SIZE) + 1) * ISO9660_SECTOR_SIZE;
            if (next_sector >= dir_size) break;
            dir_offset = next_sector;
            continue;
        }

        if (dir_offset + de->length > dir_size) break;

        if (de->name_length == 1 && (de->name[0] == 0 || de->name[0] == 1)) {
            dir_offset += de->length;
            continue;
        }

        iso_name_to_lower(de->name, de->name_length, entry_name, VFS_MAX_NAME);

        if (strcmp(entry_name, name) == 0) {
            extent = read_le32(de->extent_location.le);
            length = read_le32(de->data_length.le);
            kfree(dir_data);
            return iso_create_vfs_node(ctx, entry_name, extent, length, de->file_flags);
        }

        dir_offset += de->length;
    }

    kfree(dir_data);
    return NULL;
}

static int iso_devnode_read(uint64_t lba, uint32_t count, void *buffer, void *priv) {
    vfs_node_t *dev;
    uint64_t offset;
    uint64_t size;
    uint64_t ret;

    dev = (vfs_node_t *)priv;
    if (!dev)
        return -1;
    offset = lba * ISO9660_SECTOR_SIZE;
    size = (uint64_t)count * ISO9660_SECTOR_SIZE;
    ret = vfs_read(dev, offset, size, (uint8_t *)buffer);
    if (ret != size)
        return -1;
    return 0;
}

static vfs_node_t *iso_mount(const char *device, const char *mountpoint) {
    vfs_node_t *dev_node;
    iso9660_context_t *ctx;
    vfs_node_t *root;

    (void)mountpoint;

    if (device && device[0] != '\0') {
        dev_node = vfs_namei(device);
        if (!dev_node)
            return NULL;
        ctx = (iso9660_context_t *)kmalloc(sizeof(iso9660_context_t));
        if (!ctx)
            return NULL;
        memset(ctx, 0, sizeof(iso9660_context_t));
        if (iso9660_init_ctx_device(ctx, iso_devnode_read, dev_node) != 0) {
            kfree(ctx);
            return NULL;
        }
        root = iso_create_vfs_node(ctx, "iso9660", ctx->root_extent,
                                    ctx->root_length,
                                    ISO9660_DE_FLAG_DIRECTORY);
        if (!root) {
            kfree(ctx->pvd);
            kfree(ctx);
            return NULL;
        }
        return root;
    }

    if (!iso_initialized)
        return NULL;

    return iso_create_vfs_node(&iso_ctx, "iso9660", iso_ctx.root_extent,
                                iso_ctx.root_length,
                                ISO9660_DE_FLAG_DIRECTORY);
}

static int iso_unmount(vfs_node_t *mountpoint) {
    iso9660_vfs_node_t *inode;
    iso9660_context_t *ctx;

    if (!mountpoint)
        return -1;
    inode = (iso9660_vfs_node_t *)mountpoint->private_data;
    if (!inode)
        return -1;
    ctx = inode->ctx;
    if (ctx && ctx != &iso_ctx) {
        if (ctx->pvd)
            kfree(ctx->pvd);
        kfree(ctx);
    }
    kfree(inode);
    return 0;
}

void iso9660_vfs_register(void) {
    iso_fs_type.name = "iso9660";
    iso_fs_type.mount = iso_mount;
    iso_fs_type.unmount = iso_unmount;
    iso_fs_type.next = NULL;
    vfs_register_fs(&iso_fs_type);
}

iso9660_context_t *iso9660_get_context(void) {
    if (!iso_initialized) return NULL;
    return &iso_ctx;
}

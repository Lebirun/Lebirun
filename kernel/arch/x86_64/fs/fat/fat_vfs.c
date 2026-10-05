#include "fat.h"
#include <string.h>
#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <lebirun/ramfs.h>
#include <lebirun/drivers/sata/ahci.h>

typedef struct {
    fat_fs_t *fs;
    uint32_t first_cluster;
    uint32_t dir_cluster;
    int is_root;
    uint64_t entry_lba;
    uint32_t entry_off;
    int has_entry;
} fat_node_t;

static uint64_t fat_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size,
                             uint8_t *buffer);
static uint64_t fat_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size,
                              uint8_t *buffer);
static void fat_vfs_open(vfs_node_t *node, uint64_t flags);
static void fat_vfs_close(vfs_node_t *node);
static dirent_t *fat_vfs_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *fat_vfs_finddir(vfs_node_t *node, const char *name);

static const vfs_node_ops_t fat_node_ops;

static vfs_node_t *fat_make_node(fat_fs_t *fs, const char *name,
                                 uint32_t first_cluster, uint32_t dir_cluster,
                                 int is_root, uint64_t entry_lba,
                                 uint32_t entry_off, int has_entry,
                                 uint64_t length, int is_dir) {
    vfs_node_t *node;
    fat_node_t *priv;

    node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    priv = (fat_node_t *)kmalloc(sizeof(fat_node_t));
    if (!priv) {
        kfree(node);
        return NULL;
    }
    memset(node, 0, sizeof(vfs_node_t));
    priv->fs = fs;
    priv->first_cluster = first_cluster;
    priv->dir_cluster = dir_cluster;
    priv->is_root = is_root;
    priv->entry_lba = entry_lba;
    priv->entry_off = entry_off;
    priv->has_entry = has_entry;
    node->inode = first_cluster;
    node->length = length;
    node->atime = ramfs_get_time();
    node->mtime = node->atime;
    node->ctime = node->atime;
    node->flags = (is_dir ? VFS_DIRECTORY : VFS_FILE) | VFS_DYNAMIC;
    if (vfs_node_set_name(node, name) != 0) {
        kfree(priv);
        kfree(node);
        return NULL;
    }
    node->read = fat_vfs_read;
    node->write = fat_vfs_write;
    node->open = fat_vfs_open;
    node->close = fat_vfs_close;
    node->readdir = fat_vfs_readdir;
    node->finddir = fat_vfs_finddir;
    node->ops = &fat_node_ops;
    node->private_data = priv;
    return node;
}

static int fat_flush_entry(vfs_node_t *node) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_entry_t entry;
    uint16_t fdate;
    uint16_t ftime;

    if (!priv || !priv->has_entry) return 0;
    fat_dos_time(node->mtime, &fdate, &ftime);
    mutex_lock(&priv->fs->lock);
    if (fat_read_lba(priv->fs, priv->entry_lba, priv->fs->scratch) != 0) {
        mutex_unlock(&priv->fs->lock);
        return -1;
    }
    memcpy(entry.raw, priv->fs->scratch + priv->entry_off, FAT_ENTRY_SIZE);
    entry.raw[26] = (uint8_t)(priv->first_cluster & 0xFF);
    entry.raw[27] = (uint8_t)((priv->first_cluster >> 8) & 0xFF);
    entry.raw[20] = (uint8_t)((priv->first_cluster >> 16) & 0xFF);
    entry.raw[21] = (uint8_t)((priv->first_cluster >> 24) & 0xFF);
    entry.raw[28] = (uint8_t)(node->length & 0xFF);
    entry.raw[29] = (uint8_t)((node->length >> 8) & 0xFF);
    entry.raw[30] = (uint8_t)((node->length >> 16) & 0xFF);
    entry.raw[31] = (uint8_t)((node->length >> 24) & 0xFF);
    entry.raw[22] = (uint8_t)(ftime & 0xFF);
    entry.raw[23] = (uint8_t)((ftime >> 8) & 0xFF);
    entry.raw[24] = (uint8_t)(fdate & 0xFF);
    entry.raw[25] = (uint8_t)((fdate >> 8) & 0xFF);
    memcpy(priv->fs->scratch + priv->entry_off, entry.raw, FAT_ENTRY_SIZE);
    if (fat_write_lba(priv->fs, priv->entry_lba, priv->fs->scratch) != 0) {
        mutex_unlock(&priv->fs->lock);
        return -1;
    }
    mutex_unlock(&priv->fs->lock);
    return 0;
}

static uint32_t fat_walk_inner(fat_fs_t *fs, uint32_t *start, uint64_t idx,
                               int extend, int zero) {
    uint32_t c = *start;
    uint64_t i;

    if (idx > fs->cluster_count + 1) return 1;
    if (!c) {
        if (!extend) return 1;
        c = zero ? fat_alloc_cluster(fs, 0) : fat_alloc_cluster_nz(fs, 0);
        if (c < 2) return 1;
        *start = c;
    }
    for (i = 0; i < idx; i++) {
        uint32_t next = fat_next_cluster(fs, c);
        if (next < 2 || next >= fs->cluster_count + 2) {
            if (!extend) return 1;
            next = zero ? fat_alloc_cluster(fs, c) : fat_alloc_cluster_nz(fs, c);
            if (next < 2) return 1;
        }
        c = next;
    }
    return c;
}

static uint32_t fat_walk(fat_fs_t *fs, uint32_t *start, uint64_t idx,
                         int extend) {
    return fat_walk_inner(fs, start, idx, extend, 1);
}

static uint32_t fat_walk_nz(fat_fs_t *fs, uint32_t *start, uint64_t idx,
                            int extend) {
    return fat_walk_inner(fs, start, idx, extend, 0);
}

static uint64_t fat_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size,
                             uint8_t *buffer) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_fs_t *fs;
    uint64_t done = 0;
    uint64_t bpc;

    if (!priv) return 0;
    fs = priv->fs;
    if (offset >= node->length) return 0;
    if (offset + size > node->length) size = node->length - offset;
    bpc = (uint64_t)fs->bps * fs->spc;
    mutex_lock(&fs->lock);
    {
        uint32_t cc = 0;
        uint64_t cc_idx = 0;
        int cc_ok = 0;
        while (done < size) {
            uint64_t pos = offset + done;
            uint64_t idx = pos / bpc;
            uint64_t coff;
            uint64_t chunk;
            uint64_t so;
            uint64_t take;
            uint64_t nsec;
            uint64_t lba;
            uint32_t c;
            if (cc_ok && idx == cc_idx + 1) {
                c = fat_next_cluster(fs, cc);
                if (c < 2 || c >= fs->cluster_count + 2) break;
            } else {
                c = fat_walk(fs, &priv->first_cluster, idx, 0);
                if (c < 2) break;
            }
            cc = c;
            cc_idx = idx;
            cc_ok = 1;
            coff = pos % bpc;
            chunk = bpc - coff;
            if (chunk > size - done) chunk = size - done;
            so = coff % fs->bps;
            if (so > 0) {
                take = fs->bps - so;
                if (take > chunk) take = chunk;
                lba = fat_cluster_lba(fs, c) + coff / fs->bps;
                if (fat_read_lba(fs, lba, fs->scratch) != 0) break;
                memcpy(buffer + done, fs->scratch + so, take);
                done += take;
                chunk -= take;
                coff += take;
            }
            if (chunk >= fs->bps) {
                nsec = chunk / fs->bps;
                lba = fat_cluster_lba(fs, c) + coff / fs->bps;
                if (fat_read_lba_range(fs, lba, nsec, buffer + done) != 0)
                    break;
                done += nsec * fs->bps;
                chunk -= nsec * fs->bps;
                coff += nsec * fs->bps;
            }
            if (chunk > 0) {
                lba = fat_cluster_lba(fs, c) + coff / fs->bps;
                so = coff % fs->bps;
                if (fat_read_lba(fs, lba, fs->scratch) != 0) break;
                memcpy(buffer + done, fs->scratch + so, chunk);
                done += chunk;
                chunk = 0;
            }
        }
    }
    mutex_unlock(&fs->lock);
    return done;
}

static uint64_t fat_vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size,
                              uint8_t *buffer) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_fs_t *fs;
    uint64_t done = 0;
    uint64_t bpc;

    if (!priv) return 0;
    fs = priv->fs;
    bpc = (uint64_t)fs->bps * fs->spc;
    mutex_lock(&fs->lock);
    {
        uint32_t cc = 0;
        uint64_t cc_idx = 0;
        int cc_ok = 0;
        int nz = offset <= node->length;
        while (done < size) {
            uint64_t pos = offset + done;
            uint64_t idx = pos / bpc;
            uint64_t coff;
            uint64_t chunk;
            uint64_t so;
            uint64_t take;
            uint64_t nsec;
            uint64_t lba;
            uint32_t c;
            if (cc_ok && idx == cc_idx + 1) {
                uint32_t nx = fat_next_cluster(fs, cc);
                if (nx < 2 || nx >= fs->cluster_count + 2) {
                    nx = nz ? fat_alloc_cluster_nz(fs, cc) :
                              fat_alloc_cluster(fs, cc);
                    if (nx < 2) break;
                }
                c = nx;
            } else {
                c = nz ? fat_walk_nz(fs, &priv->first_cluster, idx, 1) :
                         fat_walk(fs, &priv->first_cluster, idx, 1);
                if (c < 2) break;
            }
            cc = c;
            cc_idx = idx;
            cc_ok = 1;
            coff = pos % bpc;
            chunk = bpc - coff;
            if (chunk > size - done) chunk = size - done;
            so = coff % fs->bps;
            if (so > 0) {
                take = fs->bps - so;
                if (take > chunk) take = chunk;
                lba = fat_cluster_lba(fs, c) + coff / fs->bps;
                if (fat_read_lba(fs, lba, fs->scratch) != 0) break;
                memcpy(fs->scratch + so, buffer + done, take);
                if (fat_write_lba(fs, lba, fs->scratch) != 0) break;
                done += take;
                chunk -= take;
                coff += take;
            }
            if (chunk >= fs->bps) {
                nsec = chunk / fs->bps;
                lba = fat_cluster_lba(fs, c) + coff / fs->bps;
                if (fat_write_lba_range(fs, lba, nsec, buffer + done) != 0)
                    break;
                done += nsec * fs->bps;
                chunk -= nsec * fs->bps;
                coff += nsec * fs->bps;
            }
            if (chunk > 0) {
                lba = fat_cluster_lba(fs, c) + coff / fs->bps;
                so = coff % fs->bps;
                if (fat_read_lba(fs, lba, fs->scratch) != 0) break;
                memcpy(fs->scratch + so, buffer + done, chunk);
                if (fat_write_lba(fs, lba, fs->scratch) != 0) break;
                done += chunk;
                chunk = 0;
            }
        }
    }
    if (offset + done > node->length) node->length = offset + done;
    node->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    fat_flush_entry(node);
    return done;
}

static void fat_vfs_open(vfs_node_t *node, uint64_t flags) {
    (void)node;
    (void)flags;
}

static void fat_vfs_close(vfs_node_t *node) {
    fat_node_t *priv;

    if (!node || node->ref_count != 0) return;
    priv = (fat_node_t *)node->private_data;
    if (!priv) return;
    fat_flush_entry(node);
    kfree(priv);
    node->private_data = NULL;
}

static void fat_sfn_name(const uint8_t sfn[FAT_SFN_LEN], char *out) {
    uint64_t i;
    uint64_t o = 0;
    int ei;

    for (i = 0; i < 8 && sfn[i] != ' '; i++) {
        char c = (char)sfn[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        out[o++] = c;
    }
    ei = 8;
    while (ei < 11 && sfn[ei] == ' ') ei++;
    if (ei < 11) {
        out[o++] = '.';
        for (; ei < 11; ei++) {
            char c = (char)sfn[ei];
            if (c >= 'A' && c <= 'Z') c += 32;
            out[o++] = c;
        }
    }
    out[o] = 0;
}

static int fat_vfs_readdir_next(vfs_node_t *node, uint64_t *cookie,
                                dirent_t *result) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_fs_t *fs;
    uint64_t idx;
    uint64_t lba;
    uint32_t off;
    fat_entry_t entry;
    uint8_t attr;
    char name[16];

    if (!node || !priv || !cookie || !result) return VFS_READDIR_IO;
    if ((node->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return VFS_READDIR_IO;
    fs = priv->fs;
    idx = *cookie;
    mutex_lock(&fs->lock);
    for (;; idx++) {
        if (fat_dir_slot(fs, priv->dir_cluster, priv->is_root, idx, &lba,
                         &off) != 0) {
            mutex_unlock(&fs->lock);
            return VFS_READDIR_END;
        }
        if (fat_read_entry(fs, lba, off, &entry) != 0) {
            mutex_unlock(&fs->lock);
            return VFS_READDIR_IO;
        }
        if (entry.raw[0] == 0x00) {
            mutex_unlock(&fs->lock);
            return VFS_READDIR_END;
        }
        if (entry.raw[0] == 0xE5) continue;
        attr = entry.raw[11];
        if (attr == FAT_ATTR_LFN) continue;
        if (attr & FAT_ATTR_VOLUME) continue;
        if (entry.raw[0] == '.' &&
            (entry.raw[1] == ' ' ||
             (entry.raw[1] == '.' && entry.raw[2] == ' '))) continue;
        break;
    }
    fat_sfn_name(entry.raw, name);
    if (vfs_dirent_set_name(result, name) != 0) {
        mutex_unlock(&fs->lock);
        return VFS_READDIR_NOMEM;
    }
    result->type = (attr & FAT_ATTR_DIR) ? VFS_DIRECTORY : VFS_FILE;
    result->inode = fat_file_cluster(&entry);
    *cookie = idx + 1;
    mutex_unlock(&fs->lock);
    return 0;
}

static dirent_t *fat_vfs_readdir(vfs_node_t *node, uint64_t index) {
    static dirent_t dent;
    uint64_t cookie = 0;
    uint64_t i;

    for (i = 0; i <= index; i++) {
        if (fat_vfs_readdir_next(node, &cookie, &dent) != 0) return NULL;
    }
    return &dent;
}

static vfs_node_t *fat_vfs_finddir(vfs_node_t *node, const char *name) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_fs_t *fs;
    uint8_t sfn[FAT_SFN_LEN];
    fat_entry_t entry;
    uint64_t elba;
    uint32_t eoff;

    if (!node || !priv || !name) return NULL;
    if ((node->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return NULL;
    if (fat_to_sfn(name, sfn) != 0) return NULL;
    fs = priv->fs;
    mutex_lock(&fs->lock);
    if (fat_dir_find(fs, priv->dir_cluster, priv->is_root, sfn, &entry,
                     &elba, &eoff) != 0) {
        mutex_unlock(&fs->lock);
        return NULL;
    }
    mutex_unlock(&fs->lock);
    return fat_make_node(fs, name, fat_file_cluster(&entry),
                         fat_file_cluster(&entry), 0, elba, eoff, 1,
                         fat_file_size(&entry), fat_is_dir(&entry));
}

static int fat_vfs_create(vfs_node_t *parent, const char *name,
                          uint64_t flags) {
    fat_node_t *priv = (fat_node_t *)parent->private_data;
    fat_fs_t *fs;
    uint8_t sfn[FAT_SFN_LEN];
    uint16_t fdate;
    uint16_t ftime;

    (void)flags;
    if (!parent || !priv || !name) return -1;
    if ((parent->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return -1;
    if (fat_to_sfn(name, sfn) != 0) return -1;
    fs = priv->fs;
    fat_dos_time(ramfs_get_time(), &fdate, &ftime);
    mutex_lock(&fs->lock);
    if (fat_dir_add(fs, priv->dir_cluster, priv->is_root, sfn, FAT_ATTR_ARCHIVE,
                    0, fdate, ftime, NULL, NULL) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    parent->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    return 0;
}

static int fat_vfs_unlink(vfs_node_t *parent, const char *name) {
    fat_node_t *priv = (fat_node_t *)parent->private_data;
    fat_fs_t *fs;
    uint8_t sfn[FAT_SFN_LEN];
    fat_entry_t entry;
    uint64_t elba;
    uint32_t eoff;

    if (!parent || !priv || !name) return -1;
    if ((parent->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return -1;
    if (fat_to_sfn(name, sfn) != 0) return -1;
    fs = priv->fs;
    mutex_lock(&fs->lock);
    if (fat_dir_find(fs, priv->dir_cluster, priv->is_root, sfn, &entry,
                     &elba, &eoff) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    if (entry.raw[11] & FAT_ATTR_DIR) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    fat_free_chain(fs, fat_file_cluster(&entry));
    entry.raw[0] = 0xE5;
    if (fat_dir_emit(fs, elba, eoff, &entry) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    parent->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    return 0;
}

static int fat_vfs_mkdir(vfs_node_t *parent, const char *name,
                         uint64_t perms) {
    fat_node_t *priv = (fat_node_t *)parent->private_data;
    fat_fs_t *fs;
    uint8_t sfn[FAT_SFN_LEN];
    uint32_t cluster;
    fat_entry_t dot;
    uint64_t dlba;
    uint32_t doff;
    uint16_t fdate;
    uint16_t ftime;

    (void)perms;
    if (!parent || !priv || !name) return -1;
    if ((parent->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return -1;
    if (fat_to_sfn(name, sfn) != 0) return -1;
    fs = priv->fs;
    fat_dos_time(ramfs_get_time(), &fdate, &ftime);
    mutex_lock(&fs->lock);
    cluster = fat_alloc_cluster(fs, 0);
    if (cluster < 2) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    if (fat_dir_add(fs, priv->dir_cluster, priv->is_root, sfn, FAT_ATTR_DIR,
                    cluster, fdate, ftime, NULL, NULL) != 0) {
        fat_free_chain(fs, cluster);
        mutex_unlock(&fs->lock);
        return -1;
    }
    memset(dot.raw, ' ', FAT_SFN_LEN);
    dot.raw[0] = '.';
    dot.raw[11] = FAT_ATTR_DIR;
    dot.raw[14] = (uint8_t)(ftime & 0xFF);
    dot.raw[15] = (uint8_t)((ftime >> 8) & 0xFF);
    dot.raw[16] = (uint8_t)(fdate & 0xFF);
    dot.raw[17] = (uint8_t)((fdate >> 8) & 0xFF);
    dot.raw[22] = (uint8_t)(ftime & 0xFF);
    dot.raw[23] = (uint8_t)((ftime >> 8) & 0xFF);
    dot.raw[24] = (uint8_t)(fdate & 0xFF);
    dot.raw[25] = (uint8_t)((fdate >> 8) & 0xFF);
    dot.raw[26] = (uint8_t)(cluster & 0xFF);
    dot.raw[27] = (uint8_t)((cluster >> 8) & 0xFF);
    dot.raw[20] = (uint8_t)((cluster >> 16) & 0xFF);
    dot.raw[21] = (uint8_t)((cluster >> 24) & 0xFF);
    dlba = fat_cluster_lba(fs, cluster);
    doff = 0;
    if (fat_dir_emit(fs, dlba, doff, &dot) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    memset(dot.raw, ' ', FAT_SFN_LEN);
    dot.raw[0] = '.';
    dot.raw[1] = '.';
    dot.raw[11] = FAT_ATTR_DIR;
    dot.raw[14] = (uint8_t)(ftime & 0xFF);
    dot.raw[15] = (uint8_t)((ftime >> 8) & 0xFF);
    dot.raw[16] = (uint8_t)(fdate & 0xFF);
    dot.raw[17] = (uint8_t)((fdate >> 8) & 0xFF);
    dot.raw[22] = (uint8_t)(ftime & 0xFF);
    dot.raw[23] = (uint8_t)((ftime >> 8) & 0xFF);
    dot.raw[24] = (uint8_t)(fdate & 0xFF);
    dot.raw[25] = (uint8_t)((fdate >> 8) & 0xFF);
    {
        uint32_t up = (priv->is_root && fs->fat_bits != 32) ? 0 :
                      priv->dir_cluster;
        dot.raw[26] = (uint8_t)(up & 0xFF);
        dot.raw[27] = (uint8_t)((up >> 8) & 0xFF);
        dot.raw[20] = (uint8_t)((up >> 16) & 0xFF);
        dot.raw[21] = (uint8_t)((up >> 24) & 0xFF);
    }
    if (fat_dir_emit(fs, dlba, 32, &dot) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    parent->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    return 0;
}

static int fat_vfs_truncate(vfs_node_t *node, uint64_t length) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_fs_t *fs;
    uint64_t bpc;
    uint64_t keep;
    uint32_t c;

    if (!node || !priv) return -1;
    if ((node->flags & VFS_TYPE_MASK) != VFS_FILE) return -1;
    fs = priv->fs;
    bpc = (uint64_t)fs->bps * fs->spc;
    mutex_lock(&fs->lock);
    if (length >= node->length) {
        node->length = length;
        mutex_unlock(&fs->lock);
        fat_flush_entry(node);
        return 0;
    }
    if (!length) {
        fat_free_chain(fs, priv->first_cluster);
        priv->first_cluster = 0;
        node->length = 0;
        node->mtime = ramfs_get_time();
        mutex_unlock(&fs->lock);
        fat_flush_entry(node);
        return 0;
    }
    keep = (length + bpc - 1) / bpc;
    c = priv->first_cluster;
    while (keep > 1 && c >= 2) {
        c = fat_next_cluster(fs, c);
        keep--;
    }
    if (c >= 2) {
        uint32_t rest = fat_next_cluster(fs, c);
        fat_set_next(fs, c, fs->fat_bits == 32 ? 0x0FFFFFFF :
                            fs->fat_bits == 16 ? 0xFFFF : 0x0FFF);
        if (rest >= 2) fat_free_chain(fs, rest);
    }
    node->length = length;
    node->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    fat_flush_entry(node);
    return 0;
}

static int fat_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                          vfs_node_t *new_parent, const char *new_name) {
    fat_node_t *opriv = (fat_node_t *)old_parent->private_data;
    fat_node_t *npriv;
    fat_fs_t *fs;
    uint8_t sfn[FAT_SFN_LEN];
    uint8_t nsfn[FAT_SFN_LEN];
    fat_entry_t entry;
    uint64_t elba;
    uint32_t eoff;

    if (!old_parent || !opriv || !old_name || !new_parent || !new_name)
        return -1;
    npriv = (fat_node_t *)new_parent->private_data;
    if (!npriv || npriv->fs != opriv->fs) return -1;
    if (fat_to_sfn(old_name, sfn) != 0) return -1;
    if (fat_to_sfn(new_name, nsfn) != 0) return -1;
    fs = opriv->fs;
    mutex_lock(&fs->lock);
    if (fat_dir_find(fs, opriv->dir_cluster, opriv->is_root, sfn, &entry,
                     &elba, &eoff) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    {
        fat_entry_t check;
        if (!fat_dir_find(fs, npriv->dir_cluster, npriv->is_root, nsfn,
                          &check, NULL, NULL)) {
            mutex_unlock(&fs->lock);
            return -1;
        }
    }
    entry.raw[0] = 0xE5;
    if (fat_dir_emit(fs, elba, eoff, &entry) != 0) {
        mutex_unlock(&fs->lock);
        return -1;
    }
    memcpy(entry.raw, nsfn, FAT_SFN_LEN);
    if (fat_dir_add(fs, npriv->dir_cluster, npriv->is_root, nsfn,
                    entry.raw[11], fat_file_cluster(&entry), 0, 0, &elba,
                    &eoff) != 0) {
        memcpy(entry.raw, sfn, FAT_SFN_LEN);
        fat_dir_emit(fs, elba, eoff, &entry);
        mutex_unlock(&fs->lock);
        return -1;
    }
    {
        fat_entry_t added;
        if (fat_dir_find(fs, npriv->dir_cluster, npriv->is_root, nsfn, &added,
                         &elba, &eoff) == 0) {
            memcpy(added.raw + 12, entry.raw + 12, 20);
            fat_dir_emit(fs, elba, eoff, &added);
        }
    }
    mutex_unlock(&fs->lock);
    return 0;
}

static const vfs_node_ops_t fat_node_ops = {
    .create = fat_vfs_create,
    .unlink = fat_vfs_unlink,
    .mkdir = fat_vfs_mkdir,
    .truncate = fat_vfs_truncate,
    .rename = fat_vfs_rename,
    .readdir_next = fat_vfs_readdir_next
};

static vfs_fs_type_t fat_fs_type;

static vfs_node_t *fat_do_mount(const char *device, const char *mountpoint) {    vfs_node_t *dev_node;
    fat_fs_t *fs;
    vfs_node_t *root;
    uint32_t root_cluster;
    int is_root;

    extern uint64_t devfs_get_partition_start(vfs_node_t *node);

    (void)mountpoint;
    if (!device || !device[0]) return NULL;
    dev_node = vfs_namei(device);
    if (!dev_node) return NULL;
    if ((dev_node->flags & VFS_TYPE_MASK) != VFS_BLOCKDEVICE) {
        vfs_release(dev_node);
        return NULL;
    }
    fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t));
    if (!fs) {
        vfs_release(dev_node);
        return NULL;
    }
    if (fat_mount_fs(fs, ahci_get_port((uint32_t)dev_node->inode),
                     devfs_get_partition_start(dev_node)) != 0) {
        kfree(fs);
        vfs_release(dev_node);
        return NULL;
    }
    vfs_release(dev_node);
    is_root = fs->fat_bits == 32 ? 0 : 1;
    root_cluster = fs->fat_bits == 32 ? fs->root_cluster : 0;
    root = fat_make_node(fs, "", root_cluster, root_cluster, is_root, 0, 0, 0,
                         0, 1);
    if (!root) {
        fat_unmount_fs(fs);
        kfree(fs);
        return NULL;
    }
    __atomic_add_fetch(&root->ref_count, 1, __ATOMIC_ACQ_REL);
    return root;
}

static int fat_do_unmount(vfs_node_t *mountpoint) {
    fat_node_t *priv;
    fat_fs_t *fs;

    if (!mountpoint) return -1;
    priv = (fat_node_t *)mountpoint->private_data;
    if (!priv) return -1;
    fs = priv->fs;
    __atomic_sub_fetch(&mountpoint->ref_count, 1, __ATOMIC_ACQ_REL);
    vfs_release(mountpoint);
    fat_unmount_fs(fs);
    kfree(fs);
    return 0;
}

static int fat_do_sync(vfs_node_t *node, int data_only) {
    (void)node;
    (void)data_only;
    return 0;
}

void fat_vfs_register(void) {
    fat_fs_type.name = "fat";
    fat_fs_type.mount = fat_do_mount;
    fat_fs_type.unmount = fat_do_unmount;
    fat_fs_type.sync = fat_do_sync;
    fat_fs_type.next = NULL;

    vfs_register_fs(&fat_fs_type);
}

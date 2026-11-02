#include "fat.h"
#include <string.h>
#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <lebirun/ramfs.h>
#include <lebirun/blkdev.h>

typedef struct {
    vfs_node_t vfs;
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
static void fat_vfs_close(vfs_node_t *node);
static int fat_vfs_truncate(vfs_node_t *node, uint64_t length);
static dirent_t *fat_vfs_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *fat_vfs_finddir(vfs_node_t *node, const char *name);

static const vfs_node_ops_t fat_node_ops;

static uint16_t fat_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static void fat_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t fat_up(fat_node_t *n) {
    return n->is_root || n->dir_cluster == n->fs->root_cluster ? 0 :
           n->dir_cluster;
}

static vfs_node_t *fat_make_node(fat_fs_t *fs, const char *name,
                                 const fat_found_t *f) {
    fat_node_t *n = (fat_node_t *)kmalloc(sizeof(fat_node_t));
    const uint8_t *r;
    int dir = 1;

    if (!n) return NULL;
    memset(n, 0, sizeof(fat_node_t));
    n->fs = fs;
    if (f) {
        r = f->e.raw;
        dir = fat_is_dir(&f->e);
        n->first_cluster = n->dir_cluster = fat_file_cluster(&f->e);
        n->entry_lba = f->lba;
        n->entry_off = f->off;
        n->has_entry = 1;
        n->vfs.inode = f->lba * 128 + f->off / FAT_ENTRY_SIZE;
        n->vfs.length = dir ? 0 : fat_file_size(&f->e);
        n->vfs.mtime = fat_unix(fat_rd16(r + 24), fat_rd16(r + 22));
        n->vfs.atime = fat_unix(fat_rd16(r + 18), 0);
        if (!n->vfs.atime) n->vfs.atime = n->vfs.mtime;
        n->vfs.ctime = n->vfs.mtime;
    } else {
        n->is_root = fs->fat_bits != 32;
        n->first_cluster = n->dir_cluster = n->is_root ? 0 : fs->root_cluster;
        n->vfs.inode = 1;
        n->vfs.mtime = n->vfs.atime = n->vfs.ctime = ramfs_get_time();
    }
    n->vfs.mask = 0755;
    n->vfs.flags = (dir ? VFS_DIRECTORY : VFS_FILE) | VFS_DYNAMIC | VFS_EMBEDDED;
    if (vfs_node_set_name(&n->vfs, name) != 0) {
        kfree(n);
        return NULL;
    }
    n->vfs.read = fat_vfs_read;
    n->vfs.write = fat_vfs_write;
    n->vfs.close = fat_vfs_close;
    n->vfs.readdir = fat_vfs_readdir;
    n->vfs.finddir = fat_vfs_finddir;
    n->vfs.ops = &fat_node_ops;
    n->vfs.private_data = n;
    return &n->vfs;
}

static int fat_flush_entry_locked(vfs_node_t *node) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_entry_t entry;
    uint16_t fdate;
    uint16_t ftime;

    if (!priv || !priv->has_entry) return 0;
    if (fat_read_entry(priv->fs, priv->entry_lba, priv->entry_off, &entry) != 0)
        return -1;
    if (!entry.raw[0] || entry.raw[0] == 0xE5 ||
        (entry.raw[11] & 0x3F) == FAT_ATTR_LFN)
        return -1;
    fat_dos_time(node->mtime, &fdate, &ftime);
    fat_set_cluster(entry.raw, priv->first_cluster);
    if (!fat_is_dir(&entry)) {
        entry.raw[28] = (uint8_t)(node->length & 0xFF);
        entry.raw[29] = (uint8_t)((node->length >> 8) & 0xFF);
        entry.raw[30] = (uint8_t)((node->length >> 16) & 0xFF);
        entry.raw[31] = (uint8_t)((node->length >> 24) & 0xFF);
    }
    fat_wr16(entry.raw + 18, fdate);
    fat_wr16(entry.raw + 22, ftime);
    fat_wr16(entry.raw + 24, fdate);
    return fat_dir_emit(priv->fs, priv->entry_lba, priv->entry_off, &entry);
}

static int fat_flush_entry(vfs_node_t *node) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    int result;

    if (!priv || !priv->has_entry) return 0;
    mutex_lock(&priv->fs->lock);
    result = fat_flush_entry_locked(node);
    mutex_unlock(&priv->fs->lock);
    return result;
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

    if (!priv || offset >= 0xFFFFFFFFULL) return 0;
    if (size > 0xFFFFFFFFULL - offset) size = 0xFFFFFFFFULL - offset;
    if (offset > node->length && fat_vfs_truncate(node, offset) != 0) return 0;
    fs = priv->fs;
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
                uint32_t nx = fat_next_cluster(fs, cc);
                if (nx < 2 || nx >= fs->cluster_count + 2) {
                    nx = fat_alloc_cluster_nz(fs, cc);
                    if (nx < 2) break;
                }
                c = nx;
            } else {
                c = fat_walk_nz(fs, &priv->first_cluster, idx, 1);
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

static void fat_vfs_close(vfs_node_t *node) {
    if (!node || node->ref_count != 0) return;
    fat_flush_entry(node);
    vfs_node_release_name(node);
    kfree(node);
}

static int fat_vfs_readdir_next(vfs_node_t *node, uint64_t *cookie,
                                dirent_t *result) {
    fat_node_t *priv = (fat_node_t *)node->private_data;
    fat_fs_t *fs;
    fat_found_t f;
    uint64_t idx;
    int r;

    if (!node || !priv || !cookie || !result) return VFS_READDIR_IO;
    if ((node->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return VFS_READDIR_IO;
    fs = priv->fs;
    idx = *cookie;
    mutex_lock(&fs->lock);
    r = fat_dir_next(fs, priv->dir_cluster, priv->is_root, &idx, &f, fs->name);
    if (!r) {
        fsname_to_utf8(fs->name, f.len, fs->u8);
        if (vfs_dirent_set_name(result, fs->u8) != 0) {
            r = VFS_READDIR_NOMEM;
        } else {
            result->type = fat_is_dir(&f.e) ? VFS_DIRECTORY : VFS_FILE;
            result->inode = f.lba * 128 + f.off / FAT_ENTRY_SIZE;
            *cookie = idx;
        }
    } else {
        r = r > 0 ? VFS_READDIR_END : VFS_READDIR_IO;
    }
    mutex_unlock(&fs->lock);
    return r;
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
    vfs_node_t *r = NULL;
    fat_found_t f;

    if (!node || !priv || !name) return NULL;
    if ((node->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return NULL;
    mutex_lock(&priv->fs->lock);
    if (!fat_dir_lookup(priv->fs, priv->dir_cluster, priv->is_root, name, &f))
        r = fat_make_node(priv->fs, name, &f);
    mutex_unlock(&priv->fs->lock);
    return r;
}

static void fat_tmpl(uint8_t *raw, uint8_t attr, uint32_t cluster) {
    uint16_t fdate;
    uint16_t ftime;

    memset(raw, 0, FAT_ENTRY_SIZE);
    fat_dos_time(ramfs_get_time(), &fdate, &ftime);
    raw[11] = attr;
    fat_wr16(raw + 14, ftime);
    fat_wr16(raw + 16, fdate);
    fat_wr16(raw + 18, fdate);
    fat_wr16(raw + 22, ftime);
    fat_wr16(raw + 24, fdate);
    fat_set_cluster(raw, cluster);
}

static int fat_vfs_mk(vfs_node_t *parent, const char *name, int dir,
                      int excl) {
    fat_node_t *priv;
    fat_fs_t *fs;
    fat_found_t f;
    fat_entry_t e;
    uint64_t lba;
    uint32_t cluster = 0;
    int r;

    if (!parent || !name) return -1;
    priv = (fat_node_t *)parent->private_data;
    if (!priv || (parent->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return -1;
    fs = priv->fs;
    mutex_lock(&fs->lock);
    if ((r = fat_dir_lookup(fs, priv->dir_cluster, priv->is_root, name, &f)) <= 0) {
        mutex_unlock(&fs->lock);
        return r ? -1 : dir || excl ? -17 : 0;
    }
    if (dir && (cluster = fat_alloc_cluster(fs, 0)) < 2) {
        mutex_unlock(&fs->lock);
        return -28;
    }
    fat_tmpl(e.raw, dir ? FAT_ATTR_DIR : FAT_ATTR_ARCHIVE, cluster);
    r = fat_dir_create(fs, priv->dir_cluster, priv->is_root, name, e.raw,
                       NULL, NULL);
    if (!r && dir) {
        lba = fat_cluster_lba(fs, cluster);
        memset(e.raw, ' ', FAT_SFN_LEN);
        e.raw[0] = '.';
        if (fat_dir_emit(fs, lba, 0, &e) != 0) r = -1;
        e.raw[1] = '.';
        fat_set_cluster(e.raw, fat_up(priv));
        if (!r && fat_dir_emit(fs, lba, FAT_ENTRY_SIZE, &e) != 0) r = -1;
    } else if (r && dir) {
        fat_free_chain(fs, cluster);
    }
    if (!r) parent->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    return r;
}

static int fat_vfs_create(vfs_node_t *parent, const char *name,
                          uint64_t flags) {
    return fat_vfs_mk(parent, name, 0, (flags & VFS_CREATE_EXCL) != 0);
}

static int fat_vfs_mkdir(vfs_node_t *parent, const char *name,
                         uint64_t perms) {
    (void)perms;
    return fat_vfs_mk(parent, name, 1, 1);
}

static int fat_dir_empty(fat_fs_t *fs, const fat_found_t *f) {
    fat_found_t c;
    uint64_t i = 0;
    int r;

    if (!fat_is_dir(&f->e)) return 1;
    r = fat_dir_next(fs, fat_file_cluster(&f->e), 0, &i, &c, NULL);
    return r < 0 ? -1 : r;
}

static int fat_vfs_unlink(vfs_node_t *parent, const char *name) {
    fat_node_t *priv = (fat_node_t *)parent->private_data;
    fat_fs_t *fs;
    fat_found_t f;
    int r = -1;

    if (!parent || !priv || !name) return -1;
    if ((parent->flags & VFS_TYPE_MASK) != VFS_DIRECTORY) return -1;
    fs = priv->fs;
    mutex_lock(&fs->lock);
    if (!fat_dir_lookup(fs, priv->dir_cluster, priv->is_root, name, &f)) {
        r = fat_dir_empty(fs, &f);
        if (r > 0 && !(r = fat_dir_remove(fs, priv->dir_cluster, priv->is_root, &f))) {
            fat_free_chain(fs, fat_file_cluster(&f.e));
            parent->mtime = ramfs_get_time();
        } else if (!r) {
            r = -39;
        }
    }
    mutex_unlock(&fs->lock);
    return r;
}

static int fat_vfs_truncate(vfs_node_t *node, uint64_t length) {
    fat_node_t *priv;
    fat_fs_t *fs;
    uint64_t bpc;
    uint64_t keep;
    uint64_t old_length;
    uint64_t old_mtime;
    uint64_t pos;
    uint64_t idx;
    uint64_t lba;
    uint64_t take;
    uint32_t off;
    uint32_t c;
    uint32_t rest;
    uint32_t first_new = 0;
    uint32_t old_tail = 0;

    if (!node) return -1;
    priv = (fat_node_t *)node->private_data;
    if (!priv || length > 0xFFFFFFFFULL) return -1;
    if ((node->flags & VFS_TYPE_MASK) != VFS_FILE) return -1;
    fs = priv->fs;
    bpc = (uint64_t)fs->bps * fs->spc;
    mutex_lock(&fs->lock);
    if (length >= node->length) {
        old_length = node->length;
        old_mtime = node->mtime;
        pos = old_length;
        if (pos < length) {
            c = priv->first_cluster;
            if (!c) {
                c = fat_alloc_cluster_nz(fs, 0);
                if (c < 2) goto fail;
                priv->first_cluster = c;
                first_new = c;
            }
            idx = pos / bpc;
            while (idx) {
                if (c < 2 || c >= fs->cluster_count + 2) goto fail;
                rest = fat_next_cluster(fs, c);
                if (rest >= (fs->fat_bits == 32 ? 0x0FFFFFF8 :
                             fs->fat_bits == 16 ? 0xFFF8 : 0xFF8)) {
                    rest = fat_alloc_cluster_nz(fs, c);
                    if (rest < 2) goto fail;
                    if (!first_new) {
                        first_new = rest;
                        old_tail = c;
                    }
                }
                if (rest < 2 || rest >= fs->cluster_count + 2) goto fail;
                c = rest;
                idx--;
            }
            while (pos < length) {
                if (c < 2 || c >= fs->cluster_count + 2) goto fail;
                off = (uint32_t)(pos % fs->bps);
                take = fs->bps - off;
                if (take > length - pos) take = length - pos;
                lba = fat_cluster_lba(fs, c) + (pos % bpc) / fs->bps;
                if (off || take < fs->bps) {
                    if (fat_read_lba(fs, lba, fs->scratch) != 0) goto fail;
                }
                memset(fs->scratch + off, 0, take);
                if (fat_write_lba(fs, lba, fs->scratch) != 0) goto fail;
                pos += take;
                if (pos < length && pos % bpc == 0) {
                    rest = fat_next_cluster(fs, c);
                    if (rest >= (fs->fat_bits == 32 ? 0x0FFFFFF8 :
                                 fs->fat_bits == 16 ? 0xFFF8 : 0xFF8)) {
                        rest = fat_alloc_cluster_nz(fs, c);
                        if (rest < 2) goto fail;
                        if (!first_new) {
                            first_new = rest;
                            old_tail = c;
                        }
                    }
                    if (rest < 2 || rest >= fs->cluster_count + 2) goto fail;
                    c = rest;
                }
            }
            node->mtime = ramfs_get_time();
        }
        node->length = length;
        if (fat_flush_entry_locked(node) != 0) {
            node->length = old_length;
            node->mtime = old_mtime;
            goto fail;
        }
        mutex_unlock(&fs->lock);
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
        rest = fat_next_cluster(fs, c);
        fat_set_next(fs, c, fs->fat_bits == 32 ? 0x0FFFFFFF :
                            fs->fat_bits == 16 ? 0xFFFF : 0x0FFF);
        if (rest >= 2) fat_free_chain(fs, rest);
    }
    node->length = length;
    node->mtime = ramfs_get_time();
    mutex_unlock(&fs->lock);
    fat_flush_entry(node);
    return 0;
fail:
    if (first_new) {
        if (!old_tail) {
            priv->first_cluster = 0;
            fat_free_chain(fs, first_new);
        } else if (fat_set_next(fs, old_tail,
                               fs->fat_bits == 32 ? 0x0FFFFFFF :
                               fs->fat_bits == 16 ? 0xFFFF : 0x0FFF) == 0) {
            fat_free_chain(fs, first_new);
        }
    }
    mutex_unlock(&fs->lock);
    return -1;
}

static int fat_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                          vfs_node_t *new_parent, const char *new_name) {
    fat_node_t *o = (fat_node_t *)old_parent->private_data;
    fat_node_t *d;
    fat_fs_t *fs;
    fat_found_t e;
    fat_found_t t;
    fat_entry_t up;
    uint64_t lba;
    uint32_t off;
    int same_dir;
    int dir;
    int r = -1;

    if (!o || !old_name || !new_parent || !new_name ||
        new_parent->ops != &fat_node_ops)
        return -1;
    d = (fat_node_t *)new_parent->private_data;
    if (!d || d->fs != o->fs) return -1;
    fs = o->fs;
    same_dir = o->dir_cluster == d->dir_cluster && o->is_root == d->is_root;
    mutex_lock(&fs->lock);
    if (fat_dir_lookup(fs, o->dir_cluster, o->is_root, old_name, &e) != 0 ||
        (r = fat_dir_lookup(fs, d->dir_cluster, d->is_root, new_name, &t)) < 0)
        goto out;
    dir = fat_is_dir(&e.e);
    if (!r && (!same_dir || t.idx != e.idx)) {
        if (fat_is_dir(&t.e) != dir) {
            r = dir ? -20 : -21;
            goto out;
        }
        if ((r = fat_dir_empty(fs, &t)) <= 0) {
            r = r ? -1 : -39;
            goto out;
        }
        if ((r = fat_dir_remove(fs, d->dir_cluster, d->is_root, &t)) != 0)
            goto out;
        fat_free_chain(fs, fat_file_cluster(&t.e));
    }
    if ((r = fat_dir_create(fs, d->dir_cluster, d->is_root, new_name, e.e.raw,
                            NULL, NULL)) != 0 ||
        (r = fat_dir_remove(fs, o->dir_cluster, o->is_root, &e)) != 0)
        goto out;
    if (dir && !same_dir &&
        !fat_dir_slot(fs, fat_file_cluster(&e.e), 0, 1, &lba, &off, NULL) &&
        !fat_read_entry(fs, lba, off, &up) && up.raw[0] == '.' &&
        up.raw[1] == '.') {
        fat_set_cluster(up.raw, fat_up(d));
        r = fat_dir_emit(fs, lba, off, &up);
    }
out:
    mutex_unlock(&fs->lock);
    return r;
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

static vfs_node_t *fat_do_mount(const char *device, const char *mountpoint) {
    vfs_node_t *dev_node;
    fat_fs_t *fs;
    vfs_node_t *root;

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
    if (fat_mount_fs(fs, dev_node->inode,
                     devfs_get_partition_start(dev_node)) != 0) {
        kfree(fs);
        vfs_release(dev_node);
        return NULL;
    }
    vfs_release(dev_node);
    root = fat_make_node(fs, "", NULL);
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
    fat_sync_fsinfo(fs);
    fat_unmount_fs(fs);
    kfree(fs);
    return 0;
}

static int fat_do_sync(vfs_node_t *node, int data_only) {
    fat_fs_t *fs;
    int r;

    (void)data_only;
    if (!node || !node->private_data) return 0;
    fs = ((fat_node_t *)node->private_data)->fs;
    mutex_lock(&fs->lock);
    r = fat_sync_fsinfo(fs);
    mutex_unlock(&fs->lock);
    return r;
}

static int fat_do_statfs(vfs_node_t *root, vfs_statfs_t *st) {
    fat_fs_t *fs = ((fat_node_t *)root->private_data)->fs;

    mutex_lock(&fs->lock);
    st->bfree = fat_count_free(fs);
    mutex_unlock(&fs->lock);
    st->type = 0x4D44;
    st->bsize = (uint64_t)fs->bps * fs->spc;
    st->blocks = fs->cluster_count;
    return st->bfree == UINT32_MAX ? -1 : 0;
}

void fat_vfs_register(void) {
    fat_fs_type.name = "fat";
    fat_fs_type.mount = fat_do_mount;
    fat_fs_type.unmount = fat_do_unmount;
    fat_fs_type.sync = fat_do_sync;
    fat_fs_type.statfs = fat_do_statfs;
    fat_fs_type.next = NULL;

    vfs_register_fs(&fat_fs_type);
}

#include "fat.h"
#include <string.h>
#include <lebirun/mem_map.h>

static uint16_t fat_get16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t fat_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fat_inval(fat_fs_t *fs, uint64_t lba, uint64_t n) {
    if (fs->fbuf_lba + 1 >= lba && fs->fbuf_lba < lba + n) fs->fbuf_lba = ~0ULL;
    if (fs->dbuf_lba - lba < n) fs->dbuf_lba = ~0ULL;
}

int fat_read_lba(fat_fs_t *fs, uint64_t lba, uint8_t *buf) {
    uint64_t sectors;
    uint64_t i;

    sectors = (fs->bps + 511) / 512;
    for (i = 0; i < sectors; i++) {
        if (blkdev_read(blkdev_get(fs->dev), fs->part_lba + lba * sectors + i,
                        1, buf + i * 512) != 0) return -1;
    }
    return 0;
}

int fat_read_lba_range(fat_fs_t *fs, uint64_t lba, uint64_t count,
                       uint8_t *buf) {
    uint64_t sectors;

    if (!count) return 0;
    sectors = (fs->bps + 511) / 512;
    return blkdev_read(blkdev_get(fs->dev), fs->part_lba + lba * sectors,
                       count * sectors, buf);
}

int fat_write_lba(fat_fs_t *fs, uint64_t lba, const uint8_t *buf) {
    uint64_t sectors;
    uint64_t i;

    fat_inval(fs, lba, 1);
    sectors = (fs->bps + 511) / 512;
    for (i = 0; i < sectors; i++) {
        if (blkdev_write(blkdev_get(fs->dev), fs->part_lba + lba * sectors + i,
                         1, buf + i * 512) != 0) return -1;
    }
    return 0;
}

int fat_write_lba_range(fat_fs_t *fs, uint64_t lba, uint64_t count,
                        const uint8_t *buf) {
    uint64_t sectors;

    if (!count) return 0;
    fat_inval(fs, lba, count);
    sectors = (fs->bps + 511) / 512;
    return blkdev_write(blkdev_get(fs->dev), fs->part_lba + lba * sectors,
                        count * sectors, buf);
}

uint64_t fat_cluster_lba(fat_fs_t *fs, uint32_t cluster) {
    return (uint64_t)fs->data_start + (uint64_t)(cluster - 2) * fs->spc;
}

static int fat_is_eoc(fat_fs_t *fs, uint32_t v) {
    if (fs->fat_bits == 32) return v >= 0x0FFFFFF8;
    if (fs->fat_bits == 16) return v >= 0xFFF8;
    return v >= 0xFF8;
}

static uint32_t fat_eoc(fat_fs_t *fs) {
    if (fs->fat_bits == 32) return 0x0FFFFFFF;
    if (fs->fat_bits == 16) return 0xFFFF;
    return 0x0FFF;
}

static uint64_t fat_table_lba(fat_fs_t *fs, uint32_t cluster, uint32_t *off) {
    uint64_t bit_off;

    if (fs->fat_bits == 32)
        bit_off = (uint64_t)cluster * 4;
    else if (fs->fat_bits == 16)
        bit_off = (uint64_t)cluster * 2;
    else
        bit_off = (uint64_t)cluster + (uint64_t)cluster / 2;
    *off = (uint32_t)(bit_off % fs->bps);
    return (uint64_t)fs->reserved + bit_off / fs->bps;
}

uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster) {
    uint64_t lba;
    uint32_t off;
    uint32_t value;
    uint8_t *p;

    if (cluster < 2 || cluster >= fs->cluster_count + 2) return 1;
    lba = fat_table_lba(fs, cluster, &off);
    if (fs->fbuf_lba + 1 == lba && off + 4 <= fs->bps) {
        p = fs->fbuf + fs->bps + off;
    } else {
        if (fs->fbuf_lba != lba) {
            fs->fbuf_lba = ~0ULL;
            if (fat_read_lba_range(fs, lba, 2, fs->fbuf) != 0) return 1;
            fs->fbuf_lba = lba;
        }
        p = fs->fbuf + off;
    }
    if (fs->fat_bits == 32) return fat_get32(p) & 0x0FFFFFFF;
    value = fat_get16(p);
    if (fs->fat_bits == 16) return value;
    return cluster & 1 ? value >> 4 : value & 0x0FFF;
}

static int fat_patch_entry(fat_fs_t *fs, uint8_t *sec, uint32_t cluster,
                           uint32_t off, uint32_t value) {
    uint32_t old;
    uint8_t b0;
    uint8_t b1;

    if (cluster < 2 || cluster >= fs->cluster_count + 2) return -1;
    if (fs->fat_bits == 32) {
        if (off + 4 > fs->bps) return -1;
        old = fat_get32(sec + off) & 0xF0000000;
        value = (value & 0x0FFFFFFF) | old;
        sec[off] = (uint8_t)value;
        sec[off + 1] = (uint8_t)(value >> 8);
        sec[off + 2] = (uint8_t)(value >> 16);
        sec[off + 3] = (uint8_t)(value >> 24);
    } else if (fs->fat_bits == 16) {
        if (off + 2 > fs->bps) return -1;
        sec[off] = (uint8_t)value;
        sec[off + 1] = (uint8_t)(value >> 8);
    } else {
        if (off + 2 > fs->bps * 2) return -1;
        b0 = sec[off];
        b1 = sec[off + 1];
        if (cluster & 1) {
            b0 = (uint8_t)((b0 & 0x0F) | ((value & 0x0F) << 4));
            b1 = (uint8_t)(value >> 4);
        } else {
            b0 = (uint8_t)value;
            b1 = (uint8_t)((b1 & 0xF0) | ((value >> 8) & 0x0F));
        }
        sec[off] = b0;
        sec[off + 1] = b1;
    }
    return 0;
}

int fat_set_next(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint64_t lba;
    uint64_t flba;
    uint32_t off;
    uint8_t f;
    int crossing;

    if (cluster < 2 || cluster >= fs->cluster_count + 2) return -1;
    lba = fat_table_lba(fs, cluster, &off);
    crossing = fs->fat_bits == 12 && off + 2 > fs->bps;
    for (f = 0; f < fs->fats; f++) {
        flba = lba + (uint64_t)f * fs->fat_sectors;
        if (fat_read_lba(fs, flba, fs->scratch) != 0) return -1;
        if (crossing &&
            fat_read_lba(fs, flba + 1, fs->scratch + fs->bps) != 0)
            return -1;
        if (fat_patch_entry(fs, fs->scratch, cluster, off, value) != 0)
            return -1;
        if (fat_write_lba(fs, flba, fs->scratch) != 0) return -1;
        if (crossing &&
            fat_write_lba(fs, flba + 1, fs->scratch + fs->bps) != 0)
            return -1;
    }
    return 0;
}

int fat_chain_append(fat_fs_t *fs, uint32_t prev, uint32_t next) {
    uint64_t lba_n;
    uint64_t lba_p;
    uint64_t flba_n;
    uint64_t flba_p;
    uint32_t off_n;
    uint32_t off_p;
    uint32_t eoc;
    uint8_t f;

    if (next < 2 || next >= fs->cluster_count + 2) return -1;
    eoc = fat_eoc(fs);
    if (prev < 2) return fat_set_next(fs, next, eoc);
    if (prev >= fs->cluster_count + 2) return -1;
    lba_n = fat_table_lba(fs, next, &off_n);
    lba_p = fat_table_lba(fs, prev, &off_p);
    if (fs->fat_bits == 12 &&
        (off_n + 2 > fs->bps || off_p + 2 > fs->bps)) {
        if (fat_set_next(fs, next, eoc) != 0) goto fail;
        if (fat_set_next(fs, prev, next) != 0) {
            if (fat_set_next(fs, prev, eoc) != 0) return -1;
            goto fail;
        }
        return 0;
    }
    for (f = 0; f < fs->fats; f++) {
        flba_n = lba_n + (uint64_t)f * fs->fat_sectors;
        flba_p = lba_p + (uint64_t)f * fs->fat_sectors;
        if (flba_n == flba_p) {
            if (fat_read_lba(fs, flba_n, fs->scratch) != 0) goto fail;
            if (fat_patch_entry(fs, fs->scratch, next, off_n, eoc) != 0)
                goto fail;
            if (fat_patch_entry(fs, fs->scratch, prev, off_p, next) != 0)
                goto fail;
            if (fat_write_lba(fs, flba_n, fs->scratch) != 0) goto fail;
        } else {
            if (fat_read_lba(fs, flba_n, fs->scratch) != 0) goto fail;
            if (fat_patch_entry(fs, fs->scratch, next, off_n, eoc) != 0)
                goto fail;
            if (fat_write_lba(fs, flba_n, fs->scratch) != 0) goto fail;
            if (fat_read_lba(fs, flba_p, fs->scratch) != 0) {
                fat_set_next(fs, next, 0);
                goto fail;
            }
            if (fat_patch_entry(fs, fs->scratch, prev, off_p, next) != 0) {
                fat_set_next(fs, next, 0);
                goto fail;
            }
            if (fat_write_lba(fs, flba_p, fs->scratch) != 0) {
                fat_set_next(fs, next, 0);
                goto fail;
            }
        }
    }
    return 0;
fail:
    fat_set_next(fs, next, 0);
    return -1;
}

static int fat_zero_cluster(fat_fs_t *fs, uint32_t cluster) {
    uint64_t lba;
    uint64_t i;

    lba = fat_cluster_lba(fs, cluster);
    memset(fs->scratch, 0, (uint64_t)fs->bps * 2);
    for (i = 0; i < fs->spc; i += 2) {
        if (fat_write_lba_range(fs, lba + i, fs->spc - i < 2 ? 1 : 2,
                                fs->scratch) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_alloc_inner(fat_fs_t *fs, uint32_t prev, int zero) {
    uint32_t c;
    uint64_t checked = 0;
    uint64_t total;

    total = fs->cluster_count;
    if (!fs->free_count) return 1;
    if (fs->alloc_hint < 2 || fs->alloc_hint >= fs->cluster_count + 2)
        fs->alloc_hint = 2;
    c = fs->alloc_hint;
    while (checked < total) {
        if (fat_next_cluster(fs, c) == 0) {
            fs->alloc_hint = c + 1;
            if (fs->alloc_hint >= fs->cluster_count + 2)
                fs->alloc_hint = 2;
            if (fat_chain_append(fs, prev, c) != 0) return 1;
            if (zero && fat_zero_cluster(fs, c) != 0) {
                if (prev >= 2) fat_set_next(fs, prev, fat_eoc(fs));
                fat_set_next(fs, c, 0);
                return 1;
            }
            if (fs->free_count != UINT32_MAX) fs->free_count--;
            fs->fsinfo_dirty = 1;
            return c;
        }
        if (++c >= fs->cluster_count + 2) c = 2;
        checked++;
    }
    return 1;
}

uint32_t fat_alloc_cluster(fat_fs_t *fs, uint32_t prev) {
    return fat_alloc_inner(fs, prev, 1);
}

uint32_t fat_alloc_cluster_nz(fat_fs_t *fs, uint32_t prev) {
    return fat_alloc_inner(fs, prev, 0);
}

void fat_free_chain(fat_fs_t *fs, uint32_t start) {
    uint32_t c = start;
    uint32_t next;
    uint64_t hops = 0;

    while (c >= 2 && c < fs->cluster_count + 2) {
        next = fat_next_cluster(fs, c);
        if (next && fat_set_next(fs, c, 0) == 0) {
            if (fs->free_count != UINT32_MAX) fs->free_count++;
            fs->fsinfo_dirty = 1;
        }
        if (fat_is_eoc(fs, next)) break;
        if (next < 2 || next >= fs->cluster_count + 2) break;
        if (++hops > fs->cluster_count + 1) break;
        c = next;
    }
}

uint32_t fat_count_free(fat_fs_t *fs) {
    uint32_t c;
    uint32_t n = 0;

    if (fs->free_count != UINT32_MAX) return fs->free_count;
    for (c = 2; c < fs->cluster_count + 2; c++)
        if (!fat_next_cluster(fs, c)) n++;
    return fs->free_count = n;
}

int fat_sync_fsinfo(fat_fs_t *fs) {
    uint8_t *p = fs->scratch;
    uint32_t hint = fs->alloc_hint;
    int i;

    if (!fs->fsinfo || !fs->fsinfo_dirty) return 0;
    if (fat_read_lba(fs, fs->fsinfo, p) != 0) return -1;
    fs->fsinfo_dirty = 0;
    if (fat_get32(p) != 0x41615252 || fat_get32(p + 484) != 0x61417272)
        return 0;
    if (hint < 2 || hint >= fs->cluster_count + 2) hint = UINT32_MAX;
    for (i = 0; i < 4; i++) {
        p[488 + i] = (uint8_t)(fs->free_count >> (i * 8));
        p[492 + i] = (uint8_t)(hint >> (i * 8));
    }
    return fat_write_lba(fs, fs->fsinfo, p);
}

int fat_to_sfn(const char *name, uint8_t sfn[FAT_SFN_LEN]) {
    uint64_t i;
    uint64_t dot = 0;
    uint64_t ni = 0;
    uint64_t ei = 0;
    char c;

    memset(sfn, ' ', FAT_SFN_LEN);
    for (i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i + 1;
    }
    if (!dot) {
        for (i = 0; name[i]; i++) {
            c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (ni >= 8) return -1;
            sfn[ni++] = (uint8_t)c;
        }
        return ni ? 0 : -1;
    }
    for (i = 0; i < dot - 1; i++) {
        c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (ni >= 8) return -1;
        sfn[ni++] = (uint8_t)c;
    }
    if (!ni) return -1;
    for (i = dot; name[i]; i++) {
        c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (ei >= 3) return -1;
        sfn[8 + ei++] = (uint8_t)c;
    }
    return 0;
}

uint64_t fat_file_size(const fat_entry_t *entry) {
    return fat_get32(entry->raw + 28);
}

uint32_t fat_file_cluster(const fat_entry_t *entry) {
    return (uint32_t)fat_get16(entry->raw + 26) |
           ((uint32_t)fat_get16(entry->raw + 20) << 16);
}

int fat_is_dir(const fat_entry_t *entry) {
    return (entry->raw[11] & FAT_ATTR_DIR) != 0;
}

void fat_set_cluster(uint8_t *raw, uint32_t cluster) {
    raw[26] = (uint8_t)cluster;
    raw[27] = (uint8_t)(cluster >> 8);
    raw[20] = (uint8_t)(cluster >> 16);
    raw[21] = (uint8_t)(cluster >> 24);
}

int fat_dir_slot(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                 uint64_t idx, uint64_t *lba, uint32_t *off, fat_cur_t *cur) {
    uint64_t epc;
    uint64_t ci;
    uint64_t hops = 0;

    if (idx >= 65536) return -1;
    if (is_root && fs->fat_bits != 32) {
        if (idx * FAT_ENTRY_SIZE >=
            (uint64_t)fs->root_sectors * fs->bps) return -1;
        *lba = (uint64_t)fs->reserved + (uint64_t)fs->fats * fs->fat_sectors +
               idx * FAT_ENTRY_SIZE / fs->bps;
        *off = (uint32_t)(idx * FAT_ENTRY_SIZE % fs->bps);
        return 0;
    }
    epc = (uint64_t)fs->bps * fs->spc / FAT_ENTRY_SIZE;
    ci = idx / epc;
    if (cur && cur->c && cur->ci <= ci) {
        dir_cluster = cur->c;
        hops = cur->ci;
    }
    for (; hops < ci; hops++) {
        dir_cluster = fat_next_cluster(fs, dir_cluster);
        if (dir_cluster < 2 || dir_cluster >= fs->cluster_count + 2) return -1;
    }
    if (dir_cluster < 2 || dir_cluster >= fs->cluster_count + 2) return -1;
    if (cur) {
        cur->c = dir_cluster;
        cur->ci = ci;
    }
    *lba = fat_cluster_lba(fs, dir_cluster) + idx % epc * FAT_ENTRY_SIZE / fs->bps;
    *off = (uint32_t)(idx * FAT_ENTRY_SIZE % fs->bps);
    return 0;
}

int fat_read_entry(fat_fs_t *fs, uint64_t lba, uint32_t off,
                   fat_entry_t *out) {
    if (fs->dbuf_lba != lba) {
        fs->dbuf_lba = ~0ULL;
        if (fat_read_lba(fs, lba, fs->dbuf) != 0) return -1;
        fs->dbuf_lba = lba;
    }
    memcpy(out->raw, fs->dbuf + off, FAT_ENTRY_SIZE);
    return 0;
}

int fat_dir_emit(fat_fs_t *fs, uint64_t entry_lba, uint32_t entry_off,
                 const fat_entry_t *entry) {
    fat_entry_t old;

    if (fat_read_entry(fs, entry_lba, entry_off, &old) != 0) return -1;
    memcpy(fs->dbuf + entry_off, entry->raw, FAT_ENTRY_SIZE);
    if (fat_write_lba(fs, entry_lba, fs->dbuf) != 0) return -1;
    fs->dbuf_lba = entry_lba;
    return 0;
}

static const uint8_t fat_lfn_pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static uint8_t fat_lfn_sum(const uint8_t *sfn) {
    uint8_t s = 0;
    int i;

    for (i = 0; i < FAT_SFN_LEN; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sfn[i]);
    return s;
}

static int fat_sfn_u16(const uint8_t *r, uint16_t *o) {
    uint8_t c;
    int n = 0;
    int i;

    for (i = 0; i < FAT_SFN_LEN; i++) {
        c = i ? r[i] : r[0] == 0x05 ? 0xE5 : r[0];
        if (i == 8 && c != ' ') {
            if (o) o[n] = '.';
            n++;
        }
        if (c == ' ') {
            if (i < 8) i = 7;
            continue;
        }
        if (c >= 'A' && c <= 'Z' && (r[12] & (i < 8 ? 0x08 : 0x10))) c += 32;
        if (o) o[n] = c;
        n++;
    }
    return n;
}

int fat_dir_next(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                 uint64_t *idx, fat_found_t *f, uint16_t *out) {
    uint8_t *r = f->e.raw;
    fat_cur_t cur = {0, 0};
    uint64_t i;
    uint32_t ord = 0;
    uint32_t len = 0;
    uint32_t k;
    uint32_t j;
    uint32_t p;
    uint16_t c;
    uint8_t sum = 0;

    for (i = *idx;; i++) {
        if (fat_dir_slot(fs, dir_cluster, is_root, i, &f->lba, &f->off,
                         &cur) != 0)
            return 1;
        if (fat_read_entry(fs, f->lba, f->off, &f->e) != 0) return -1;
        if (!r[0]) return 1;
        if (r[0] == 0xE5) {
            ord = 0;
            continue;
        }
        if ((r[11] & 0x3F) == FAT_ATTR_LFN) {
            k = r[0] & 0x1F;
            if (r[0] & 0x40) {
                if (!k || k > 20) {
                    ord = 0;
                    continue;
                }
                len = k * 13;
                sum = r[13];
                f->start = i;
            } else if (!ord || k != ord - 1 || r[13] != sum) {
                ord = 0;
                continue;
            }
            ord = k;
            for (j = 0; j < 13; j++) {
                p = (k - 1) * 13 + j;
                c = fat_get16(r + fat_lfn_pos[j]);
                if (p >= len) break;
                if (!c) {
                    len = p;
                    break;
                }
                if (out) out[p] = c;
            }
            continue;
        }
        if ((r[11] & FAT_ATTR_VOLUME) ||
            (r[0] == '.' && (r[1] == ' ' || (r[1] == '.' && r[2] == ' ')))) {
            ord = 0;
            continue;
        }
        if (ord == 1 && len && len <= FSNAME_MAX && fat_lfn_sum(r) == sum) {
            f->len = (uint16_t)len;
        } else {
            f->start = i;
            f->len = (uint16_t)fat_sfn_u16(r, out);
        }
        f->idx = i;
        *idx = i + 1;
        return 0;
    }
}

static int fat_ieq(const char *a, const char *b) {
    char x;
    char y;

    do {
        x = *a++;
        y = *b++;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    } while (x);
    return 1;
}

int fat_dir_lookup(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   const char *name, fat_found_t *f) {
    uint8_t sfn[FAT_SFN_LEN];
    uint64_t i = 0;
    int s = fat_to_sfn(name, sfn) == 0;
    int r;

    while (!(r = fat_dir_next(fs, dir_cluster, is_root, &i, f, fs->name))) {
        if (s && !memcmp(f->e.raw, sfn, FAT_SFN_LEN)) return 0;
        fsname_to_utf8(fs->name, f->len, fs->u8);
        if (fat_ieq(fs->u8, name)) return 0;
    }
    return r;
}

static int fat_make_sfn(const uint16_t *nm, int len, uint8_t *sfn,
                        uint8_t *nt) {
    int cs[2] = {0, 0};
    int dot = -1;
    int lossy = 0;
    int bl = 0;
    int el = 0;
    int i = 0;
    int k;
    int e;
    uint16_t c;

    memset(sfn, ' ', FAT_SFN_LEN);
    while (i < len && nm[i] == '.') {
        i++;
        lossy = 1;
    }
    for (k = i; k < len; k++)
        if (nm[k] == '.') dot = k;
    for (k = i; k < len; k++) {
        if (k == dot) continue;
        c = nm[k];
        e = dot >= 0 && k > dot;
        if (c == ' ' || c == '.') {
            lossy = 1;
            continue;
        }
        if (c >= 0x80 || strchr("+,;=[]", c)) {
            c = '_';
            lossy = 1;
        } else if (c >= 'a' && c <= 'z') {
            c -= 32;
            cs[e] |= 2;
        } else if (c >= 'A' && c <= 'Z') {
            cs[e] |= 1;
        }
        if (e ? el < 3 : bl < 8) sfn[e ? 8 + el++ : bl++] = (uint8_t)c;
        else lossy = 1;
    }
    if (!bl) {
        sfn[0] = '_';
        lossy = 1;
    }
    if (cs[0] == 3 || cs[1] == 3) lossy = 1;
    *nt = lossy ? 0 : (uint8_t)((cs[0] == 2 ? 0x08 : 0) | (cs[1] == 2 ? 0x10 : 0));
    return lossy;
}

static void fat_tail(uint8_t *sfn, const uint8_t *basis, uint32_t n,
                     uint16_t hash) {
    char d[8];
    int dl = 0;
    int bl = 0;
    int p;

    memcpy(sfn, basis, FAT_SFN_LEN);
    while (bl < 8 && basis[bl] != ' ') bl++;
    if (n > 4) {
        if (bl > 2) bl = 2;
        hash = (uint16_t)(hash + n);
        for (p = 0; p < 4; p++)
            sfn[bl++] = "0123456789ABCDEF"[hash >> (12 - p * 4) & 15];
        n = 1;
    }
    do d[dl++] = (char)('0' + n % 10); while (n /= 10);
    p = bl < 7 - dl ? bl : 7 - dl;
    sfn[p++] = '~';
    while (dl) sfn[p++] = (uint8_t)d[--dl];
    while (p < 8) sfn[p++] = ' ';
}

static int fat_sfn_used(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                        const uint8_t *sfn) {
    fat_found_t f;
    uint64_t i = 0;
    int r;

    while (!(r = fat_dir_next(fs, dir_cluster, is_root, &i, &f, NULL)))
        if (!memcmp(f.e.raw, sfn, FAT_SFN_LEN)) return 1;
    return r < 0 ? -1 : 0;
}

int fat_dir_create(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   const char *name, const uint8_t *tmpl,
                   uint64_t *entry_lba, uint32_t *entry_off) {
    fat_entry_t e;
    fat_cur_t cur = {0, 0};
    uint8_t basis[FAT_SFN_LEN];
    uint8_t sfn[FAT_SFN_LEN];
    uint8_t nt;
    uint8_t sum;
    uint16_t hash = 0;
    uint16_t ch;
    uint64_t i = 0;
    uint64_t lba = 0;
    uint32_t off = 0;
    uint32_t n;
    uint32_t k;
    uint32_t j;
    uint32_t c;
    uint32_t run = 0;
    int len;
    int r;

    if ((len = fsname_to_utf16(name, fs->name)) <= 0) return -1;
    if (fat_make_sfn(fs->name, len, basis, &nt)) {
        for (k = 0; k < (uint32_t)len; k++) hash = (uint16_t)(hash * 31 + fs->name[k]);
        for (k = 1;; k++) {
            if (k > 0x10004) return -17;
            fat_tail(sfn, basis, k, hash);
            if ((r = fat_sfn_used(fs, dir_cluster, is_root, sfn)) < 0) return -1;
            if (!r) break;
        }
        n = (uint32_t)(len + 12) / 13 + 1;
    } else {
        memcpy(sfn, basis, FAT_SFN_LEN);
        n = 1;
    }
    while (run < n) {
        if (fat_dir_slot(fs, dir_cluster, is_root, i, &lba, &off, &cur) != 0) {
            if ((is_root && fs->fat_bits != 32) || i >= 65536) return -28;
            for (c = dir_cluster;; c = j) {
                j = fat_next_cluster(fs, c);
                if (j < 2 || j >= fs->cluster_count + 2) break;
            }
            if (fat_alloc_cluster(fs, c) < 2) return -28;
            continue;
        }
        if (fat_read_entry(fs, lba, off, &e) != 0) return -1;
        run = !e.raw[0] || e.raw[0] == 0xE5 ? run + 1 : 0;
        i++;
    }
    i -= n;
    sum = fat_lfn_sum(sfn);
    for (k = 0; k < n; k++) {
        if (fat_dir_slot(fs, dir_cluster, is_root, i + k, &lba, &off, &cur) != 0)
            return -1;
        memset(e.raw, 0, FAT_ENTRY_SIZE);
        if (k + 1 == n) {
            memcpy(e.raw, tmpl, FAT_ENTRY_SIZE);
            memcpy(e.raw, sfn, FAT_SFN_LEN);
            e.raw[12] = nt;
        } else {
            e.raw[0] = (uint8_t)((n - 1 - k) | (k ? 0 : 0x40));
            e.raw[11] = FAT_ATTR_LFN;
            e.raw[13] = sum;
            for (j = 0; j < 13; j++) {
                c = (n - 2 - k) * 13 + j;
                ch = c < (uint32_t)len ? fs->name[c] : c == (uint32_t)len ? 0 : 0xFFFF;
                e.raw[fat_lfn_pos[j]] = (uint8_t)ch;
                e.raw[fat_lfn_pos[j] + 1] = (uint8_t)(ch >> 8);
            }
        }
        if (fat_dir_emit(fs, lba, off, &e) != 0) return -1;
    }
    if (entry_lba) *entry_lba = lba;
    if (entry_off) *entry_off = off;
    return 0;
}

int fat_dir_remove(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   const fat_found_t *f) {
    fat_entry_t e;
    fat_cur_t cur = {0, 0};
    uint64_t i;
    uint64_t lba;
    uint32_t off;

    for (i = f->start; i <= f->idx; i++) {
        if (fat_dir_slot(fs, dir_cluster, is_root, i, &lba, &off, &cur) != 0 ||
            fat_read_entry(fs, lba, off, &e) != 0)
            return -1;
        e.raw[0] = 0xE5;
        if (fat_dir_emit(fs, lba, off, &e) != 0) return -1;
    }
    return 0;
}

int fat_mount_fs(fat_fs_t *fs, uint64_t dev, uint64_t part_lba) {
    uint8_t *boot;
    uint32_t bps;
    uint32_t fat_sz;
    uint32_t total;
    uint32_t root_entries;

    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->part_lba = part_lba;
    boot = (uint8_t *)kmalloc(4096);
    if (!boot) return -1;
    if (blkdev_read(blkdev_get(dev), part_lba, 1, boot) != 0) {
        kfree(boot);
        return -1;
    }
    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        kfree(boot);
        return -1;
    }
    bps = fat_get16(boot + 11);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) {
        kfree(boot);
        return -1;
    }
    fs->bps = bps;
    fs->spc = boot[13];
    if (!fs->spc || (fs->spc & (fs->spc - 1))) {
        kfree(boot);
        return -1;
    }
    fs->reserved = fat_get16(boot + 14);
    fs->fats = boot[16];
    if (!fs->fats || !fs->reserved) {
        kfree(boot);
        return -1;
    }
    if (boot[21] != 0xF0 && boot[21] < 0xF8) {
        kfree(boot);
        return -1;
    }
    if ((uint64_t)fs->spc * bps > 32768) {
        kfree(boot);
        return -1;
    }
    root_entries = fat_get16(boot + 17);
    total = fat_get16(boot + 19);
    if (!total) total = fat_get32(boot + 32);
    if (!total) {
        kfree(boot);
        return -1;
    }
    fs->total_sectors = total;
    fat_sz = fat_get16(boot + 22);
    if (!fat_sz) fat_sz = fat_get32(boot + 36);
    if (!fat_sz) {
        kfree(boot);
        return -1;
    }
    fs->fat_sectors = fat_sz;
    fs->root_sectors = (root_entries * 32 + bps - 1) / bps;
    if (fs->root_sectors) {
        fs->data_start = fs->reserved + fs->fats * fat_sz + fs->root_sectors;
    } else {
        fs->data_start = fs->reserved + fs->fats * fat_sz;
        fs->root_cluster = fat_get32(boot + 44);
        if (fs->root_cluster < 2) {
            kfree(boot);
            return -1;
        }
    }
    if (fs->data_start >= total) {
        kfree(boot);
        return -1;
    }
    fs->cluster_count = (total - fs->data_start) / fs->spc;
    if (fs->cluster_count < 4085) fs->fat_bits = 12;
    else if (fs->cluster_count < 65525) fs->fat_bits = 16;
    else fs->fat_bits = 32;
    if (fs->fat_bits != 32 && !root_entries) {
        kfree(boot);
        return -1;
    }
    if ((uint64_t)fs->fat_sectors * bps * 8 / fs->fat_bits <
        fs->cluster_count + 2) {
        kfree(boot);
        return -1;
    }
    fs->free_count = UINT32_MAX;
    if (fs->fat_bits == 32 && fat_get16(boot + 48) < fs->reserved)
        fs->fsinfo = fat_get16(boot + 48);
    fs->fbuf_lba = ~0ULL;
    fs->dbuf_lba = ~0ULL;
    fs->scratch = (uint8_t *)kmalloc((uint64_t)bps * 2);
    fs->fbuf = (uint8_t *)kmalloc((uint64_t)bps * 2);
    fs->dbuf = (uint8_t *)kmalloc(bps);
    kfree(boot);
    if (!fs->scratch || !fs->fbuf || !fs->dbuf) {
        fat_unmount_fs(fs);
        return -1;
    }
    mutex_init(&fs->lock);
    return 0;
}

void fat_unmount_fs(fat_fs_t *fs) {
    kfree(fs->scratch);
    kfree(fs->fbuf);
    kfree(fs->dbuf);
    fs->scratch = NULL;
    fs->fbuf = NULL;
    fs->dbuf = NULL;
}

uint64_t fat_unix(uint16_t date, uint16_t time) {
    int64_t m = date >> 5 & 15;
    int64_t y = (date >> 9) + 1980 - (m <= 2);
    int64_t era = y / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + (date & 31) - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;

    if (!m || !(date & 31) || days < 0) return 0;
    return (uint64_t)days * 86400 + (time >> 11) * 3600 +
           (time >> 5 & 63) * 60 + (time & 31) * 2;
}

void fat_dos_time(uint64_t unix, uint16_t *date, uint16_t *time) {
    uint64_t days;
    uint64_t rem;
    int64_t z;
    int64_t era;
    int64_t doe;
    int64_t yoe;
    int64_t y;
    int64_t doy;
    int64_t mp;
    int64_t d;
    int64_t m;
    uint64_t hh;
    uint64_t mm;
    uint64_t ss;

    days = unix / 86400;
    rem = unix % 86400;
    z = (int64_t)days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    if (y < 1980) {
        y = 1980;
        m = 1;
        d = 1;
    }
    if (y > 2107) {
        y = 2107;
        m = 12;
        d = 31;
    }
    hh = rem / 3600;
    mm = (rem % 3600) / 60;
    ss = rem % 60;
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)((hh << 11) | (mm << 5) | (ss / 2));
}

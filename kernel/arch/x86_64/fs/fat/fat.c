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

int fat_read_lba(fat_fs_t *fs, uint64_t lba, uint8_t *buf) {
    uint64_t sectors;
    uint64_t i;

    sectors = (fs->bps + 511) / 512;
    for (i = 0; i < sectors; i++) {
        if (ahci_read_sectors(fs->port, fs->part_lba + lba * sectors + i,
                              1, buf + i * 512) != 0) return -1;
    }
    return 0;
}

int fat_read_lba_range(fat_fs_t *fs, uint64_t lba, uint64_t count,
                       uint8_t *buf) {
    uint64_t sectors;

    if (!count) return 0;
    sectors = (fs->bps + 511) / 512;
    return ahci_read_sectors(fs->port, fs->part_lba + lba * sectors,
                             count * sectors, buf);
}

int fat_write_lba(fat_fs_t *fs, uint64_t lba, const uint8_t *buf) {
    uint64_t sectors;
    uint64_t i;

    sectors = (fs->bps + 511) / 512;
    for (i = 0; i < sectors; i++) {
        if (ahci_write_sectors(fs->port, fs->part_lba + lba * sectors + i,
                               1, buf + i * 512) != 0) return -1;
    }
    return 0;
}

int fat_write_lba_range(fat_fs_t *fs, uint64_t lba, uint64_t count,
                        const uint8_t *buf) {
    uint64_t sectors;

    if (!count) return 0;
    sectors = (fs->bps + 511) / 512;
    return ahci_write_sectors(fs->port, fs->part_lba + lba * sectors,
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
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint8_t b3;

    if (cluster < 2 || cluster >= fs->cluster_count + 2) return 1;
    lba = fat_table_lba(fs, cluster, &off);
    if (fat_read_lba(fs, lba, fs->scratch) != 0) return 1;
    b0 = fs->scratch[off];
    if (off + 1 >= fs->bps) {
        if (fat_read_lba(fs, lba + 1, fs->scratch + fs->bps) != 0) return 1;
        b1 = fs->scratch[fs->bps];
        b2 = fs->scratch[fs->bps + 1];
        b3 = fs->scratch[fs->bps + 2];
    } else if (off + 3 >= fs->bps && fs->fat_bits == 32) {
        if (fat_read_lba(fs, lba + 1, fs->scratch + fs->bps) != 0) return 1;
        b1 = fs->scratch[off + 1];
        b2 = fs->scratch[fs->bps];
        b3 = fs->scratch[fs->bps + 1];
    } else {
        b1 = fs->scratch[off + 1];
        b2 = fs->scratch[off + 2];
        b3 = fs->scratch[off + 3];
    }
    if (fs->fat_bits == 32)
        return ((uint32_t)b0 | ((uint32_t)b1 << 8) |
                ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24)) & 0x0FFFFFFF;
    if (fs->fat_bits == 16)
        return (uint32_t)b0 | ((uint32_t)b1 << 8);
    if (cluster & 1)
        return ((uint32_t)b0 >> 4) | ((uint32_t)b1 << 4);
    return (uint32_t)b0 | (((uint32_t)b1 & 0x0F) << 8);
}

static int fat_patch_entry(fat_fs_t *fs, uint8_t *sec, uint32_t cluster,
                           uint32_t off, uint32_t value) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) return -1;
    if (fs->fat_bits == 32) {
        uint32_t old;
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
        uint8_t b0;
        uint8_t b1;
        if (off + 2 > fs->bps) return -1;
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
    uint32_t off;
    uint8_t f;

    if (cluster < 2 || cluster >= fs->cluster_count + 2) return -1;
    lba = fat_table_lba(fs, cluster, &off);
    for (f = 0; f < fs->fats; f++) {
        uint64_t flba = lba + (uint64_t)f * fs->fat_sectors;
        if (fat_read_lba(fs, flba, fs->scratch) != 0) return -1;
        if (fat_patch_entry(fs, fs->scratch, cluster, off, value) != 0)
            return -1;
        if (fat_write_lba(fs, flba, fs->scratch) != 0) return -1;
    }
    return 0;
}

int fat_chain_append(fat_fs_t *fs, uint32_t prev, uint32_t next) {
    uint64_t lba_n;
    uint64_t lba_p;
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
    for (f = 0; f < fs->fats; f++) {
        uint64_t flba_n = lba_n + (uint64_t)f * fs->fat_sectors;
        uint64_t flba_p = lba_p + (uint64_t)f * fs->fat_sectors;
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
    memset(fs->scratch, 0, fs->bps);
    for (i = 0; i < fs->spc; i++) {
        if (fat_write_lba(fs, lba + i, fs->scratch) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_alloc_inner(fat_fs_t *fs, uint32_t prev, int zero) {
    uint32_t c;
    uint64_t checked = 0;
    uint64_t total;

    total = fs->cluster_count;
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
        fat_set_next(fs, c, 0);
        if (fat_is_eoc(fs, next)) break;
        if (next < 2 || next >= fs->cluster_count + 2) break;
        if (++hops > fs->cluster_count + 1) break;
        c = next;
    }
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

int fat_dir_slot(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   uint64_t idx, uint64_t *lba, uint32_t *off) {
    uint64_t epc;
    uint64_t hops = 0;

    if (is_root && fs->fat_bits != 32) {
        if (idx * FAT_ENTRY_SIZE >=
            (uint64_t)fs->root_sectors * fs->bps) return -1;
        *lba = (uint64_t)fs->reserved + (uint64_t)fs->fats * fs->fat_sectors +
               idx * FAT_ENTRY_SIZE / fs->bps;
        *off = (uint32_t)(idx * FAT_ENTRY_SIZE % fs->bps);
        return 0;
    }
    epc = (uint64_t)fs->bps * fs->spc / FAT_ENTRY_SIZE;
    if (!epc) return -1;
    while (idx >= epc) {
        dir_cluster = fat_next_cluster(fs, dir_cluster);
        if (dir_cluster < 2 ||
            dir_cluster >= fs->cluster_count + 2) return -1;
        idx -= epc;
        if (++hops > fs->cluster_count + 1) return -1;
    }
    *lba = fat_cluster_lba(fs, dir_cluster) +
           idx * FAT_ENTRY_SIZE / fs->bps;
    *off = (uint32_t)(idx * FAT_ENTRY_SIZE % fs->bps);
    return 0;
}

int fat_read_entry(fat_fs_t *fs, uint64_t lba, uint32_t off,
                     fat_entry_t *out) {
    if (fat_read_lba(fs, lba, fs->scratch) != 0) return -1;
    memcpy(out->raw, fs->scratch + off, FAT_ENTRY_SIZE);
    return 0;
}

int fat_dir_emit(fat_fs_t *fs, uint64_t entry_lba, uint32_t entry_off,
                 const fat_entry_t *entry) {
    if (fat_read_lba(fs, entry_lba, fs->scratch) != 0) return -1;
    memcpy(fs->scratch + entry_off, entry->raw, FAT_ENTRY_SIZE);
    if (fat_write_lba(fs, entry_lba, fs->scratch) != 0) return -1;
    return 0;
}

int fat_dir_find(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                 const uint8_t sfn[FAT_SFN_LEN], fat_entry_t *out,
                 uint64_t *entry_lba, uint32_t *entry_off) {
    uint64_t idx = 0;
    uint64_t lba;
    uint32_t off;
    uint8_t attr;

    for (;; idx++) {
        if (fat_dir_slot(fs, dir_cluster, is_root, idx, &lba, &off) != 0)
            return -1;
        if (fat_read_entry(fs, lba, off, out) != 0) return -1;
        if (out->raw[0] == 0x00) return -1;
        if (out->raw[0] == 0xE5) continue;
        attr = out->raw[11];
        if (attr == FAT_ATTR_LFN) continue;
        if (attr & FAT_ATTR_VOLUME) continue;
        if (memcmp(out->raw, sfn, FAT_SFN_LEN) == 0) {
            if (entry_lba) *entry_lba = lba;
            if (entry_off) *entry_off = off;
            return 0;
        }
    }
}

int fat_dir_add(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                const uint8_t sfn[FAT_SFN_LEN], uint8_t attr,
                uint32_t first_cluster, uint16_t fdate, uint16_t ftime,
                uint64_t *entry_lba, uint32_t *entry_off) {
    fat_entry_t entry;
    uint64_t idx = 0;
    uint64_t lba = 0;
    uint32_t off = 0;
    uint32_t c = dir_cluster;
    uint64_t epc;
    uint64_t i;

    if (is_root && fs->fat_bits != 32) {
        for (;; idx++) {
            if (fat_dir_slot(fs, 0, 1, idx, &lba, &off) != 0) return -1;
            if (fat_read_entry(fs, lba, off, &entry) != 0) return -1;
            if (entry.raw[0] == 0x00 || entry.raw[0] == 0xE5) break;
        }
    } else {
        epc = (uint64_t)fs->bps * fs->spc / FAT_ENTRY_SIZE;
        if (!epc) return -1;
        for (;;) {
            if (idx / epc > fs->cluster_count + 1) return -1;
            for (i = 0; i < epc; i++, idx++) {
                lba = fat_cluster_lba(fs, c) +
                      (idx % epc) * FAT_ENTRY_SIZE / fs->bps;
                off = (uint32_t)(idx * FAT_ENTRY_SIZE % fs->bps);
                if (fat_read_entry(fs, lba, off, &entry) != 0) return -1;
                if (entry.raw[0] == 0x00 || entry.raw[0] == 0xE5) goto found;
            }
            {
                uint32_t next = fat_next_cluster(fs, c);
                if (next >= 2 && next < fs->cluster_count + 2) {
                    c = next;
                } else {
                    c = fat_alloc_cluster(fs, c);
                    if (c < 2) return -1;
                }
            }
        }
    }
found:
    memset(entry.raw, 0, FAT_ENTRY_SIZE);
    memcpy(entry.raw, sfn, FAT_SFN_LEN);
    entry.raw[11] = attr;
    entry.raw[14] = (uint8_t)(ftime & 0xFF);
    entry.raw[15] = (uint8_t)((ftime >> 8) & 0xFF);
    entry.raw[16] = (uint8_t)(fdate & 0xFF);
    entry.raw[17] = (uint8_t)((fdate >> 8) & 0xFF);
    entry.raw[18] = (uint8_t)(fdate & 0xFF);
    entry.raw[22] = (uint8_t)(ftime & 0xFF);
    entry.raw[23] = (uint8_t)((ftime >> 8) & 0xFF);
    entry.raw[24] = (uint8_t)(fdate & 0xFF);
    entry.raw[25] = (uint8_t)((fdate >> 8) & 0xFF);
    entry.raw[26] = (uint8_t)(first_cluster & 0xFF);
    entry.raw[27] = (uint8_t)((first_cluster >> 8) & 0xFF);
    entry.raw[20] = (uint8_t)((first_cluster >> 16) & 0xFF);
    entry.raw[21] = (uint8_t)((first_cluster >> 24) & 0xFF);
    if (fat_dir_emit(fs, lba, off, &entry) != 0) return -1;
    if (entry_lba) *entry_lba = lba;
    if (entry_off) *entry_off = off;
    return 0;
}

int fat_mount_fs(fat_fs_t *fs, ahci_port_t *port, uint64_t part_lba) {
    uint8_t *boot;
    uint32_t bps;
    uint32_t fat_sz;
    uint32_t total;
    uint32_t root_entries;

    memset(fs, 0, sizeof(*fs));
    fs->port = port;
    fs->part_lba = part_lba;
    boot = (uint8_t *)kmalloc(4096);
    if (!boot) return -1;
    if (ahci_read_sectors(port, part_lba, 1, boot) != 0) {
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
    fs->scratch = (uint8_t *)kmalloc((uint64_t)bps * 2);
    kfree(boot);
    if (!fs->scratch) return -1;
    mutex_init(&fs->lock);
    return 0;
}

void fat_unmount_fs(fat_fs_t *fs) {
    if (fs->scratch) {
        kfree(fs->scratch);
        fs->scratch = NULL;
    }
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

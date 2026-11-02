#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <lebirun/blkdev.h>
#include <lebirun/mutex.h>
#include <lebirun/fsname.h>

#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME    0x08
#define FAT_ATTR_DIR       0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

#define FAT_ENTRY_SIZE     32
#define FAT_SFN_LEN        11

typedef struct {
    uint64_t dev;
    uint64_t part_lba;
    uint32_t bps;
    uint8_t spc;
    uint16_t reserved;
    uint8_t fats;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t data_start;
    uint32_t root_cluster;
    uint32_t root_sectors;
    uint32_t cluster_count;
    uint8_t fat_bits;
    uint32_t alloc_hint;
    uint32_t free_count;
    uint16_t fsinfo;
    uint8_t fsinfo_dirty;
    uint8_t *scratch;
    uint8_t *fbuf;
    uint64_t fbuf_lba;
    uint8_t *dbuf;
    uint64_t dbuf_lba;
    mutex_t lock;
    uint16_t name[260];
    char u8[FSNAME_MAX * 3 + 1];
} fat_fs_t;

typedef struct {
    uint8_t raw[FAT_ENTRY_SIZE];
} fat_entry_t;

typedef struct {
    uint32_t c;
    uint64_t ci;
} fat_cur_t;

typedef struct {
    fat_entry_t e;
    uint64_t start;
    uint64_t idx;
    uint64_t lba;
    uint32_t off;
    uint16_t len;
} fat_found_t;

int fat_mount_fs(fat_fs_t *fs, uint64_t dev, uint64_t part_lba);
void fat_unmount_fs(fat_fs_t *fs);
uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster);
int fat_set_next(fat_fs_t *fs, uint32_t cluster, uint32_t value);
int fat_chain_append(fat_fs_t *fs, uint32_t prev, uint32_t next);
uint32_t fat_alloc_cluster(fat_fs_t *fs, uint32_t prev);
uint32_t fat_alloc_cluster_nz(fat_fs_t *fs, uint32_t prev);
void fat_free_chain(fat_fs_t *fs, uint32_t start);
uint32_t fat_count_free(fat_fs_t *fs);
int fat_sync_fsinfo(fat_fs_t *fs);
uint64_t fat_cluster_lba(fat_fs_t *fs, uint32_t cluster);
int fat_read_lba(fat_fs_t *fs, uint64_t lba, uint8_t *buf);
int fat_read_lba_range(fat_fs_t *fs, uint64_t lba, uint64_t count,
                       uint8_t *buf);
int fat_write_lba(fat_fs_t *fs, uint64_t lba, const uint8_t *buf);
int fat_write_lba_range(fat_fs_t *fs, uint64_t lba, uint64_t count,
                        const uint8_t *buf);
int fat_to_sfn(const char *name, uint8_t sfn[FAT_SFN_LEN]);
int fat_dir_next(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                 uint64_t *idx, fat_found_t *f, uint16_t *out);
int fat_dir_lookup(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   const char *name, fat_found_t *f);
int fat_dir_create(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   const char *name, const uint8_t *tmpl,
                   uint64_t *entry_lba, uint32_t *entry_off);
int fat_dir_remove(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                   const fat_found_t *f);
void fat_dos_time(uint64_t unix, uint16_t *date, uint16_t *time);
uint64_t fat_unix(uint16_t date, uint16_t time);
int fat_dir_emit(fat_fs_t *fs, uint64_t entry_lba, uint32_t entry_off,
                 const fat_entry_t *entry);
int fat_dir_slot(fat_fs_t *fs, uint32_t dir_cluster, int is_root,
                 uint64_t idx, uint64_t *lba, uint32_t *off, fat_cur_t *cur);
int fat_read_entry(fat_fs_t *fs, uint64_t lba, uint32_t off,
                   fat_entry_t *out);
uint64_t fat_file_size(const fat_entry_t *entry);
uint32_t fat_file_cluster(const fat_entry_t *entry);
void fat_set_cluster(uint8_t *raw, uint32_t cluster);
int fat_is_dir(const fat_entry_t *entry);

#endif

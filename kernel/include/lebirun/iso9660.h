#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/vfs.h>

#define ISO9660_SECTOR_SIZE       2048
#define ISO9660_SYSTEM_AREA_SIZE  (16 * ISO9660_SECTOR_SIZE)

#define ISO9660_VD_PRIMARY   1
#define ISO9660_VD_TERMINATOR 255

#define ISO9660_DE_FLAG_HIDDEN     0x01
#define ISO9660_DE_FLAG_DIRECTORY  0x02
#define ISO9660_DE_FLAG_ASSOC      0x04
#define ISO9660_DE_FLAG_RECORD     0x08
#define ISO9660_DE_FLAG_PROTECTION 0x10
#define ISO9660_DE_FLAG_MULTIEXT   0x80

typedef struct {
    uint8_t  le[2];
    uint8_t  be[2];
} __attribute__((packed)) iso9660_u16_both_t;

typedef struct {
    uint8_t  le[4];
    uint8_t  be[4];
} __attribute__((packed)) iso9660_u32_both_t;

typedef struct {
    uint8_t  year[4];
    uint8_t  month[2];
    uint8_t  day[2];
    uint8_t  hour[2];
    uint8_t  minute[2];
    uint8_t  second[2];
    uint8_t  hundredths[2];
    int8_t   timezone;
} __attribute__((packed)) iso9660_datetime_t;

typedef struct {
    uint8_t  years;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    int8_t   timezone;
} __attribute__((packed)) iso9660_dir_datetime_t;

typedef struct {
    uint8_t           type;
    uint8_t           id[5];
    uint8_t           version;
    uint8_t           unused1;
    uint8_t           system_id[32];
    uint8_t           volume_id[32];
    uint8_t           unused2[8];
    iso9660_u32_both_t volume_space_size;
    uint8_t           unused3[32];
    uint8_t           volume_set_size[4];
    uint8_t           volume_seq_number[4];
    uint8_t           logical_block_size[4];
    iso9660_u32_both_t path_table_size;
    uint8_t           path_table_le[4];
    uint8_t           opt_path_table_le[4];
    uint8_t           path_table_be[4];
    uint8_t           opt_path_table_be[4];
    uint8_t           root_dir_entry[34];
    uint8_t           volume_set_id[128];
    uint8_t           publisher_id[128];
    uint8_t           preparer_id[128];
    uint8_t           application_id[128];
    uint8_t           copyright_file[37];
    uint8_t           abstract_file[37];
    uint8_t           bibliographic_file[37];
    iso9660_datetime_t creation_date;
    iso9660_datetime_t modification_date;
    iso9660_datetime_t expiration_date;
    iso9660_datetime_t effective_date;
    uint8_t           file_structure_version;
    uint8_t           unused4;
    uint8_t           app_use[512];
    uint8_t           reserved[653];
} __attribute__((packed)) iso9660_pvd_t;

typedef struct {
    uint8_t               length;
    uint8_t               ext_attr_length;
    iso9660_u32_both_t    extent_location;
    iso9660_u32_both_t    data_length;
    iso9660_dir_datetime_t recording_date;
    uint8_t               file_flags;
    uint8_t               file_unit_size;
    uint8_t               interleave_gap;
    uint8_t               volume_seq_number[4];
    uint8_t               name_length;
    uint8_t               name[1];
} __attribute__((packed)) iso9660_dirent_t;

typedef int (*iso9660_read_fn)(uint64_t lba, uint32_t count, void *buffer, void *priv);

typedef struct {
    uint8_t *base;
    uint64_t size;
    iso9660_pvd_t *pvd;
    uint32_t block_size;
    uint32_t root_extent;
    uint32_t root_length;
    iso9660_read_fn read_fn;
    void *read_priv;
} iso9660_context_t;

typedef struct {
    vfs_node_t vfs;
    uint32_t extent_lba;
    uint32_t data_length;
    iso9660_context_t *ctx;
} iso9660_vfs_node_t;

void iso9660_init(uint64_t mod_start, uint64_t mod_end);
int iso9660_init_device(iso9660_read_fn read_fn, void *priv);
void iso9660_vfs_register(void);
iso9660_context_t *iso9660_get_context(void);

#endif

#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>

#define MBR_SIGNATURE       0xAA55
#define MBR_PARTITION_COUNT 4
#define MBR_PART_OFFSET     446

#define GPT_SIGNATURE       0x5452415020494645ULL
#define GPT_HEADER_LBA      1
#define GPT_REVISION_1_0    0x00010000

#define PART_TYPE_EMPTY         0x00
#define PART_TYPE_FAT12         0x01
#define PART_TYPE_FAT16_SMALL   0x04
#define PART_TYPE_EXTENDED      0x05
#define PART_TYPE_FAT16_LARGE   0x06
#define PART_TYPE_NTFS          0x07
#define PART_TYPE_FAT32         0x0B
#define PART_TYPE_FAT32_LBA     0x0C
#define PART_TYPE_FAT16_LBA     0x0E
#define PART_TYPE_EXTENDED_LBA  0x0F
#define PART_TYPE_LINUX_SWAP    0x82
#define PART_TYPE_LINUX         0x83
#define PART_TYPE_LINUX_LVM     0x8E
#define PART_TYPE_GPT_PROTECT   0xEE

#define PARTITION_MAX           16

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_partition_entry_t;

typedef struct {
    uint8_t  bootstrap[446];
    mbr_partition_entry_t partitions[4];
    uint16_t signature;
} __attribute__((packed)) mbr_t;

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed)) gpt_partition_entry_t;

static const uint8_t GPT_TYPE_LINUX_FS[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static const uint8_t GPT_TYPE_LINUX_SWAP[16] = {
    0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
    0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F
};

static const uint8_t GPT_TYPE_EFI_SYSTEM[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static const uint8_t GPT_TYPE_EMPTY[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

typedef struct {
    int      valid;
    uint64_t port_index;
    int      part_number;
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t  mbr_type;
    uint8_t  gpt_type_guid[16];
    int      is_gpt;
} partition_info_t;

typedef struct {
    int             count;
    int             is_gpt;
    partition_info_t parts[PARTITION_MAX];
} partition_table_t;

int partition_scan(uint64_t port_index, partition_table_t *table);
int partition_scan_mbr(uint64_t port_index, partition_table_t *table);
int partition_scan_gpt(uint64_t port_index, partition_table_t *table);
const char *partition_type_name(uint8_t mbr_type);
int partition_is_guid_zero(const uint8_t *guid);
int partition_is_guid_equal(const uint8_t *a, const uint8_t *b);

#endif

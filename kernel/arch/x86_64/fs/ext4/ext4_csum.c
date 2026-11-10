#include <lebirun/fs/ext4/ext4.h>
#include <stddef.h>
#include <string.h>

static const uint32_t crc32c_nibble[16] = {
    0x00000000, 0x105EC76F, 0x20BD8EDE, 0x30E349B1, 0x417B1DBC, 0x5125DAD3,
    0x61C69362, 0x7198540D, 0x82F63B78, 0x92A8FC17, 0xA24BB5A6, 0xB21572C9,
    0xC38D26C4, 0xD3D3E1AB, 0xE330A81A, 0xF36E6F75
};

static const uint16_t crc16_nibble[16] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
};

static uint32_t crc32c(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc32c_nibble[crc & 15];
        crc = (crc >> 4) ^ crc32c_nibble[crc & 15];
    }
    return crc;
}

static uint16_t crc16(uint16_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    while (len--) {
        crc ^= *p++;
        crc = (uint16_t)((crc >> 4) ^ crc16_nibble[crc & 15]);
        crc = (uint16_t)((crc >> 4) ^ crc16_nibble[crc & 15]);
    }
    return crc;
}

static uint32_t inode_seed(ext4_fs_t *fs, uint32_t ino, uint32_t gen) {
    return crc32c(crc32c(fs->csum_seed, &ino, 4), &gen, 4);
}

void ext4_csum_init(ext4_fs_t *fs) {
    fs->metadata_csum = (fs->sb.s_feature_ro_compat &
                         EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    fs->gdt_csum = (fs->sb.s_feature_ro_compat &
                    EXT4_FEATURE_RO_COMPAT_GDT_CSUM) != 0;
    if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
        fs->csum_seed = fs->sb.s_checksum_seed;
    else
        fs->csum_seed = crc32c(~0u, fs->sb.s_uuid, sizeof(fs->sb.s_uuid));
}

void ext4_super_csum(ext4_fs_t *fs) {
    if (fs->metadata_csum)
        fs->sb.s_checksum = crc32c(~0u, &fs->sb,
                                   offsetof(ext4_superblock_t, s_checksum));
}

void ext4_desc_csum(ext4_fs_t *fs, uint32_t group, uint8_t *raw) {
    ext4_group_desc_t *desc = (ext4_group_desc_t *)raw;
    size_t off = offsetof(ext4_group_desc_t, bg_checksum);
    uint16_t crc;

    desc->bg_checksum = 0;
    if (fs->metadata_csum) {
        desc->bg_checksum = (uint16_t)crc32c(crc32c(fs->csum_seed, &group, 4),
                                             raw, fs->desc_size);
    } else if (fs->gdt_csum) {
        crc = crc16(0xFFFF, fs->sb.s_uuid, sizeof(fs->sb.s_uuid));
        crc = crc16(crc, &group, 4);
        crc = crc16(crc, raw, off);
        off += 2;
        if (fs->desc_size > off)
            crc = crc16(crc, raw + off, fs->desc_size - off);
        desc->bg_checksum = crc;
    }
}

void ext4_inode_csum(ext4_fs_t *fs, uint32_t ino, uint8_t *raw) {
    ext4_inode_t *inode = (ext4_inode_t *)raw;
    uint32_t crc;
    int hi;

    if (!fs->metadata_csum) return;
    hi = fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE && inode->i_extra_isize >= 4;
    inode->i_checksum_lo = 0;
    if (hi) inode->i_checksum_hi = 0;
    crc = crc32c(inode_seed(fs, ino, inode->i_generation), raw, fs->inode_size);
    inode->i_checksum_lo = (uint16_t)crc;
    if (hi) inode->i_checksum_hi = (uint16_t)(crc >> 16);
}

void ext4_bitmap_csum(ext4_fs_t *fs, ext4_group_desc_t *desc,
                      const uint8_t *bitmap, int inode) {
    uint32_t crc;

    if (!fs->metadata_csum) return;
    if (inode) {
        crc = crc32c(fs->csum_seed, bitmap, fs->sb.s_inodes_per_group / 8);
        desc->bg_inode_bitmap_csum_lo = (uint16_t)crc;
        if (fs->desc_size >= 64) desc->bg_inode_bitmap_csum_hi = (uint16_t)(crc >> 16);
    } else {
        crc = crc32c(fs->csum_seed, bitmap, fs->sb.s_clusters_per_group / 8);
        desc->bg_block_bitmap_csum_lo = (uint16_t)crc;
        if (fs->desc_size >= 64) desc->bg_block_bitmap_csum_hi = (uint16_t)(crc >> 16);
    }
}

uint32_t ext4_dir_tail_size(ext4_fs_t *fs, const uint8_t *block) {
    const ext4_dir_entry_t *tail;

    if (!fs->metadata_csum) return 0;
    tail = (const ext4_dir_entry_t *)(block + fs->block_size - EXT4_DIR_TAIL_SIZE);
    if (tail->inode || tail->rec_len != EXT4_DIR_TAIL_SIZE || tail->name_len ||
        tail->file_type != EXT4_DIR_TAIL_FT) return 0;
    return EXT4_DIR_TAIL_SIZE;
}

void ext4_dir_set_tail(ext4_fs_t *fs, uint8_t *block) {
    ext4_dir_entry_t *tail;

    if (!fs->metadata_csum) return;
    tail = (ext4_dir_entry_t *)(block + fs->block_size - EXT4_DIR_TAIL_SIZE);
    memset(tail, 0, EXT4_DIR_TAIL_SIZE);
    tail->rec_len = EXT4_DIR_TAIL_SIZE;
    tail->file_type = EXT4_DIR_TAIL_FT;
}

void ext4_dir_csum(ext4_fs_t *fs, uint32_t ino, const ext4_inode_t *dir,
                   uint8_t *block) {
    uint32_t crc;
    uint32_t size;

    if (!ext4_dir_tail_size(fs, block)) return;
    size = fs->block_size - EXT4_DIR_TAIL_SIZE;
    crc = crc32c(inode_seed(fs, ino, dir->i_generation), block, size);
    memcpy(block + size + 8, &crc, 4);
}

void ext4_extent_csum(ext4_fs_t *fs, uint32_t ino, const ext4_inode_t *inode,
                      uint8_t *block) {
    uint32_t off;
    uint32_t crc;

    if (!fs->metadata_csum) return;
    off = sizeof(ext4_extent_header_t) +
          ((ext4_extent_header_t *)block)->eh_max * sizeof(ext4_extent_t);
    if (off + 4 > fs->block_size) return;
    crc = crc32c(inode_seed(fs, ino, inode->i_generation), block, off);
    memcpy(block + off, &crc, 4);
}

uint32_t ext4_jsb_csum(const uint8_t *jsb) {
    static const uint8_t zero[4];

    return crc32c(crc32c(crc32c(~0u, jsb, 0xFC), zero, 4), jsb + 0x100, 1024 - 0x100);
}

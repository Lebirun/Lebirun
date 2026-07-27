#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <string.h>

#define JBD2_MAGIC 0xC03B3998u
#define JBD2_DESCRIPTOR_BLOCK 1u
#define JBD2_COMMIT_BLOCK 2u
#define JBD2_REVOKE_BLOCK 5u
#define JBD2_FLAG_ESCAPE 1u
#define JBD2_FLAG_SAME_UUID 2u
#define JBD2_FLAG_DELETED 4u
#define JBD2_FLAG_LAST_TAG 8u
#define JBD2_FEATURE_INCOMPAT_REVOKE 1u
#define JBD2_FEATURE_INCOMPAT_64BIT 2u
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT 4u
#define JBD2_FEATURE_INCOMPAT_CSUM_V2 8u
#define JBD2_FEATURE_INCOMPAT_CSUM_V3 16u
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT 32u

typedef struct {
    uint32_t maxlen;
    uint32_t first;
    uint32_t sequence;
    uint32_t start;
    uint32_t incompat;
    int has_64bit;
    int has_csum;
} ext4_journal_info_t;

static uint32_t journal_be32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | (uint32_t)value[3];
}

static void journal_put_be32(uint8_t *value, uint32_t number) {
    value[0] = (uint8_t)(number >> 24);
    value[1] = (uint8_t)(number >> 16);
    value[2] = (uint8_t)(number >> 8);
    value[3] = (uint8_t)number;
}

static uint32_t journal_next(ext4_journal_info_t *info, uint32_t block) {
    block++;
    if (block >= info->maxlen) block = info->first;
    return block;
}

static int journal_read(ext4_fs_t *fs, ext4_inode_t *inode,
                        uint32_t logical, uint8_t *buffer) {
    uint64_t physical;

    physical = ext4_inode_get_block(fs, inode, logical);
    if (physical == 0) return -1;
    return ext4_read_block(fs, physical, buffer);
}

static int journal_write(ext4_fs_t *fs, ext4_inode_t *inode,
                         uint32_t logical, uint8_t *buffer) {
    uint64_t physical;

    physical = ext4_inode_get_block(fs, inode, logical);
    if (physical == 0) return -1;
    return ext4_write_block(fs, physical, buffer);
}

static int journal_header(uint8_t *block, uint32_t *type,
                          uint32_t *sequence) {
    if (journal_be32(block) != JBD2_MAGIC) return -1;
    *type = journal_be32(block + 4);
    *sequence = journal_be32(block + 8);
    return 0;
}

static int journal_parse_tag(ext4_journal_info_t *info, uint8_t *block,
                             uint32_t block_size, uint32_t *offset,
                             uint64_t *home,
                             uint32_t *flags) {
    uint32_t position;
    uint32_t size;
    uint32_t low;
    uint32_t high;

    position = *offset;
    if (info->has_64bit) size = 16;
    else if (info->has_csum) size = 12;
    else size = 8;
    if (position > block_size - size) return -1;
    low = journal_be32(block + position);
    high = 0;
    if (info->has_64bit) {
        *flags = journal_be32(block + position + 4);
        high = journal_be32(block + position + 8);
    } else if (info->has_csum) {
        *flags = journal_be32(block + position + 4);
    } else {
        *flags = ((uint32_t)block[position + 6] << 8) |
                 block[position + 7];
    }
    position += size;
    if (!(*flags & JBD2_FLAG_SAME_UUID)) position += 16;
    if (position > block_size) return -1;
    *home = ((uint64_t)high << 32) | low;
    *offset = position;
    return 0;
}

static int journal_descriptor_count(ext4_fs_t *fs,
                                    ext4_journal_info_t *info,
                                    uint8_t *block, uint32_t *count) {
    uint32_t offset;
    uint32_t flags;
    uint64_t home;
    uint32_t tags;

    offset = 12;
    tags = 0;
    while (offset < fs->block_size) {
        if (journal_parse_tag(info, block, fs->block_size, &offset, &home,
                              &flags) != 0)
            return -1;
        if (home >= fs->total_blocks) return -1;
        if (!(flags & JBD2_FLAG_DELETED)) tags++;
        if (flags & JBD2_FLAG_LAST_TAG) {
            *count = tags;
            return 0;
        }
    }
    return -1;
}

static int journal_find_commit(ext4_fs_t *fs, ext4_inode_t *inode,
                               ext4_journal_info_t *info,
                               uint32_t start, uint32_t sequence,
                               uint32_t *commit, uint8_t *block) {
    uint32_t cursor;
    uint32_t type;
    uint32_t block_sequence;
    uint32_t count;
    uint32_t i;

    cursor = start;
    for (i = 0; i < info->maxlen; i++) {
        if (journal_read(fs, inode, cursor, block) != 0) return -1;
        if (journal_header(block, &type, &block_sequence) != 0 ||
            block_sequence != sequence) return -1;
        if (type == JBD2_COMMIT_BLOCK) {
            *commit = cursor;
            return 0;
        }
        if (type == JBD2_DESCRIPTOR_BLOCK) {
            if (journal_descriptor_count(fs, info, block, &count) != 0)
                return -1;
            cursor = journal_next(info, cursor);
            while (count > 0) {
                cursor = journal_next(info, cursor);
                count--;
            }
            continue;
        }
        if (type != JBD2_REVOKE_BLOCK) return -1;
        cursor = journal_next(info, cursor);
    }
    return -1;
}

static int journal_is_revoked(ext4_fs_t *fs, ext4_inode_t *inode,
                              ext4_journal_info_t *info, uint32_t start,
                              uint32_t commit, uint32_t sequence,
                              uint64_t home, uint8_t *block) {
    uint32_t cursor;
    uint32_t type;
    uint32_t block_sequence;
    uint32_t count;
    uint32_t offset;
    uint32_t step;
    uint32_t tags;
    uint64_t revoked;

    cursor = start;
    while (cursor != commit) {
        if (journal_read(fs, inode, cursor, block) != 0) return -1;
        if (journal_header(block, &type, &block_sequence) != 0 ||
            block_sequence != sequence) return -1;
        if (type == JBD2_REVOKE_BLOCK) {
            count = journal_be32(block + 12);
            step = info->has_64bit ? 8 : 4;
            if (count < 16 || count > fs->block_size) return -1;
            offset = 16;
            while (offset + step <= count) {
                if (info->has_64bit)
                    revoked = ((uint64_t)journal_be32(block + offset) << 32) |
                              journal_be32(block + offset + 4);
                else
                    revoked = journal_be32(block + offset);
                if (revoked == home) return 1;
                offset += step;
            }
            cursor = journal_next(info, cursor);
            continue;
        }
        if (type != JBD2_DESCRIPTOR_BLOCK) return -1;
        if (journal_descriptor_count(fs, info, block, &tags) != 0)
            return -1;
        cursor = journal_next(info, cursor);
        while (tags > 0) {
            cursor = journal_next(info, cursor);
            tags--;
        }
    }
    return 0;
}

static int journal_replay_transaction(ext4_fs_t *fs, ext4_inode_t *inode,
                                      ext4_journal_info_t *info,
                                      uint32_t start, uint32_t commit,
                                      uint32_t sequence, uint8_t *metadata,
                                      uint8_t *data, uint8_t *scan) {
    uint32_t cursor;
    uint32_t data_cursor;
    uint32_t type;
    uint32_t block_sequence;
    uint32_t offset;
    uint32_t flags;
    uint32_t revoke;
    uint64_t home;

    cursor = start;
    while (cursor != commit) {
        if (journal_read(fs, inode, cursor, metadata) != 0) return -1;
        if (journal_header(metadata, &type, &block_sequence) != 0 ||
            block_sequence != sequence) return -1;
        if (type == JBD2_REVOKE_BLOCK) {
            cursor = journal_next(info, cursor);
            continue;
        }
        if (type != JBD2_DESCRIPTOR_BLOCK) return -1;
        offset = 12;
        data_cursor = journal_next(info, cursor);
        for (;;) {
            if (journal_parse_tag(info, metadata, fs->block_size, &offset,
                                  &home,
                                  &flags) != 0) return -1;
            if (!(flags & JBD2_FLAG_DELETED)) {
                if (journal_read(fs, inode, data_cursor, data) != 0)
                    return -1;
                revoke = journal_is_revoked(fs, inode, info, start, commit,
                                            sequence, home, scan);
                if ((int)revoke < 0) return -1;
                if (!revoke) {
                    if (flags & JBD2_FLAG_ESCAPE)
                        journal_put_be32(data, JBD2_MAGIC);
                    if (ext4_write_block(fs, home, data) != 0) return -1;
                }
                data_cursor = journal_next(info, data_cursor);
            }
            if (flags & JBD2_FLAG_LAST_TAG) break;
        }
        cursor = data_cursor;
    }
    return 0;
}

int ext4_journal_replay(ext4_fs_t *fs) {
    ext4_inode_t journal_inode;
    ext4_journal_info_t info;
    uint8_t *superblock;
    uint8_t *metadata;
    uint8_t *data;
    uint8_t *scan;
    uint32_t cursor;
    uint32_t commit;
    uint32_t sequence;
    uint32_t transactions;
    uint32_t unsupported;
    int result;

    if (!fs || fs->sb.s_journal_inum == 0) return -1;
    if (ext4_read_inode(fs, fs->sb.s_journal_inum, &journal_inode) != 0)
        return -1;
    superblock = (uint8_t *)kmalloc(fs->block_size);
    metadata = (uint8_t *)kmalloc(fs->block_size);
    data = (uint8_t *)kmalloc(fs->block_size);
    scan = (uint8_t *)kmalloc(fs->block_size);
    if (!superblock || !metadata || !data || !scan) {
        if (superblock) kfree(superblock);
        if (metadata) kfree(metadata);
        if (data) kfree(data);
        if (scan) kfree(scan);
        return -1;
    }
    result = -1;
    if (journal_read(fs, &journal_inode, 0, superblock) != 0) goto done;
    if (journal_be32(superblock) != JBD2_MAGIC) goto done;
    if (journal_be32(superblock + 12) != fs->block_size) goto done;
    memset(&info, 0, sizeof(info));
    info.maxlen = journal_be32(superblock + 16);
    info.first = journal_be32(superblock + 20);
    info.sequence = journal_be32(superblock + 24);
    info.start = journal_be32(superblock + 28);
    info.incompat = journal_be32(superblock + 40);
    unsupported = info.incompat &
                  ~(JBD2_FEATURE_INCOMPAT_REVOKE |
                    JBD2_FEATURE_INCOMPAT_64BIT |
                    JBD2_FEATURE_INCOMPAT_CSUM_V2 |
                    JBD2_FEATURE_INCOMPAT_CSUM_V3);
    if (unsupported || info.maxlen <= info.first ||
        info.start >= info.maxlen) goto done;
    info.has_64bit =
        (info.incompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
    info.has_csum =
        (info.incompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 |
                          JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0;
    cursor = info.start;
    sequence = info.sequence;
    transactions = 0;
    while (cursor != 0 && transactions < info.maxlen) {
        if (journal_find_commit(fs, &journal_inode, &info, cursor,
                                sequence, &commit, metadata) != 0) break;
        if (journal_replay_transaction(fs, &journal_inode, &info, cursor,
                                       commit, sequence, metadata, data,
                                       scan) != 0)
            goto done;
        cursor = journal_next(&info, commit);
        sequence++;
        transactions++;
        if (cursor == info.start) break;
    }
    journal_put_be32(superblock + 28, 0);
    journal_put_be32(superblock + 24, sequence);
    if (journal_write(fs, &journal_inode, 0, superblock) != 0) goto done;
    fs->sb.s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
    fs->super_dirty = true;
    result = 0;
done:
    kfree(scan);
    kfree(data);
    kfree(metadata);
    kfree(superblock);
    return result;
}

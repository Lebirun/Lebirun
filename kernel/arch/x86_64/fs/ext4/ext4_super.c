#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

int ext4_read_superblock(ext4_fs_t *fs) {
    ahci_port_t *port = ahci_get_port(fs->port_index);
    if (!port) {
        return -1;
    }

    uint8_t *buffer = (uint8_t *)kmalloc(4096);
    if (!buffer) {
        return -1;
    }

    if (ahci_read_sectors(port, fs->partition_start_lba, 8, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    memcpy(&fs->sb, buffer + EXT4_SUPERBLOCK_OFFSET, sizeof(ext4_superblock_t));
    kfree(buffer);

    if (fs->sb.s_magic != EXT4_SUPER_MAGIC) {
        return -1;
    }

    fs->block_size = 1024 << fs->sb.s_log_block_size;
    fs->inode_size = fs->sb.s_inode_size;
    if (fs->inode_size == 0) {
        fs->inode_size = EXT4_GOOD_OLD_INODE_SIZE;
    }
    fs->inodes_per_block = fs->block_size / fs->inode_size;
    fs->sectors_per_block = fs->block_size / 512;
    fs->first_data_block = fs->sb.s_first_data_block;

    fs->total_blocks = fs->sb.s_blocks_count_lo;
    if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        fs->total_blocks |= ((uint64_t)fs->sb.s_blocks_count_hi << 32);
        fs->is_64bit = true;
    } else {
        fs->is_64bit = false;
    }

    fs->total_inodes = fs->sb.s_inodes_count;
    fs->groups_count = (fs->sb.s_blocks_count_lo - fs->sb.s_first_data_block + 
                        fs->sb.s_blocks_per_group - 1) / fs->sb.s_blocks_per_group;

    if (fs->is_64bit && fs->sb.s_desc_size > 0) {
        fs->desc_size = fs->sb.s_desc_size;
    } else {
        fs->desc_size = 32;
    }
    fs->desc_per_block = fs->block_size / fs->desc_size;

    fs->use_extents = (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0;

    return 0;
}

int ext4_write_superblock(ext4_fs_t *fs) {
    ahci_port_t *port = ahci_get_port(fs->port_index);
    if (!port) {
        return -1;
    }

    uint8_t *buffer = (uint8_t *)kmalloc(4096);
    if (!buffer) {
        return -1;
    }

    memset(buffer, 0, 4096);
    if (ahci_read_sectors(port, fs->partition_start_lba, 8, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    memcpy(buffer + EXT4_SUPERBLOCK_OFFSET, &fs->sb, sizeof(ext4_superblock_t));

    if (ahci_write_sectors(port, fs->partition_start_lba, 8, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    kfree(buffer);
    return 0;
}

int ext4_validate_superblock(ext4_superblock_t *sb) {
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        return -1;
    }

    if (sb->s_log_block_size > 6) {
        return -1;
    }

    if (sb->s_blocks_per_group == 0 || sb->s_inodes_per_group == 0) {
        return -1;
    }

    if (sb->s_inode_size != 0 && (sb->s_inode_size < 128 || sb->s_inode_size > 1024)) {
        return -1;
    }

    return 0;
}

void ext4_print_superblock(ext4_superblock_t *sb) {
    (void)sb;
}

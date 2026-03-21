#include <kernel/fs/ext4/ext4.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

static uint32_t cache_tick_counter = 0;

static int find_cache_entry(ext4_fs_t *fs, uint64_t block) {
    for (int i = 0; i < EXT4_CACHE_BLOCKS; i++) {
        if (fs->block_cache[i].data && fs->block_cache[i].block_num == (uint32_t)block) {
            return i;
        }
    }
    return -1;
}

static int find_free_cache_entry(ext4_fs_t *fs) {
    int oldest = -1;
    uint32_t oldest_tick = 0xFFFFFFFF;

    for (int i = 0; i < EXT4_CACHE_BLOCKS; i++) {
        if (!fs->block_cache[i].data) {
            return i;
        }
        if (fs->block_cache[i].ref_count == 0 && fs->block_cache[i].last_access < oldest_tick) {
            oldest_tick = fs->block_cache[i].last_access;
            oldest = i;
        }
    }

    if (oldest >= 0 && fs->block_cache[oldest].dirty) {
        int ret = ext4_write_block(fs, fs->block_cache[oldest].block_num, fs->block_cache[oldest].data);
        if (ret == 0) {
            fs->block_cache[oldest].dirty = false;
        }
        /* If write failed, still evict this slot - caller handles the failure */
    }

    return oldest;
}

int ext4_read_block(ext4_fs_t *fs, uint64_t block, void *buffer) {
    ahci_port_t *port = ahci_get_port(fs->port_index);
    if (!port) {
        return -1;
    }

    uint64_t lba = fs->partition_start_lba + block * fs->sectors_per_block;
    
    if (ahci_read_sectors(port, lba, fs->sectors_per_block, buffer) != 0) {
        return -1;
    }

    return 0;
}

int ext4_write_block(ext4_fs_t *fs, uint64_t block, const void *buffer) {
    ahci_port_t *port = ahci_get_port(fs->port_index);
    if (!port) {
        return -1;
    }

    uint64_t lba = fs->partition_start_lba + block * fs->sectors_per_block;
    
    if (ahci_write_sectors(port, lba, fs->sectors_per_block, buffer) != 0) {
        return -1;
    }

    return 0;
}

uint8_t *ext4_get_block(ext4_fs_t *fs, uint64_t block) {
    int idx = find_cache_entry(fs, block);
    
    if (idx >= 0) {
        fs->block_cache[idx].ref_count++;
        fs->block_cache[idx].last_access = ++cache_tick_counter;
        return fs->block_cache[idx].data;
    }

    idx = find_free_cache_entry(fs);
    if (idx < 0) {
        return NULL;
    }

    if (!fs->block_cache[idx].data) {
        fs->block_cache[idx].data = (uint8_t *)kmalloc(fs->block_size);
        if (!fs->block_cache[idx].data) {
            return NULL;
        }
    }

    if (ext4_read_block(fs, block, fs->block_cache[idx].data) != 0) {
        return NULL;
    }

    fs->block_cache[idx].block_num = (uint32_t)block;
    fs->block_cache[idx].ref_count = 1;
    fs->block_cache[idx].dirty = false;
    fs->block_cache[idx].last_access = ++cache_tick_counter;

    return fs->block_cache[idx].data;
}

void ext4_release_block(ext4_fs_t *fs, uint64_t block) {
    int idx = find_cache_entry(fs, block);
    if (idx >= 0 && fs->block_cache[idx].ref_count > 0) {
        fs->block_cache[idx].ref_count--;
    }
}

void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block) {
    int idx = find_cache_entry(fs, block);
    if (idx >= 0) {
        fs->block_cache[idx].dirty = true;
    }
}

int ext4_sync_blocks(ext4_fs_t *fs) {
    int errors = 0;

    for (int i = 0; i < EXT4_CACHE_BLOCKS; i++) {
        if (fs->block_cache[i].data && fs->block_cache[i].dirty) {
            if (ext4_write_block(fs, fs->block_cache[i].block_num, fs->block_cache[i].data) != 0) {
                errors++;
            } else {
                fs->block_cache[i].dirty = false;
            }
        }
    }

    return errors ? -1 : 0;
}

void ext4_flush_cache(ext4_fs_t *fs) {
    ext4_sync_blocks(fs);
    
    for (int i = 0; i < EXT4_CACHE_BLOCKS; i++) {
        if (fs->block_cache[i].data) {
            kfree(fs->block_cache[i].data);
            fs->block_cache[i].data = NULL;
            fs->block_cache[i].block_num = 0;
            fs->block_cache[i].ref_count = 0;
            fs->block_cache[i].dirty = false;
        }
    }
}

static int ext4_read_group_desc(ext4_fs_t *fs, uint32_t group, ext4_group_desc_t *desc) {
    uint32_t desc_block = fs->first_data_block + 1 + (group * fs->desc_size) / fs->block_size;
    uint32_t desc_offset = (group * fs->desc_size) % fs->block_size;

    uint8_t *block = ext4_get_block(fs, desc_block);
    if (!block) {
        return -1;
    }

    memcpy(desc, block + desc_offset, sizeof(ext4_group_desc_t));
    ext4_release_block(fs, desc_block);

    return 0;
}

static int ext4_write_group_desc(ext4_fs_t *fs, uint32_t group, ext4_group_desc_t *desc) {
    uint32_t desc_block = fs->first_data_block + 1 + (group * fs->desc_size) / fs->block_size;
    uint32_t desc_offset = (group * fs->desc_size) % fs->block_size;

    uint8_t *block = ext4_get_block(fs, desc_block);
    if (!block) {
        return -1;
    }

    memcpy(block + desc_offset, desc, sizeof(ext4_group_desc_t));
    ext4_mark_block_dirty(fs, desc_block);
    ext4_release_block(fs, desc_block);

    return 0;
}

int ext4_alloc_block(ext4_fs_t *fs, uint32_t hint) {
    uint32_t start_group = 0;
    if (hint > 0) {
        start_group = (hint - fs->first_data_block) / fs->sb.s_blocks_per_group;
    }

    for (uint32_t g = 0; g < fs->groups_count; g++) {
        uint32_t group = (start_group + g) % fs->groups_count;
        ext4_group_desc_t desc;

        if (ext4_read_group_desc(fs, group, &desc) != 0) {
            continue;
        }

        if (desc.bg_free_blocks_count_lo == 0) {
            continue;
        }

        uint64_t bitmap_block = desc.bg_block_bitmap_lo;
        if (fs->is_64bit) {
            bitmap_block |= ((uint64_t)desc.bg_block_bitmap_hi << 32);
        }

        uint8_t *bitmap = ext4_get_block(fs, bitmap_block);
        if (!bitmap) {
            continue;
        }

        uint32_t blocks_in_group = fs->sb.s_blocks_per_group;
        if (group == fs->groups_count - 1) {
            blocks_in_group = (fs->sb.s_blocks_count_lo - fs->first_data_block) % fs->sb.s_blocks_per_group;
            if (blocks_in_group == 0) blocks_in_group = fs->sb.s_blocks_per_group;
        }

        for (uint32_t bit = 0; bit < blocks_in_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint32_t bit_idx = bit % 8;

            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] |= (1 << bit_idx);
                ext4_mark_block_dirty(fs, bitmap_block);
                ext4_release_block(fs, bitmap_block);

                desc.bg_free_blocks_count_lo--;
                ext4_write_group_desc(fs, group, &desc);

                fs->sb.s_free_blocks_count_lo--;
                ext4_write_superblock(fs);

                return fs->first_data_block + group * fs->sb.s_blocks_per_group + bit;
            }
        }

        ext4_release_block(fs, bitmap_block);
    }

    return -1;
}

int ext4_free_block(ext4_fs_t *fs, uint64_t block) {
    if (block < fs->first_data_block || block >= fs->total_blocks) {
        return -1;
    }

    uint32_t group = (block - fs->first_data_block) / fs->sb.s_blocks_per_group;
    uint32_t bit = (block - fs->first_data_block) % fs->sb.s_blocks_per_group;

    ext4_group_desc_t desc;
    if (ext4_read_group_desc(fs, group, &desc) != 0) {
        return -1;
    }

    uint64_t bitmap_block = desc.bg_block_bitmap_lo;
    if (fs->is_64bit) {
        bitmap_block |= ((uint64_t)desc.bg_block_bitmap_hi << 32);
    }

    uint8_t *bitmap = ext4_get_block(fs, bitmap_block);
    if (!bitmap) {
        return -1;
    }

    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx = bit % 8;

    if (!(bitmap[byte_idx] & (1 << bit_idx))) {
        ext4_release_block(fs, bitmap_block);
        return -1;
    }

    bitmap[byte_idx] &= ~(1 << bit_idx);
    ext4_mark_block_dirty(fs, bitmap_block);
    ext4_release_block(fs, bitmap_block);

    desc.bg_free_blocks_count_lo++;
    ext4_write_group_desc(fs, group, &desc);

    fs->sb.s_free_blocks_count_lo++;
    ext4_write_superblock(fs);

    return 0;
}

int ext4_load_group_descs(ext4_fs_t *fs) {
    fs->group_descs = (ext4_group_desc_t *)kmalloc(fs->groups_count * sizeof(ext4_group_desc_t));
    if (!fs->group_descs) {
        return -1;
    }

    for (uint32_t i = 0; i < fs->groups_count; i++) {
        if (ext4_read_group_desc(fs, i, &fs->group_descs[i]) != 0) {
            kfree(fs->group_descs);
            fs->group_descs = NULL;
            return -1;
        }
    }

    return 0;
}

#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/blkdev.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

static uint32_t ext4_group_free_blocks(ext4_fs_t *fs,
                                       const ext4_group_desc_t *desc) {
    uint32_t count;

    count = desc->bg_free_blocks_count_lo;
    if (fs->is_64bit)
        count |= (uint32_t)desc->bg_free_blocks_count_hi << 16;
    return count;
}

static void ext4_set_group_free_blocks(ext4_fs_t *fs,
                                       ext4_group_desc_t *desc,
                                       uint32_t count) {
    desc->bg_free_blocks_count_lo = (uint16_t)count;
    if (fs->is_64bit)
        desc->bg_free_blocks_count_hi = (uint16_t)(count >> 16);
}

static uint64_t ext4_super_free_blocks(ext4_fs_t *fs) {
    uint64_t count;

    count = fs->sb.s_free_blocks_count_lo;
    if (fs->is_64bit)
        count |= (uint64_t)fs->sb.s_free_blocks_count_hi << 32;
    return count;
}

static void ext4_set_super_free_blocks(ext4_fs_t *fs, uint64_t count) {
    fs->sb.s_free_blocks_count_lo = (uint32_t)count;
    if (fs->is_64bit)
        fs->sb.s_free_blocks_count_hi = (uint32_t)(count >> 32);
}

static int find_cache_entry(ext4_fs_t *fs, uint64_t block) {
    int i;

    for (i = 0; i < (int)fs->block_cache_count; i++) {
        if (fs->block_cache[i].data && fs->block_cache[i].block_num == block) {
            return i;
        }
    }
    return -1;
}

static uint8_t *ext4_alloc_cache_data(ext4_fs_t *fs) {
    if (fs->block_size >= PAGE_SIZE)
        return (uint8_t *)slab_page_alloc(fs->block_size);
    return (uint8_t *)kmalloc(fs->block_size);
}

static void ext4_free_cache_data(ext4_fs_t *fs, uint8_t *data) {
    if (fs->block_size >= PAGE_SIZE)
        slab_page_free(data, fs->block_size);
    else
        kfree(data);
}

static void ext4_discard_cache_entry(ext4_fs_t *fs,
                                      ext4_block_cache_entry_t *entry) {
    if (entry->data) ext4_free_cache_data(fs, entry->data);
    memset(entry, 0, sizeof(*entry));
}

static int find_free_cache_entry(ext4_fs_t *fs) {
    int oldest;
    int dirty_oldest;
    uint32_t oldest_tick;
    uint32_t dirty_oldest_tick;
    int i;
    uint32_t old_count;
    uint32_t new_count;
    ext4_block_cache_entry_t *new_cache;
    int ret;

retry:
    oldest = -1;
    dirty_oldest = -1;
    oldest_tick = 0xFFFFFFFF;
    dirty_oldest_tick = 0xFFFFFFFF;
    for (i = 0; i < (int)fs->block_cache_count; i++) {
        if (!fs->block_cache[i].data) {
            return i;
        }
        if (fs->block_cache[i].ref_count == 0) {
            if (fs->block_cache[i].dirty &&
                fs->block_cache[i].last_access < dirty_oldest_tick) {
                dirty_oldest_tick = fs->block_cache[i].last_access;
                dirty_oldest = i;
            } else if (!fs->block_cache[i].dirty &&
                       fs->block_cache[i].last_access < oldest_tick) {
                oldest_tick = fs->block_cache[i].last_access;
                oldest = i;
            }
        }
    }

    if (oldest >= 0) return oldest;

    if (dirty_oldest >= 0 && fs->block_cache_count >= 8) {
        ret = ext4_sync_blocks(fs);
        if (ret != 0) return -1;
        goto retry;
    }

    if (dirty_oldest < 0 || fs->block_cache_count < 8) {
        old_count = fs->block_cache_count;
        if (old_count == 0) new_count = 1;
        else if (old_count > UINT32_MAX / 2) return -1;
        else new_count = old_count * 2;
        new_cache = (ext4_block_cache_entry_t *)krealloc(fs->block_cache,
            (uint64_t)new_count * sizeof(ext4_block_cache_entry_t));
        if (!new_cache) return -1;
        memset(new_cache + old_count, 0,
            (uint64_t)(new_count - old_count) * sizeof(ext4_block_cache_entry_t));
        fs->block_cache = new_cache;
        fs->block_cache_count = new_count;
        return (int)old_count;
    }

    return -1;
}

int ext4_read_block(ext4_fs_t *fs, uint64_t block, void *buffer) {
    blkdev_t *dev;
    uint64_t lba;

    dev = blkdev_get(fs->port_index);
    if (!dev) {
        return -1;
    }

    lba = fs->partition_start_lba + block * fs->sectors_per_block;
    
    if (blkdev_read(dev, lba, fs->sectors_per_block, buffer) != 0) {
        return -1;
    }

    return 0;
}

int ext4_read_blocks(ext4_fs_t *fs, uint64_t block, uint32_t count,
                     void *buffer) {
    blkdev_t *dev;
    uint64_t lba;
    uint64_t sectors;
    int idx;
    uint32_t i;

    if (!fs || !buffer || count == 0) {
        return -1;
    }

    dev = blkdev_get(fs->port_index);
    if (!dev) {
        return -1;
    }

    if (fs->sectors_per_block != 0 &&
        (uint64_t)count > UINT64_MAX / fs->sectors_per_block) {
        return -1;
    }
    sectors = (uint64_t)count * fs->sectors_per_block;
    if (fs->sectors_per_block != 0 &&
        block > (UINT64_MAX - fs->partition_start_lba) /
                fs->sectors_per_block) {
        return -1;
    }
    if ((uint64_t)count - 1 > UINT64_MAX - block) return -1;

    for (i = 0; i < count; i++) {
        idx = find_cache_entry(fs, block + i);
        if (idx >= 0 && count == 1) {
            memcpy(buffer, fs->block_cache[idx].data, fs->block_size);
            fs->block_cache[idx].last_access = ++fs->cache_tick;
            return 0;
        }
        if (idx >= 0 && fs->block_cache[idx].dirty) {
            return -1;
        }
    }

    lba = fs->partition_start_lba + block * fs->sectors_per_block;
    if (blkdev_read(dev, lba, sectors, buffer) != 0) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        idx = find_cache_entry(fs, block + i);
        if (idx >= 0 && fs->block_cache[idx].data) {
            memcpy(fs->block_cache[idx].data,
                   (const uint8_t *)buffer + (uint64_t)i * fs->block_size,
                   fs->block_size);
            fs->block_cache[idx].last_access = ++fs->cache_tick;
        }
    }

    return 0;
}

int ext4_read_blocks_unlocked(ext4_fs_t *fs, uint64_t block, uint32_t count,
                              void *buffer) {
    blkdev_t *dev;
    uint64_t gen;
    uint32_t i;
    int idx;
    int result;

    dev = blkdev_get(fs->port_index);
    if (!dev || count == 0 ||
        (uint64_t)count > UINT64_MAX / fs->sectors_per_block ||
        block > (UINT64_MAX - fs->partition_start_lba) / fs->sectors_per_block)
        return -1;
    for (i = 0; i < count; i++) {
        idx = find_cache_entry(fs, block + i);
        if (idx >= 0 && fs->block_cache[idx].dirty) return -1;
    }
    gen = fs->free_gen;
    mutex_unlock(&fs->lock);
    result = blkdev_read(dev, fs->partition_start_lba + block * fs->sectors_per_block,
                         (uint64_t)count * fs->sectors_per_block, buffer);
    mutex_lock(&fs->lock);
    if (result != 0) return -1;
    for (i = 0; i < count; i++) {
        idx = find_cache_entry(fs, block + i);
        if (idx < 0) continue;
        if (fs->block_cache[idx].dirty)
            memcpy((uint8_t *)buffer + (uint64_t)i * fs->block_size,
                   fs->block_cache[idx].data, fs->block_size);
        else
            memcpy(fs->block_cache[idx].data,
                   (uint8_t *)buffer + (uint64_t)i * fs->block_size, fs->block_size);
    }
    return fs->free_gen != gen;
}

int ext4_write_block(ext4_fs_t *fs, uint64_t block, const void *buffer) {
    blkdev_t *dev;
    uint64_t lba;
    int idx;

    dev = blkdev_get(fs->port_index);
    if (!dev) {
        return -1;
    }

    lba = fs->partition_start_lba + block * fs->sectors_per_block;
    
    if (blkdev_write(dev, lba, fs->sectors_per_block, buffer) != 0) {
        return -1;
    }

    idx = find_cache_entry(fs, block);
    if (idx >= 0 && fs->block_cache[idx].data) {
        if (fs->block_cache[idx].data != (const uint8_t *)buffer)
            memcpy(fs->block_cache[idx].data, buffer, fs->block_size);
        fs->block_cache[idx].dirty = false;
        fs->block_cache[idx].last_access = ++fs->cache_tick;
    }

    return 0;
}

int ext4_write_blocks(ext4_fs_t *fs, uint64_t block, uint32_t count, const void *buffer) {
    blkdev_t *dev;
    uint64_t lba;
    uint64_t sectors;
    int idx;
    uint32_t i;

    if (!fs || !buffer || count == 0) {
        return -1;
    }

    if (count == 1) {
        return ext4_write_block(fs, block, buffer);
    }

    dev = blkdev_get(fs->port_index);
    if (!dev) {
        return -1;
    }

    if (fs->sectors_per_block != 0 &&
        (uint64_t)count > UINT64_MAX / fs->sectors_per_block) {
        return -1;
    }
    sectors = (uint64_t)count * fs->sectors_per_block;
    if (fs->sectors_per_block != 0 &&
        block > (UINT64_MAX - fs->partition_start_lba) /
                fs->sectors_per_block) {
        return -1;
    }
    if ((uint64_t)count - 1 > UINT64_MAX - block) return -1;
    lba = fs->partition_start_lba + block * fs->sectors_per_block;
    if (blkdev_write(dev, lba, sectors, buffer) != 0) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        idx = find_cache_entry(fs, block + i);
        if (idx >= 0 && fs->block_cache[idx].data) {
            memcpy(fs->block_cache[idx].data,
                   (const uint8_t *)buffer + (uint64_t)i * fs->block_size,
                   fs->block_size);
            fs->block_cache[idx].dirty = false;
            fs->block_cache[idx].last_access = ++fs->cache_tick;
        }
    }

    return 0;
}

uint8_t *ext4_get_block(ext4_fs_t *fs, uint64_t block) {
    uint8_t *data;
    int cached_idx;
    int idx;
    
    idx = find_cache_entry(fs, block);
    if (idx >= 0) {
        fs->block_cache[idx].ref_count++;
        fs->block_cache[idx].last_access = ++fs->cache_tick;
        return fs->block_cache[idx].data;
    }

    idx = find_free_cache_entry(fs);
    if (idx < 0) {
        return NULL;
    }

    data = fs->block_cache[idx].data;
    memset(&fs->block_cache[idx], 0, sizeof(fs->block_cache[idx]));
    if (!data) {
        data = ext4_alloc_cache_data(fs);
        if (!data) {
            return NULL;
        }
    }

    if (ext4_read_block(fs, block, data) != 0) {
        ext4_free_cache_data(fs, data);
        return NULL;
    }

    idx = find_cache_entry(fs, block);
    if (idx < 0) {
        idx = find_free_cache_entry(fs);
        cached_idx = find_cache_entry(fs, block);
        if (cached_idx < 0) {
            if (idx < 0) {
                ext4_free_cache_data(fs, data);
                return NULL;
            }
            ext4_discard_cache_entry(fs, &fs->block_cache[idx]);
            fs->block_cache[idx].data = data;
            fs->block_cache[idx].block_num = block;
            fs->block_cache[idx].ref_count = 1;
            fs->block_cache[idx].last_access = ++fs->cache_tick;
            return data;
        }
        idx = cached_idx;
    }

    fs->block_cache[idx].ref_count++;
    fs->block_cache[idx].last_access = ++fs->cache_tick;
    ext4_free_cache_data(fs, data);
    return fs->block_cache[idx].data;
}

uint8_t *ext4_get_block_overwrite(ext4_fs_t *fs, uint64_t block) {
    int idx;

    idx = find_cache_entry(fs, block);
    if (idx >= 0) {
        fs->block_cache[idx].ref_count++;
        fs->block_cache[idx].last_access = ++fs->cache_tick;
        return fs->block_cache[idx].data;
    }

    idx = find_free_cache_entry(fs);
    if (idx < 0) {
        return NULL;
    }

    if (!fs->block_cache[idx].data) {
        fs->block_cache[idx].data = ext4_alloc_cache_data(fs);
        if (!fs->block_cache[idx].data) {
            return NULL;
        }
    }

    fs->block_cache[idx].block_num = block;
    fs->block_cache[idx].ref_count = 1;
    fs->block_cache[idx].dirty = false;
    fs->block_cache[idx].last_access = ++fs->cache_tick;

    return fs->block_cache[idx].data;
}

int ext4_zero_block(ext4_fs_t *fs, uint64_t block) {
    uint8_t *data;
    int idx;

    data = ext4_get_block_overwrite(fs, block);
    if (!data) return -1;
    memset(data, 0, fs->block_size);
    if (ext4_write_block(fs, block, data) == 0) {
        ext4_release_block(fs, block);
        return 0;
    }
    idx = find_cache_entry(fs, block);
    if (idx >= 0 && fs->block_cache[idx].data == data)
        ext4_discard_cache_entry(fs, &fs->block_cache[idx]);
    return -1;
}

void ext4_release_block(ext4_fs_t *fs, uint64_t block) {
    int idx;

    idx = find_cache_entry(fs, block);
    if (idx >= 0 && fs->block_cache[idx].ref_count > 0) {
        fs->block_cache[idx].ref_count--;
    }
}

int ext4_reclaim_clean_blocks(ext4_fs_t *fs, uint32_t max_blocks) {
    int reclaimed;
    int i;

    if (!fs) return 0;

    reclaimed = 0;
    for (i = 0; i < (int)fs->block_cache_count; i++) {
        if (max_blocks != 0 && reclaimed >= (int)max_blocks) {
            break;
        }
        if (fs->block_cache[i].data &&
            fs->block_cache[i].ref_count == 0 &&
            !fs->block_cache[i].dirty) {
            ext4_discard_cache_entry(fs, &fs->block_cache[i]);
            reclaimed++;
        }
    }

    return reclaimed;
}

void ext4_compact_block_cache(ext4_fs_t *fs) {
    ext4_block_cache_entry_t *cache;
    uint32_t read_index;
    uint32_t write_index;

    if (!fs || !fs->block_cache) return;
    write_index = 0;
    for (read_index = 0; read_index < fs->block_cache_count; read_index++) {
        if (!fs->block_cache[read_index].data) continue;
        if (read_index != write_index)
            fs->block_cache[write_index] = fs->block_cache[read_index];
        write_index++;
    }
    if (write_index == 0) {
        kfree(fs->block_cache);
        fs->block_cache = NULL;
        fs->block_cache_count = 0;
        return;
    }
    cache = (ext4_block_cache_entry_t *)krealloc(
        fs->block_cache,
        (uint64_t)write_index * sizeof(ext4_block_cache_entry_t));
    if (cache) fs->block_cache = cache;
    fs->block_cache_count = write_index;
}

void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block) {
    int idx = find_cache_entry(fs, block);
    if (idx >= 0) {
        fs->block_cache[idx].dirty = true;
    }
}

static void ext4_dirty_heap_sift(ext4_fs_t *fs, uint32_t *indices,
                                 int count, int root) {
    int child;
    int largest;
    uint32_t temp;

    for (;;) {
        child = root * 2 + 1;
        if (child >= count) return;
        largest = root;
        if (fs->block_cache[indices[child]].block_num >
            fs->block_cache[indices[largest]].block_num)
            largest = child;
        if (child + 1 < count &&
            fs->block_cache[indices[child + 1]].block_num >
            fs->block_cache[indices[largest]].block_num)
            largest = child + 1;
        if (largest == root) return;
        temp = indices[root];
        indices[root] = indices[largest];
        indices[largest] = temp;
        root = largest;
    }
}

static void ext4_sort_dirty_indices(ext4_fs_t *fs, uint32_t *indices,
                                    int count) {
    int i;
    uint32_t temp;

    for (i = count / 2 - 1; i >= 0; i--)
        ext4_dirty_heap_sift(fs, indices, count, i);
    for (i = count - 1; i > 0; i--) {
        temp = indices[0];
        indices[0] = indices[i];
        indices[i] = temp;
        ext4_dirty_heap_sift(fs, indices, i, 0);
    }
}

int ext4_sync_blocks(ext4_fs_t *fs) {
    int errors = 0;
    int dirty_count = 0;
    int i;
    int j;
    int max_run;
    int run_len;
    uint32_t *dirty_idx;
    blkdev_t *dev;
    const void *vectors[8];
    uint64_t base_lba;
    uint64_t total_sectors;

    for (i = 0; i < (int)fs->block_cache_count; i++) {
        if (fs->block_cache[i].data && fs->block_cache[i].dirty) {
            dirty_count++;
        }
    }

    if (dirty_count == 0) {
        return 0;
    }

    if (dirty_count == 1 || fs->block_size != 4096) {
        for (i = 0; i < (int)fs->block_cache_count; i++) {
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

    if ((uint64_t)dirty_count > SIZE_MAX / sizeof(uint32_t)) return -1;
    dirty_idx = (uint32_t *)kmalloc((uint64_t)dirty_count * sizeof(uint32_t));
    if (!dirty_idx) return -1;

    j = 0;
    for (i = 0; i < (int)fs->block_cache_count; i++) {
        if (fs->block_cache[i].data && fs->block_cache[i].dirty) {
            dirty_idx[j++] = (uint32_t)i;
        }
    }

    ext4_sort_dirty_indices(fs, dirty_idx, dirty_count);

    dev = blkdev_get(fs->port_index);
    max_run = 8;

    i = 0;
    while (i < dirty_count) {
        run_len = 1;
        while (i + run_len < dirty_count && run_len < max_run) {
            if (fs->block_cache[dirty_idx[i + run_len]].block_num ==
                fs->block_cache[dirty_idx[i + run_len - 1]].block_num + 1) {
                run_len++;
            } else {
                break;
            }
        }

        if (run_len == 1 || !dev) {
            if (ext4_write_block(fs, fs->block_cache[dirty_idx[i]].block_num,
                                 fs->block_cache[dirty_idx[i]].data) != 0) {
                errors++;
            } else {
                fs->block_cache[dirty_idx[i]].dirty = false;
            }
            i++;
            continue;
        }

        total_sectors = (uint64_t)run_len * fs->sectors_per_block;
        base_lba = fs->partition_start_lba +
                   (uint64_t)fs->block_cache[dirty_idx[i]].block_num * fs->sectors_per_block;

        for (j = 0; j < run_len; j++) {
            vectors[j] = fs->block_cache[dirty_idx[i + j]].data;
        }

        if (blkdev_writev(dev, base_lba, total_sectors, vectors,
                                (uint32_t)run_len, fs->block_size) != 0) {
            errors++;
        } else {
            for (j = 0; j < run_len; j++) {
                fs->block_cache[dirty_idx[i + j]].dirty = false;
            }
        }

        i += run_len;
    }

    kfree(dirty_idx);
    return errors ? -1 : 0;
}

void ext4_drop_block_cache(ext4_fs_t *fs) {
    int i;

    if (!fs) return;
    for (i = 0; i < (int)fs->block_cache_count; i++) {
        ext4_discard_cache_entry(fs, &fs->block_cache[i]);
    }
    if (fs->block_cache) {
        kfree(fs->block_cache);
        fs->block_cache = NULL;
        fs->block_cache_count = 0;
    }
}

void ext4_flush_cache(ext4_fs_t *fs) {
    ext4_sync_blocks(fs);
    ext4_drop_block_cache(fs);
}

static int ext4_group_has_super(ext4_fs_t *fs, uint64_t group) {
    uint64_t n;
    uint32_t p;

    if (group == 0) return 1;
    if (fs->sb.s_feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2)
        return group == fs->sb.s_backup_bgs[0] || group == fs->sb.s_backup_bgs[1];
    if (group == 1 ||
        !(fs->sb.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER))
        return 1;
    if (!(group & 1)) return 0;
    for (p = 3; p <= 7; p += 2) {
        for (n = p; n < group; n *= p);
        if (n == group) return 1;
    }
    return 0;
}

static uint64_t ext4_group_first_block(ext4_fs_t *fs, uint64_t group) {
    return fs->first_data_block + group * fs->sb.s_blocks_per_group;
}

static int ext4_desc_location(ext4_fs_t *fs, uint64_t group,
                              uint64_t *block, uint32_t *offset) {
    uint64_t per_block;
    uint64_t meta;

    if (group >= fs->groups_count) return -1;
    per_block = fs->block_size / fs->desc_size;
    meta = group / per_block;
    *offset = (uint32_t)((group % per_block) * fs->desc_size);
    if (!(fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) ||
        meta < fs->sb.s_first_meta_bg) {
        *block = fs->first_data_block + 1 + meta;
        return 0;
    }
    meta *= per_block;
    *block = ext4_group_first_block(fs, meta) + ext4_group_has_super(fs, meta);
    if (fs->block_size == 1024 && fs->first_data_block == 0 && meta == 0)
        (*block)++;
    return 0;
}

int ext4_read_group_desc(ext4_fs_t *fs, uint64_t group, ext4_group_desc_t *desc) {
    uint64_t desc_block;
    uint32_t desc_offset;
    uint8_t *block;

    if (ext4_desc_location(fs, group, &desc_block, &desc_offset) != 0) return -1;
    block = ext4_get_block(fs, desc_block);
    if (!block) {
        return -1;
    }

    memset(desc, 0, sizeof(*desc));
    memcpy(desc, block + desc_offset,
           fs->desc_size < sizeof(*desc) ? fs->desc_size : sizeof(*desc));
    ext4_release_block(fs, desc_block);

    return 0;
}

int ext4_write_group_desc(ext4_fs_t *fs, uint64_t group, ext4_group_desc_t *desc) {
    uint64_t desc_block;
    uint32_t desc_offset;
    uint8_t *block;

    if (ext4_desc_location(fs, group, &desc_block, &desc_offset) != 0) return -1;
    block = ext4_get_block(fs, desc_block);
    if (!block) {
        return -1;
    }

    memcpy(block + desc_offset, desc,
           fs->desc_size < sizeof(*desc) ? fs->desc_size : sizeof(*desc));
    ext4_desc_csum(fs, (uint32_t)group, block + desc_offset);
    ext4_mark_block_dirty(fs, desc_block);
    ext4_release_block(fs, desc_block);

    return 0;
}

static uint32_t ext4_group_blocks(ext4_fs_t *fs, uint64_t group) {
    uint32_t count;

    if (group != fs->groups_count - 1) return fs->sb.s_blocks_per_group;
    count = (uint32_t)((fs->total_blocks - fs->first_data_block) %
                       fs->sb.s_blocks_per_group);
    return count ? count : fs->sb.s_blocks_per_group;
}

static void ext4_bitmap_mark(ext4_fs_t *fs, uint8_t *bitmap, uint64_t group,
                             uint64_t block, uint64_t count) {
    uint64_t first;
    uint64_t end;

    first = ext4_group_first_block(fs, group);
    end = first + ext4_group_blocks(fs, group);
    for (; count; count--, block++) {
        if (block >= first && block < end)
            bitmap[(block - first) / 8] |= (uint8_t)(1u << ((block - first) % 8));
    }
}

static void ext4_init_block_bitmap(ext4_fs_t *fs, uint64_t group,
                                   ext4_group_desc_t *desc, uint8_t *bitmap) {
    uint64_t per_block;
    uint64_t index;
    uint64_t meta;
    uint64_t base;
    uint64_t itable_blocks;
    uint32_t bit;

    memset(bitmap, 0, fs->block_size);
    per_block = fs->block_size / fs->desc_size;
    base = ext4_group_has_super(fs, group);
    if (!(fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) ||
        group < (uint64_t)fs->sb.s_first_meta_bg * per_block) {
        if (base) {
            if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG)
                base += fs->sb.s_first_meta_bg;
            else
                base += (fs->groups_count + per_block - 1) / per_block;
            base += fs->sb.s_reserved_gdt_blocks;
        }
    } else {
        meta = group / per_block * per_block;
        index = group - meta;
        if (index == 0 || index == 1 || index == per_block - 1) base++;
    }
    ext4_bitmap_mark(fs, bitmap, group, ext4_group_first_block(fs, group), base);
    itable_blocks = ((uint64_t)fs->sb.s_inodes_per_group * fs->inode_size +
                     fs->block_size - 1) / fs->block_size;
    ext4_bitmap_mark(fs, bitmap, group, desc->bg_block_bitmap_lo |
                     (fs->is_64bit ? (uint64_t)desc->bg_block_bitmap_hi << 32 : 0), 1);
    ext4_bitmap_mark(fs, bitmap, group, desc->bg_inode_bitmap_lo |
                     (fs->is_64bit ? (uint64_t)desc->bg_inode_bitmap_hi << 32 : 0), 1);
    ext4_bitmap_mark(fs, bitmap, group, desc->bg_inode_table_lo |
                     (fs->is_64bit ? (uint64_t)desc->bg_inode_table_hi << 32 : 0),
                     itable_blocks);
    for (bit = ext4_group_blocks(fs, group); bit < fs->block_size * 8; bit++)
        bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
    desc->bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
}

uint32_t ext4_alloc_block_run(ext4_fs_t *fs, uint64_t hint,
                              uint32_t max_count, uint64_t *first_block) {
    uint64_t start_group;
    uint32_t start_bit;
    uint64_t g;
    uint64_t group;
    ext4_group_desc_t desc;
    uint64_t bitmap_block;
    uint8_t *bitmap;
    uint32_t blocks_in_group;
    uint32_t pass;
    uint32_t bit;
    uint32_t limit;
    uint32_t byte_idx;
    uint32_t bit_idx;
    uint64_t allocated;
    uint32_t free_blocks;
    uint64_t super_free;
    uint32_t run_count;
    uint32_t run_bit;

    if (!fs || !first_block || fs->groups_count == 0 || max_count == 0)
        return 0;
    *first_block = 0;

    start_group = 0;
    start_bit = 0;
    if (hint > fs->first_data_block) {
        start_group = (hint - fs->first_data_block) / fs->sb.s_blocks_per_group;
        start_bit = (hint - fs->first_data_block) % fs->sb.s_blocks_per_group;
        if (start_group >= fs->groups_count) {
            start_group = 0;
            start_bit = 0;
        }
    } else if (fs->alloc_last_group < fs->groups_count) {
        start_group = fs->alloc_last_group;
        start_bit = fs->alloc_last_bit + 1;
    }

    for (g = 0; g < fs->groups_count; g++) {
        group = (start_group + g) % fs->groups_count;

        if (ext4_read_group_desc(fs, group, &desc) != 0) {
            continue;
        }

        free_blocks = ext4_group_free_blocks(fs, &desc);
        if (free_blocks == 0) {
            continue;
        }

        bitmap_block = desc.bg_block_bitmap_lo;
        if (fs->is_64bit) {
            bitmap_block |= ((uint64_t)desc.bg_block_bitmap_hi << 32);
        }

        bitmap = ext4_get_block(fs, bitmap_block);
        if (!bitmap) {
            continue;
        }
        if ((fs->metadata_csum || fs->gdt_csum) &&
            (desc.bg_flags & EXT4_BG_BLOCK_UNINIT)) {
            ext4_init_block_bitmap(fs, group, &desc, bitmap);
            ext4_mark_block_dirty(fs, bitmap_block);
            ext4_bitmap_csum(fs, &desc, bitmap, 0);
            ext4_write_group_desc(fs, group, &desc);
        }

        blocks_in_group = ext4_group_blocks(fs, group);

        if (group != start_group || start_bit >= blocks_in_group) {
            start_bit = 0;
        }

        for (pass = 0; pass < 2; pass++) {
            bit = pass == 0 ? start_bit : 0;
            limit = pass == 0 ? blocks_in_group : start_bit;

            while (bit < limit) {
                byte_idx = bit / 8;
                bit_idx = bit % 8;

                if (bit_idx == 0 && bitmap[byte_idx] == 0xFF) {
                    bit += 8;
                    continue;
                }

                if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                    run_count = 1;
                    while (run_count < max_count &&
                           run_count < free_blocks &&
                           bit + run_count < limit) {
                        run_bit = bit + run_count;
                        if (bitmap[run_bit / 8] &
                            (1u << (run_bit % 8)))
                            break;
                        run_count++;
                    }
                    for (run_bit = bit; run_bit < bit + run_count;
                         run_bit++)
                        bitmap[run_bit / 8] |=
                            (uint8_t)(1u << (run_bit % 8));
                    ext4_mark_block_dirty(fs, bitmap_block);
                    ext4_bitmap_csum(fs, &desc, bitmap, 0);
                    ext4_release_block(fs, bitmap_block);

                    ext4_set_group_free_blocks(fs, &desc,
                                               free_blocks - run_count);
                    ext4_write_group_desc(fs, group, &desc);

                    super_free = ext4_super_free_blocks(fs);
                    if (super_free >= run_count)
                        ext4_set_super_free_blocks(fs,
                                                   super_free - run_count);
                    fs->super_dirty = true;

                    fs->alloc_last_group = group;
                    fs->alloc_last_bit = bit + run_count - 1;

                    allocated = fs->first_data_block + group * fs->sb.s_blocks_per_group + bit;
                    *first_block = allocated;
                    return run_count;
                }

                bit++;
            }
        }

        ext4_release_block(fs, bitmap_block);
        start_bit = 0;
    }

    return 0;
}

int64_t ext4_alloc_block(ext4_fs_t *fs, uint64_t hint) {
    uint64_t block;

    if (ext4_alloc_block_run(fs, hint, 1, &block) != 1) return -1;
    return (int64_t)block;
}

int ext4_free_blocks(ext4_fs_t *fs, uint64_t block, uint64_t count) {
    uint64_t group;
    uint32_t bit;
    uint32_t n;
    uint32_t i;
    ext4_group_desc_t desc;
    uint64_t bitmap_block;
    uint8_t *bitmap;
    uint64_t super_free;

    if (block < fs->first_data_block || block >= fs->total_blocks ||
        count > fs->total_blocks - block)
        return -1;
    while (count) {
        group = (block - fs->first_data_block) / fs->sb.s_blocks_per_group;
        bit = (block - fs->first_data_block) % fs->sb.s_blocks_per_group;
        n = fs->sb.s_blocks_per_group - bit;
        if (n > count) n = (uint32_t)count;
        if (ext4_read_group_desc(fs, group, &desc) != 0) return -1;
        bitmap_block = desc.bg_block_bitmap_lo |
                       (fs->is_64bit ? (uint64_t)desc.bg_block_bitmap_hi << 32 : 0);
        bitmap = ext4_get_block(fs, bitmap_block);
        if (!bitmap) return -1;
        for (i = bit; i < bit + n; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                ext4_release_block(fs, bitmap_block);
                return -1;
            }
        }
        for (i = bit; i < bit + n; i++)
            bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
        ext4_bitmap_csum(fs, &desc, bitmap, 0);
        ext4_set_group_free_blocks(fs, &desc, ext4_group_free_blocks(fs, &desc) + n);
        ext4_mark_block_dirty(fs, bitmap_block);
        ext4_release_block(fs, bitmap_block);
        if (ext4_write_group_desc(fs, group, &desc) != 0) return -1;
        super_free = ext4_super_free_blocks(fs);
        ext4_set_super_free_blocks(fs, super_free + n);
        fs->super_dirty = true;
        fs->free_gen++;
        block += n;
        count -= n;
    }
    return 0;
}

int ext4_free_block(ext4_fs_t *fs, uint64_t block) {
    return ext4_free_blocks(fs, block, 1);
}

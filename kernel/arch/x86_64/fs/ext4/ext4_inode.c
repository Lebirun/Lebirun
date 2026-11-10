#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <lebirun/timekeeping.h>
#include <lebirun/tty.h>
#include <string.h>

extern void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block);

static ext4_inode_cache_t *find_inode_cache(ext4_fs_t *fs, uint32_t ino) {
    ext4_inode_cache_t *ic;

    for (ic = fs->inode_cache; ic; ic = ic->next)
        if (ic->ino == ino) return ic;
    return NULL;
}

static void ext4_invalidate_inode_cache(ext4_fs_t *fs, uint32_t ino) {
    ext4_inode_cache_t *ic;

    ic = find_inode_cache(fs, ino);
    if (!ic) return;
    ic->ino = 0;
    ic->ref_count = 0;
    ic->dirty = false;
    ic->vfs_node = NULL;
}

static ext4_inode_cache_t *find_free_inode_cache(ext4_fs_t *fs) {
    ext4_inode_cache_t *ic;

    for (ic = fs->inode_cache; ic; ic = ic->next)
        if (ic->ref_count == 0 && !ic->dirty) return ic;
    for (ic = fs->inode_cache; ic; ic = ic->next) {
        if (ic->ref_count == 0 &&
            ext4_write_inode(fs, ic->ino, &ic->inode) == 0) {
            ic->dirty = false;
            return ic;
        }
    }
    ic = (ext4_inode_cache_t *)kmalloc(sizeof(*ic));
    if (!ic) return NULL;
    memset(ic, 0, sizeof(*ic));
    ic->next = fs->inode_cache;
    fs->inode_cache = ic;
    return ic;
}

static int ext4_inode_table(ext4_fs_t *fs, uint32_t group, uint64_t *table) {
    ext4_group_desc_t desc;

    if (!fs->itable_block || fs->itable_group != group) {
        if (ext4_read_group_desc(fs, group, &desc) != 0) return -1;
        fs->itable_block = desc.bg_inode_table_lo;
        if (fs->is_64bit)
            fs->itable_block |= (uint64_t)desc.bg_inode_table_hi << 32;
        fs->itable_group = group;
    }
    *table = fs->itable_block;
    return 0;
}

static uint8_t *ext4_inode_slot(ext4_fs_t *fs, uint32_t ino,
                                uint64_t *inode_block) {
    uint32_t group;
    uint64_t index;
    uint64_t inode_table;
    uint8_t *block;

    if (ino == 0 || ino > fs->total_inodes) return NULL;
    group = (ino - 1) / fs->sb.s_inodes_per_group;
    index = (uint64_t)((ino - 1) % fs->sb.s_inodes_per_group) * fs->inode_size;
    if (ext4_inode_table(fs, group, &inode_table) != 0) return NULL;
    *inode_block = inode_table + index / fs->block_size;
    block = ext4_get_block(fs, *inode_block);
    return block ? block + index % fs->block_size : NULL;
}

int ext4_read_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode) {
    uint64_t inode_block;
    size_t copy_size;
    uint8_t *raw;

    raw = ext4_inode_slot(fs, ino, &inode_block);
    if (!raw) return -1;
    copy_size = fs->inode_size;
    if (copy_size > sizeof(ext4_inode_t)) copy_size = sizeof(ext4_inode_t);
    memset(inode, 0, sizeof(ext4_inode_t));
    memcpy(inode, raw, copy_size);
    ext4_release_block(fs, inode_block);
    return 0;
}

static int ext4_store_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode,
                            int clear) {
    uint64_t inode_block;
    size_t copy_size;
    uint8_t *raw;

    raw = ext4_inode_slot(fs, ino, &inode_block);
    if (!raw) return -1;
    copy_size = fs->inode_size;
    if (copy_size > sizeof(ext4_inode_t)) copy_size = sizeof(ext4_inode_t);
    if (clear) memset(raw, 0, fs->inode_size);
    memcpy(raw, inode, copy_size);
    ext4_inode_csum(fs, ino, raw);
    ext4_mark_block_dirty(fs, inode_block);
    ext4_release_block(fs, inode_block);
    return 0;
}

int ext4_write_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode) {
    return ext4_store_inode(fs, ino, inode, 0);
}

ext4_inode_cache_t *ext4_get_inode(ext4_fs_t *fs, uint32_t ino) {
    ext4_inode_cache_t *ic;

    ic = find_inode_cache(fs, ino);
    if (ic) {
        ic->ref_count++;
        return ic;
    }

    ic = find_free_inode_cache(fs);
    if (!ic) return NULL;
    ic->ino = 0;
    ic->vfs_node = NULL;
    if (ext4_read_inode(fs, ino, &ic->inode) != 0) return NULL;

    ic->ino = ino;
    ic->fs = fs;
    ic->ref_count = 1;
    ic->dirty = false;
    return ic;
}

void ext4_release_inode(ext4_inode_cache_t *ic) {
    if (ic && ic->ref_count > 0) ic->ref_count--;
}

void ext4_reclaim_inodes(ext4_fs_t *fs) {
    ext4_inode_cache_t **link;
    ext4_inode_cache_t *ic;

    if (!fs) return;
    link = &fs->inode_cache;
    while ((ic = *link)) {
        if (ic->ref_count == 0 && !ic->dirty) {
            *link = ic->next;
            kfree(ic);
        } else {
            link = &ic->next;
        }
    }
}

void ext4_drop_inode_cache(ext4_fs_t *fs) {
    ext4_inode_cache_t *ic;

    while ((ic = fs->inode_cache)) {
        fs->inode_cache = ic->next;
        kfree(ic);
    }
}

int ext4_sync_inodes(ext4_fs_t *fs) {
    ext4_inode_cache_t *ic;

    if (!fs) return -1;
    for (ic = fs->inode_cache; ic; ic = ic->next) {
        if (ic->dirty && ic->ino != 0) {
            if (ext4_write_inode(fs, ic->ino, &ic->inode) != 0) return -1;
            ic->dirty = false;
        }
    }
    return 0;
}

void ext4_mark_inode_dirty(ext4_inode_cache_t *ic) {
    if (ic) {
        ic->dirty = true;
    }
}

uint64_t ext4_inode_get_size(ext4_inode_t *inode) {
    uint64_t size = inode->i_size_lo;
    if ((inode->i_mode & 0xF000) == EXT4_S_IFREG) {
        size |= ((uint64_t)inode->i_size_high << 32);
    }
    return size;
}

void ext4_inode_set_size(ext4_inode_t *inode, uint64_t size) {
    inode->i_size_lo = (uint32_t)(size & 0xFFFFFFFF);
    inode->i_size_high = (uint32_t)(size >> 32);
}

static uint32_t ext4_extent_get_run(ext4_fs_t *fs, ext4_inode_t *inode,
                                    uint64_t logical_block,
                                    uint32_t max_blocks,
                                    uint64_t *physical_out) {
    ext4_extent_header_t *header;
    ext4_extent_t *extents;
    ext4_extent_idx_t *indices;
    uint8_t *block;
    uint64_t block_num;
    uint64_t next_block;
    uint64_t physical;
    uint64_t offset;
    uint64_t available;
    uint32_t capacity;
    uint32_t start;
    uint32_t length;
    uint32_t run;
    uint16_t depth;
    uint16_t i;
    int found;
    int unwritten;

    if (!physical_out || max_blocks == 0) return 0;
    *physical_out = 0;
    header = (ext4_extent_header_t *)inode->i_block;
    block = NULL;
    block_num = 0;
    depth = header->eh_depth;
    if (depth > 5) return 0;
    for (;;) {
        if (block) available = fs->block_size;
        else available = sizeof(inode->i_block);
        if (available < sizeof(*header)) break;
        available -= sizeof(*header);
        if (depth == 0)
            capacity = available / sizeof(ext4_extent_t);
        else
            capacity = available / sizeof(ext4_extent_idx_t);
        if (header->eh_magic != EXT4_EXTENT_MAGIC ||
            header->eh_depth != depth ||
            header->eh_entries > header->eh_max ||
            header->eh_max > capacity) break;
        if (depth == 0) {
            extents = (ext4_extent_t *)(header + 1);
            for (i = 0; i < header->eh_entries; i++) {
                start = extents[i].ee_block;
                length = extents[i].ee_len;
                unwritten = length > 32768;
                if (unwritten) length -= 32768;
                if (length == 0 || logical_block < start ||
                    logical_block - start >= length) continue;
                offset = logical_block - start;
                run = length - (uint32_t)offset;
                if (run > max_blocks) run = max_blocks;
                if (unwritten) {
                    if (block) ext4_release_block(fs, block_num);
                    return run;
                }
                physical = extents[i].ee_start_lo |
                           ((uint64_t)extents[i].ee_start_hi << 32);
                *physical_out = physical + offset;
                if (block) ext4_release_block(fs, block_num);
                return run;
            }
            break;
        }
        indices = (ext4_extent_idx_t *)(header + 1);
        found = -1;
        for (i = header->eh_entries; i > 0; i--) {
            if (logical_block >= indices[i - 1].ei_block) {
                found = (int)i - 1;
                break;
            }
        }
        if (found < 0) break;
        next_block = indices[found].ei_leaf_lo |
                     ((uint64_t)indices[found].ei_leaf_hi << 32);
        if (block) ext4_release_block(fs, block_num);
        block = ext4_get_block(fs, next_block);
        if (!block) return 0;
        block_num = next_block;
        header = (ext4_extent_header_t *)(void *)block;
        depth--;
    }
    if (block) ext4_release_block(fs, block_num);
    return 0;
}

static uint64_t ext4_extent_get_block(ext4_fs_t *fs, ext4_inode_t *inode,
                                      uint64_t logical_block) {
    uint64_t physical;

    if (ext4_extent_get_run(fs, inode, logical_block, 1, &physical) == 0)
        return 0;
    return physical;
}

static uint64_t ext4_indirect_get_block(ext4_fs_t *fs, ext4_inode_t *inode,
                                        uint64_t logical_block) {
    uint8_t *ind_block;
    uint8_t *dind_block;
    uint8_t *tind_block;
    uint32_t *ptrs;
    uint32_t *dptrs;
    uint32_t *tptrs;
    uint64_t result;
    uint32_t ptrs_per_block;
    uint32_t ind_idx;
    uint32_t ind_off;
    uint32_t dind_idx;
    uint32_t dind_off;
    uint32_t ind_block_num;
    uint32_t dind_block_num;

    ptrs_per_block = fs->block_size / 4;

    if (logical_block < EXT4_NDIR_BLOCKS) {
        return inode->i_block[logical_block];
    }

    logical_block -= EXT4_NDIR_BLOCKS;

    if (logical_block < ptrs_per_block) {
        if (inode->i_block[EXT4_IND_BLOCK] == 0) {
            return 0;
        }

        ind_block = ext4_get_block(fs, inode->i_block[EXT4_IND_BLOCK]);
        if (!ind_block) {
            return 0;
        }

        ptrs = (uint32_t *)ind_block;
        result = ptrs[logical_block];
        ext4_release_block(fs, inode->i_block[EXT4_IND_BLOCK]);
        return result;
    }

    logical_block -= ptrs_per_block;

    if (logical_block < ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[EXT4_DIND_BLOCK] == 0) {
            return 0;
        }

        dind_block = ext4_get_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
        if (!dind_block) {
            return 0;
        }

        dptrs = (uint32_t *)dind_block;
        ind_idx = logical_block / ptrs_per_block;
        ind_off = logical_block % ptrs_per_block;

        if (dptrs[ind_idx] == 0) {
            ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
            return 0;
        }

        ind_block_num = dptrs[ind_idx];
        ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);

        ind_block = ext4_get_block(fs, ind_block_num);
        if (!ind_block) {
            return 0;
        }

        ptrs = (uint32_t *)ind_block;
        result = ptrs[ind_off];
        ext4_release_block(fs, ind_block_num);
        return result;
    }

    logical_block -= ptrs_per_block * ptrs_per_block;

    if (inode->i_block[EXT4_TIND_BLOCK] == 0) {
        return 0;
    }

    tind_block = ext4_get_block(fs, inode->i_block[EXT4_TIND_BLOCK]);
    if (!tind_block) {
        return 0;
    }

    tptrs = (uint32_t *)tind_block;
    dind_idx = logical_block / (ptrs_per_block * ptrs_per_block);
    dind_off = logical_block % (ptrs_per_block * ptrs_per_block);

    if (tptrs[dind_idx] == 0) {
        ext4_release_block(fs, inode->i_block[EXT4_TIND_BLOCK]);
        return 0;
    }

    dind_block_num = tptrs[dind_idx];
    ext4_release_block(fs, inode->i_block[EXT4_TIND_BLOCK]);

    dind_block = ext4_get_block(fs, dind_block_num);
    if (!dind_block) {
        return 0;
    }

    dptrs = (uint32_t *)dind_block;
    ind_idx = dind_off / ptrs_per_block;
    ind_off = dind_off % ptrs_per_block;

    if (dptrs[ind_idx] == 0) {
        ext4_release_block(fs, dind_block_num);
        return 0;
    }

    ind_block_num = dptrs[ind_idx];
    ext4_release_block(fs, dind_block_num);

    ind_block = ext4_get_block(fs, ind_block_num);
    if (!ind_block) {
        return 0;
    }

    ptrs = (uint32_t *)ind_block;
    result = ptrs[ind_off];
    ext4_release_block(fs, ind_block_num);
    return result;
}

uint64_t ext4_inode_get_block(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t logical_block) {
    if (inode->i_flags & EXT4_INODE_FLAG_EXTENTS) {
        return ext4_extent_get_block(fs, inode, logical_block);
    } else {
        return ext4_indirect_get_block(fs, inode, logical_block);
    }
}

#define DESC_GET(fs, d, f) ((d).f##_lo | \
    ((fs)->desc_size >= 64 ? (uint32_t)(d).f##_hi << 16 : 0))
#define DESC_SET(fs, d, f, v) do { \
    uint32_t v_ = (v); \
    (d).f##_lo = (uint16_t)v_; \
    if ((fs)->desc_size >= 64) (d).f##_hi = (uint16_t)(v_ >> 16); \
} while (0)

static uint64_t ext4_inode_bitmap_block(ext4_fs_t *fs, ext4_group_desc_t *desc) {
    return desc->bg_inode_bitmap_lo |
           (fs->is_64bit ? (uint64_t)desc->bg_inode_bitmap_hi << 32 : 0);
}

int ext4_alloc_inode(ext4_fs_t *fs, uint16_t mode) {
    uint32_t group;
    uint32_t bit;
    uint32_t ino;
    uint32_t count;
    uint64_t bitmap_block;
    uint8_t *bitmap;
    ext4_group_desc_t desc;
    ext4_inode_t new_inode;
    uint32_t now;
    int uninit_bg;

    uninit_bg = fs->metadata_csum || fs->gdt_csum;
    for (group = 0; group < fs->groups_count; group++) {
        if (ext4_read_group_desc(fs, group, &desc) != 0) continue;
        if (DESC_GET(fs, desc, bg_free_inodes_count) == 0) continue;

        bitmap_block = ext4_inode_bitmap_block(fs, &desc);
        bitmap = ext4_get_block(fs, bitmap_block);
        if (!bitmap) continue;
        if (uninit_bg && (desc.bg_flags & EXT4_BG_INODE_UNINIT)) {
            memset(bitmap, 0, fs->block_size);
            for (bit = fs->sb.s_inodes_per_group; bit < fs->block_size * 8; bit++)
                bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
            desc.bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
        }

        for (bit = 0; bit < fs->sb.s_inodes_per_group; bit++) {
            if (bit % 8 == 0 && bitmap[bit / 8] == 0xFF) {
                bit += 7;
                continue;
            }
            if (bitmap[bit / 8] & (1u << (bit % 8))) continue;

            bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
            ext4_mark_block_dirty(fs, bitmap_block);
            ext4_bitmap_csum(fs, &desc, bitmap, 1);
            ext4_release_block(fs, bitmap_block);

            count = DESC_GET(fs, desc, bg_free_inodes_count);
            DESC_SET(fs, desc, bg_free_inodes_count, count - 1);
            if ((mode & 0xF000) == EXT4_S_IFDIR) {
                count = DESC_GET(fs, desc, bg_used_dirs_count);
                DESC_SET(fs, desc, bg_used_dirs_count, count + 1);
            }
            if (uninit_bg) {
                count = fs->sb.s_inodes_per_group -
                        DESC_GET(fs, desc, bg_itable_unused);
                if (bit >= count)
                    DESC_SET(fs, desc, bg_itable_unused,
                             fs->sb.s_inodes_per_group - bit - 1);
            }
            ext4_write_group_desc(fs, group, &desc);

            fs->sb.s_free_inodes_count--;
            fs->super_dirty = true;

            ino = group * fs->sb.s_inodes_per_group + bit + 1;
            memset(&new_inode, 0, sizeof(ext4_inode_t));
            new_inode.i_mode = mode;
            new_inode.i_links_count = 1;
            if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
                count = fs->sb.s_want_extra_isize ? fs->sb.s_want_extra_isize :
                        sizeof(ext4_inode_t) - EXT4_GOOD_OLD_INODE_SIZE;
                if (count > fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE)
                    count = fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE;
                new_inode.i_extra_isize = (uint16_t)count;
            }
            now = (uint32_t)(timekeeping_realtime_ns() / 1000000000ULL);
            new_inode.i_atime = now;
            new_inode.i_mtime = now;
            new_inode.i_ctime = now;
            new_inode.i_crtime = now;
            ext4_invalidate_inode_cache(fs, ino);
            if (ext4_store_inode(fs, ino, &new_inode, 1) != 0) return -1;

            return (int)ino;
        }

        ext4_release_block(fs, bitmap_block);
    }

    return -1;
}

int ext4_free_inode(ext4_fs_t *fs, uint32_t ino) {
    ext4_inode_t inode;
    ext4_inode_cache_t *ic;
    uint32_t group;
    uint32_t bit;
    uint32_t count;
    uint64_t bitmap_block;
    uint8_t *bitmap;
    ext4_group_desc_t desc;

    if (ino == 0 || ino > fs->total_inodes) return -1;
    if (ext4_read_inode(fs, ino, &inode) != 0) return -1;

    group = (ino - 1) / fs->sb.s_inodes_per_group;
    bit = (ino - 1) % fs->sb.s_inodes_per_group;
    if (ext4_read_group_desc(fs, group, &desc) != 0) return -1;

    bitmap_block = ext4_inode_bitmap_block(fs, &desc);
    bitmap = ext4_get_block(fs, bitmap_block);
    if (!bitmap) return -1;
    if (!(bitmap[bit / 8] & (1u << (bit % 8)))) {
        ext4_release_block(fs, bitmap_block);
        return -1;
    }

    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    ext4_mark_block_dirty(fs, bitmap_block);
    ext4_bitmap_csum(fs, &desc, bitmap, 1);
    ext4_release_block(fs, bitmap_block);

    count = DESC_GET(fs, desc, bg_free_inodes_count);
    DESC_SET(fs, desc, bg_free_inodes_count, count + 1);
    if ((inode.i_mode & 0xF000) == EXT4_S_IFDIR) {
        count = DESC_GET(fs, desc, bg_used_dirs_count);
        if (count > 0)
            DESC_SET(fs, desc, bg_used_dirs_count, count - 1);
    }
    ext4_write_group_desc(fs, group, &desc);

    fs->sb.s_free_inodes_count++;
    fs->super_dirty = true;
    ic = find_inode_cache(fs, ino);
    if (ic && ic->dirty) inode = ic->inode;
    ext4_invalidate_inode_cache(fs, ino);
    inode.i_links_count = 0;
    inode.i_dtime = (uint32_t)(timekeeping_realtime_ns() / 1000000000ULL);
    ext4_write_inode(fs, ino, &inode);

    return 0;
}

uint8_t ext4_mode_to_type(uint16_t mode) {
    switch (mode & 0xF000) {
        case EXT4_S_IFREG:  return EXT4_FT_REG_FILE;
        case EXT4_S_IFDIR:  return EXT4_FT_DIR;
        case EXT4_S_IFCHR:  return EXT4_FT_CHRDEV;
        case EXT4_S_IFBLK:  return EXT4_FT_BLKDEV;
        case EXT4_S_IFIFO:  return EXT4_FT_FIFO;
        case EXT4_S_IFSOCK: return EXT4_FT_SOCK;
        case EXT4_S_IFLNK:  return EXT4_FT_SYMLINK;
        default:           return EXT4_FT_UNKNOWN;
    }
}

uint8_t ext4_type_to_vfs(uint8_t ext4_type) {
    switch (ext4_type) {
        case EXT4_FT_REG_FILE: return VFS_FILE;
        case EXT4_FT_DIR:      return VFS_DIRECTORY;
        case EXT4_FT_CHRDEV:   return VFS_CHARDEVICE;
        case EXT4_FT_BLKDEV:   return VFS_BLOCKDEVICE;
        case EXT4_FT_FIFO:     return VFS_PIPE;
        case EXT4_FT_SOCK:     return VFS_SOCKET;
        case EXT4_FT_SYMLINK:  return VFS_SYMLINK;
        default:              return VFS_FILE;
    }
}

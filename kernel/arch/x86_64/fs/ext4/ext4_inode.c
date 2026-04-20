#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

extern int ext4_load_group_descs(ext4_fs_t *fs);
extern void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block);

static int find_inode_cache(ext4_fs_t *fs, uint32_t ino) {
    int i;

    for (i = 0; i < (int)fs->inode_cache_count; i++) {
        if (fs->inode_cache[i].ino == ino && fs->inode_cache[i].ref_count > 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_inode_cache(ext4_fs_t *fs) {
    int i;
    int best;
    uint32_t new_cap;
    ext4_inode_cache_t *new_cache;

    for (i = 0; i < (int)fs->inode_cache_count; i++) {
        if (fs->inode_cache[i].ref_count == 0 && !fs->inode_cache[i].dirty) {
            return i;
        }
    }

    best = -1;
    for (i = 0; i < (int)fs->inode_cache_count; i++) {
        if (fs->inode_cache[i].ref_count == 0 && fs->inode_cache[i].dirty) {
            ext4_write_inode(fs, fs->inode_cache[i].ino, &fs->inode_cache[i].inode);
            fs->inode_cache[i].dirty = false;
            best = i;
            break;
        }
    }
    if (best >= 0) return best;
    if (fs->inode_cache_count < fs->inode_cache_capacity) {
        i = (int)fs->inode_cache_count;
        fs->inode_cache_count++;
        return i;
    }
    if (fs->inode_cache_capacity < EXT4_INODE_CACHE_MAX) {
        new_cap = fs->inode_cache_capacity * 2;
        if (new_cap > EXT4_INODE_CACHE_MAX)
            new_cap = EXT4_INODE_CACHE_MAX;
        new_cache = (ext4_inode_cache_t *)kmalloc(new_cap * sizeof(ext4_inode_cache_t));
        if (new_cache) {
            memcpy(new_cache, fs->inode_cache, fs->inode_cache_count * sizeof(ext4_inode_cache_t));
            memset(new_cache + fs->inode_cache_count, 0, (new_cap - fs->inode_cache_count) * sizeof(ext4_inode_cache_t));
            i = (int)fs->inode_cache_count;
            kfree(fs->inode_cache);
            fs->inode_cache = new_cache;
            fs->inode_cache_capacity = new_cap;
            fs->inode_cache_count++;
            return i;
        }
    }
    return -1;
}

int ext4_read_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode) {
    uint32_t group;
    uint32_t index;
    uint64_t inode_table;
    uint64_t inode_block;
    uint32_t inode_offset;
    uint8_t *block;

    if (ino == 0 || ino > fs->total_inodes) {
        return -1;
    }

    group = (ino - 1) / fs->sb.s_inodes_per_group;
    index = (ino - 1) % fs->sb.s_inodes_per_group;

    if (!fs->group_descs) {
        if (ext4_load_group_descs(fs) != 0) {
            return -1;
        }
    }

    inode_table = fs->group_descs[group].bg_inode_table_lo;
    if (fs->is_64bit) {
        inode_table |= ((uint64_t)fs->group_descs[group].bg_inode_table_hi << 32);
    }

    inode_block = inode_table + (index * fs->inode_size) / fs->block_size;
    inode_offset = (index * fs->inode_size) % fs->block_size;

    block = ext4_get_block(fs, inode_block);
    if (!block) {
        return -1;
    }

    memcpy(inode, block + inode_offset, sizeof(ext4_inode_t));
    ext4_release_block(fs, inode_block);

    return 0;
}

int ext4_write_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode) {
    uint32_t group;
    uint32_t index;
    uint64_t inode_table;
    uint64_t inode_block;
    uint32_t inode_offset;
    uint8_t *block;

    if (ino == 0 || ino > fs->total_inodes) {
        return -1;
    }

    group = (ino - 1) / fs->sb.s_inodes_per_group;
    index = (ino - 1) % fs->sb.s_inodes_per_group;

    if (!fs->group_descs) {
        if (ext4_load_group_descs(fs) != 0) {
            return -1;
        }
    }

    inode_table = fs->group_descs[group].bg_inode_table_lo;
    if (fs->is_64bit) {
        inode_table |= ((uint64_t)fs->group_descs[group].bg_inode_table_hi << 32);
    }

    inode_block = inode_table + (index * fs->inode_size) / fs->block_size;
    inode_offset = (index * fs->inode_size) % fs->block_size;

    block = ext4_get_block(fs, inode_block);
    if (!block) {
        return -1;
    }

    memcpy(block + inode_offset, inode, sizeof(ext4_inode_t));
    ext4_mark_block_dirty(fs, inode_block);
    ext4_release_block(fs, inode_block);

    return 0;
}

ext4_inode_cache_t *ext4_get_inode(ext4_fs_t *fs, uint32_t ino) {
    int idx;

    idx = find_inode_cache(fs, ino);
    
    if (idx >= 0) {
        fs->inode_cache[idx].ref_count++;
        return &fs->inode_cache[idx];
    }

    idx = find_free_inode_cache(fs);
    if (idx < 0) {
        return NULL;
    }

    if (ext4_read_inode(fs, ino, &fs->inode_cache[idx].inode) != 0) {
        return NULL;
    }

    fs->inode_cache[idx].ino = ino;
    fs->inode_cache[idx].fs = fs;
    fs->inode_cache[idx].ref_count = 1;
    fs->inode_cache[idx].dirty = false;
    fs->inode_cache[idx].vfs_node = NULL;

    return &fs->inode_cache[idx];
}

void ext4_release_inode(ext4_inode_cache_t *ic) {
    if (!ic) return;

    if (ic->ref_count > 0) {
        ic->ref_count--;
    }

    if (ic->ref_count == 0 && ic->dirty) {
        ext4_write_inode(ic->fs, ic->ino, &ic->inode);
        ic->dirty = false;
    }
}

void ext4_sync_inodes(ext4_fs_t *fs) {
    int i;

    if (!fs) return;
    for (i = 0; i < (int)fs->inode_cache_count; i++) {
        if (fs->inode_cache[i].dirty && fs->inode_cache[i].ino != 0) {
            ext4_write_inode(fs, fs->inode_cache[i].ino, &fs->inode_cache[i].inode);
            fs->inode_cache[i].dirty = false;
        }
    }
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

static uint64_t ext4_extent_get_block(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t logical_block) {
    ext4_extent_header_t *header = (ext4_extent_header_t *)inode->i_block;
    
    if (header->eh_magic != EXT4_EXTENT_MAGIC) {
        return 0;
    }

    uint8_t *block_buf = NULL;

    while (1) {
        if (header->eh_depth == 0) {
            ext4_extent_t *extents = (ext4_extent_t *)(header + 1);
            for (uint16_t i = 0; i < header->eh_entries; i++) {
                uint32_t start = extents[i].ee_block;
                uint16_t len = extents[i].ee_len;
                
                if (len > 32768) {
                    len = len - 32768;
                }

                if (logical_block >= start && logical_block < start + len) {
                    uint64_t phys_start = extents[i].ee_start_lo;
                    phys_start |= ((uint64_t)extents[i].ee_start_hi << 32);
                    
                    if (block_buf) {
                        ext4_release_block(fs, 0);
                        kfree(block_buf);
                    }
                    
                    return phys_start + (logical_block - start);
                }
            }
            
            if (block_buf) {
                kfree(block_buf);
            }
            return 0;
        } else {
            ext4_extent_idx_t *indices = (ext4_extent_idx_t *)(header + 1);
            int found = -1;
            
            for (int i = header->eh_entries - 1; i >= 0; i--) {
                if (logical_block >= indices[i].ei_block) {
                    found = i;
                    break;
                }
            }

            if (found < 0) {
                if (block_buf) {
                    kfree(block_buf);
                }
                return 0;
            }

            uint64_t next_block = indices[found].ei_leaf_lo;
            next_block |= ((uint64_t)indices[found].ei_leaf_hi << 32);

            if (block_buf) {
                kfree(block_buf);
            }
            
            block_buf = (uint8_t *)kmalloc(fs->block_size);
            if (!block_buf) {
                return 0;
            }

            if (ext4_read_block(fs, next_block, block_buf) != 0) {
                kfree(block_buf);
                return 0;
            }

            header = (ext4_extent_header_t *)block_buf;
            
            if (header->eh_magic != EXT4_EXTENT_MAGIC) {
                kfree(block_buf);
                return 0;
            }
        }
    }
}

static uint64_t ext4_indirect_get_block(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t logical_block) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (logical_block < EXT4_NDIR_BLOCKS) {
        return inode->i_block[logical_block];
    }

    logical_block -= EXT4_NDIR_BLOCKS;

    if (logical_block < ptrs_per_block) {
        if (inode->i_block[EXT4_IND_BLOCK] == 0) {
            return 0;
        }

        uint8_t *ind_block = ext4_get_block(fs, inode->i_block[EXT4_IND_BLOCK]);
        if (!ind_block) {
            return 0;
        }

        uint32_t *ptrs = (uint32_t *)ind_block;
        uint64_t result = ptrs[logical_block];
        ext4_release_block(fs, inode->i_block[EXT4_IND_BLOCK]);
        return result;
    }

    logical_block -= ptrs_per_block;

    if (logical_block < ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[EXT4_DIND_BLOCK] == 0) {
            return 0;
        }

        uint8_t *dind_block = ext4_get_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
        if (!dind_block) {
            return 0;
        }

        uint32_t *dptrs = (uint32_t *)dind_block;
        uint32_t ind_idx = logical_block / ptrs_per_block;
        uint32_t ind_off = logical_block % ptrs_per_block;

        if (dptrs[ind_idx] == 0) {
            ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
            return 0;
        }

        uint32_t ind_block_num = dptrs[ind_idx];
        ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);

        uint8_t *ind_block = ext4_get_block(fs, ind_block_num);
        if (!ind_block) {
            return 0;
        }

        uint32_t *ptrs = (uint32_t *)ind_block;
        uint64_t result = ptrs[ind_off];
        ext4_release_block(fs, ind_block_num);
        return result;
    }

    logical_block -= ptrs_per_block * ptrs_per_block;

    if (inode->i_block[EXT4_TIND_BLOCK] == 0) {
        return 0;
    }

    uint8_t *tind_block = ext4_get_block(fs, inode->i_block[EXT4_TIND_BLOCK]);
    if (!tind_block) {
        return 0;
    }

    uint32_t *tptrs = (uint32_t *)tind_block;
    uint32_t dind_idx = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t dind_off = logical_block % (ptrs_per_block * ptrs_per_block);

    if (tptrs[dind_idx] == 0) {
        ext4_release_block(fs, inode->i_block[EXT4_TIND_BLOCK]);
        return 0;
    }

    uint32_t dind_block_num = tptrs[dind_idx];
    ext4_release_block(fs, inode->i_block[EXT4_TIND_BLOCK]);

    uint8_t *dind_block = ext4_get_block(fs, dind_block_num);
    if (!dind_block) {
        return 0;
    }

    uint32_t *dptrs = (uint32_t *)dind_block;
    uint32_t ind_idx = dind_off / ptrs_per_block;
    uint32_t ind_off = dind_off % ptrs_per_block;

    if (dptrs[ind_idx] == 0) {
        ext4_release_block(fs, dind_block_num);
        return 0;
    }

    uint32_t ind_block_num = dptrs[ind_idx];
    ext4_release_block(fs, dind_block_num);

    uint8_t *ind_block = ext4_get_block(fs, ind_block_num);
    if (!ind_block) {
        return 0;
    }

    uint32_t *ptrs = (uint32_t *)ind_block;
    uint64_t result = ptrs[ind_off];
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

static int ext4_read_group_desc_internal(ext4_fs_t *fs, uint32_t group, ext4_group_desc_t *desc) {
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

static int ext4_write_group_desc_internal(ext4_fs_t *fs, uint32_t group, ext4_group_desc_t *desc) {
    uint32_t desc_block = fs->first_data_block + 1 + (group * fs->desc_size) / fs->block_size;
    uint32_t desc_offset = (group * fs->desc_size) % fs->block_size;

    uint8_t *block = ext4_get_block(fs, desc_block);
    if (!block) {
        return -1;
    }

    memcpy(block + desc_offset, desc, sizeof(ext4_group_desc_t));
    ext4_mark_block_dirty(fs, desc_block);
    ext4_release_block(fs, desc_block);

    if (fs->group_descs) {
        memcpy(&fs->group_descs[group], desc, sizeof(ext4_group_desc_t));
    }

    return 0;
}

int ext4_alloc_inode(ext4_fs_t *fs, uint16_t mode) {
    for (uint32_t group = 0; group < fs->groups_count; group++) {
        ext4_group_desc_t desc;

        if (ext4_read_group_desc_internal(fs, group, &desc) != 0) {
            continue;
        }

        if (desc.bg_free_inodes_count_lo == 0) {
            continue;
        }

        uint64_t bitmap_block = desc.bg_inode_bitmap_lo;
        if (fs->is_64bit) {
            bitmap_block |= ((uint64_t)desc.bg_inode_bitmap_hi << 32);
        }

        uint8_t *bitmap = ext4_get_block(fs, bitmap_block);
        if (!bitmap) {
            continue;
        }

        for (uint32_t bit = 0; bit < fs->sb.s_inodes_per_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint32_t bit_idx = bit % 8;

            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] |= (1 << bit_idx);
                ext4_mark_block_dirty(fs, bitmap_block);
                ext4_release_block(fs, bitmap_block);

                desc.bg_free_inodes_count_lo--;
                if ((mode & EXT4_S_IFDIR) == EXT4_S_IFDIR) {
                    desc.bg_used_dirs_count_lo++;
                }
                ext4_write_group_desc_internal(fs, group, &desc);

                fs->sb.s_free_inodes_count--;
                ext4_write_superblock(fs);

                uint32_t ino = group * fs->sb.s_inodes_per_group + bit + 1;

                ext4_inode_t new_inode;
                memset(&new_inode, 0, sizeof(ext4_inode_t));
                new_inode.i_mode = mode;
                new_inode.i_links_count = 1;
                ext4_write_inode(fs, ino, &new_inode);

                return ino;
            }
        }

        ext4_release_block(fs, bitmap_block);
    }

    return -1;
}

int ext4_free_inode(ext4_fs_t *fs, uint32_t ino) {
    if (ino == 0 || ino > fs->total_inodes) {
        return -1;
    }

    ext4_inode_t inode;
    if (ext4_read_inode(fs, ino, &inode) != 0) {
        return -1;
    }

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t bit = (ino - 1) % fs->sb.s_inodes_per_group;

    ext4_group_desc_t desc;
    if (ext4_read_group_desc_internal(fs, group, &desc) != 0) {
        return -1;
    }

    uint64_t bitmap_block = desc.bg_inode_bitmap_lo;
    if (fs->is_64bit) {
        bitmap_block |= ((uint64_t)desc.bg_inode_bitmap_hi << 32);
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

    desc.bg_free_inodes_count_lo++;
    if ((inode.i_mode & 0xF000) == EXT4_S_IFDIR) {
        if (desc.bg_used_dirs_count_lo > 0) {
            desc.bg_used_dirs_count_lo--;
        }
    }
    ext4_write_group_desc_internal(fs, group, &desc);

    fs->sb.s_free_inodes_count++;
    ext4_write_superblock(fs);

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
        case EXT4_FT_SOCK:     return VFS_PIPE;
        case EXT4_FT_SYMLINK:  return VFS_SYMLINK;
        default:              return VFS_FILE;
    }
}

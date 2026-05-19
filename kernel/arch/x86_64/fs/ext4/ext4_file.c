#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

extern void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block);
extern void ext4_mark_inode_dirty(ext4_inode_cache_t *ic);

uint32_t ext4_file_read(ext4_fs_t *fs, uint32_t ino, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ext4_inode_cache_t *ic;
    uint64_t file_size;
    uint32_t bytes_read;
    uint64_t block_num;
    uint32_t block_off;
    uint32_t to_read;
    uint64_t phys_block;
    uint8_t *block;

    if (!buffer || size == 0) {
        return 0;
    }

    ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return 0;
    }

    file_size = ext4_inode_get_size(&ic->inode);

    if (offset >= file_size) {
        ext4_release_inode(ic);
        return 0;
    }

    if (offset + size > file_size) {
        size = file_size - offset;
    }

    bytes_read = 0;

    while (bytes_read < size) {
        block_num = (offset + bytes_read) / fs->block_size;
        block_off = (offset + bytes_read) % fs->block_size;
        to_read = fs->block_size - block_off;

        if (to_read > size - bytes_read) {
            to_read = size - bytes_read;
        }

        phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);

        if (phys_block == 0) {
            memset(buffer + bytes_read, 0, to_read);
        } else {
            block = ext4_get_block(fs, phys_block);
            if (!block) {
                break;
            }

            memcpy(buffer + bytes_read, block + block_off, to_read);
            ext4_release_block(fs, phys_block);
        }

        bytes_read += to_read;
    }

    ext4_release_inode(ic);
    return bytes_read;
}

static int ext4_inode_set_block(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t logical_block, uint64_t phys_block) {
    uint32_t ptrs_per_block;
    uint8_t *ind_block;
    uint32_t *ptrs;
    int new_block;
    uint8_t *zero_block;
    uint8_t *dind_block;
    uint32_t *dptrs;
    uint32_t ind_idx;
    uint32_t ind_off;
    uint32_t ind_block_num;

    if (inode->i_flags & EXT4_INODE_FLAG_EXTENTS) {
        return -1;
    }

    ptrs_per_block = fs->block_size / 4;

    if (logical_block < EXT4_NDIR_BLOCKS) {
        inode->i_block[logical_block] = (uint32_t)phys_block;
        return 0;
    }

    logical_block -= EXT4_NDIR_BLOCKS;

    if (logical_block < ptrs_per_block) {
        if (inode->i_block[EXT4_IND_BLOCK] == 0) {
            new_block = ext4_alloc_block(fs, 0);
            if (new_block < 0) {
                return -1;
            }
            inode->i_block[EXT4_IND_BLOCK] = new_block;
            
            zero_block = (uint8_t *)kmalloc(fs->block_size);
            if (zero_block) {
                memset(zero_block, 0, fs->block_size);
                ext4_write_block(fs, new_block, zero_block);
                kfree(zero_block);
            }
        }

        ind_block = ext4_get_block(fs, inode->i_block[EXT4_IND_BLOCK]);
        if (!ind_block) {
            return -1;
        }

        ptrs = (uint32_t *)ind_block;
        ptrs[logical_block] = (uint32_t)phys_block;
        ext4_mark_block_dirty(fs, inode->i_block[EXT4_IND_BLOCK]);
        ext4_release_block(fs, inode->i_block[EXT4_IND_BLOCK]);
        return 0;
    }

    logical_block -= ptrs_per_block;

    if (logical_block < (uint64_t)ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[EXT4_DIND_BLOCK] == 0) {
            new_block = ext4_alloc_block(fs, 0);
            if (new_block < 0) {
                return -1;
            }
            inode->i_block[EXT4_DIND_BLOCK] = new_block;

            zero_block = (uint8_t *)kmalloc(fs->block_size);
            if (zero_block) {
                memset(zero_block, 0, fs->block_size);
                ext4_write_block(fs, new_block, zero_block);
                kfree(zero_block);
            }
        }

        dind_block = ext4_get_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
        if (!dind_block) {
            return -1;
        }

        dptrs = (uint32_t *)dind_block;
        ind_idx = logical_block / ptrs_per_block;
        ind_off = logical_block % ptrs_per_block;

        if (dptrs[ind_idx] == 0) {
            new_block = ext4_alloc_block(fs, 0);
            if (new_block < 0) {
                ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
                return -1;
            }
            dptrs[ind_idx] = new_block;
            ext4_mark_block_dirty(fs, inode->i_block[EXT4_DIND_BLOCK]);

            zero_block = (uint8_t *)kmalloc(fs->block_size);
            if (zero_block) {
                memset(zero_block, 0, fs->block_size);
                ext4_write_block(fs, new_block, zero_block);
                kfree(zero_block);
            }
        }

        ind_block_num = dptrs[ind_idx];
        ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);

        ind_block = ext4_get_block(fs, ind_block_num);
        if (!ind_block) {
            return -1;
        }

        ptrs = (uint32_t *)ind_block;
        ptrs[ind_off] = (uint32_t)phys_block;
        ext4_mark_block_dirty(fs, ind_block_num);
        ext4_release_block(fs, ind_block_num);
        return 0;
    }

    return -1;
}

uint32_t ext4_file_write(ext4_fs_t *fs, uint32_t ino, uint32_t offset, uint32_t size, const uint8_t *buffer) {
    ext4_inode_cache_t *ic;
    uint64_t file_size;
    uint64_t new_size;
    uint64_t current_blocks;
    uint64_t needed_blocks;
    uint64_t i;
    uint64_t existing;
    int new_block;
    uint32_t bytes_written;
    uint64_t block_num;
    uint32_t block_off;
    uint32_t to_write;
    uint64_t phys_block;
    uint8_t *block;
    int cache_idx;

    if (!buffer || size == 0) {
        return 0;
    }

    ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return 0;
    }

    cache_idx = -1;
    for (i = 0; i < fs->inode_cache_count; i++) {
        if (&fs->inode_cache[i] == ic) {
            cache_idx = (int)i;
            break;
        }
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFREG) {
        ext4_release_inode(ic);
        return 0;
    }

    file_size = ext4_inode_get_size(&ic->inode);
    new_size = offset + size;

    if (new_size > file_size) {
        current_blocks = (file_size + fs->block_size - 1) / fs->block_size;
        needed_blocks = (new_size + fs->block_size - 1) / fs->block_size;

        for (i = current_blocks; i < needed_blocks; i++) {
            existing = ext4_inode_get_block(fs, &ic->inode, i);
            if (existing == 0) {
                new_block = ext4_alloc_block(fs, 0);
                if (new_block < 0) {
                    ext4_release_inode(ic);
                    return 0;
                }

                if (ext4_inode_set_block(fs, &ic->inode, i, new_block) != 0) {
                    ext4_free_block(fs, new_block);
                    ext4_release_inode(ic);
                    return 0;
                }

                ic->inode.i_blocks_lo += fs->sectors_per_block;
            }
        }

        if (cache_idx >= 0 && &fs->inode_cache[cache_idx] != ic) {
            ic = &fs->inode_cache[cache_idx];
        }
    }

    bytes_written = 0;

    while (bytes_written < size) {
        block_num = (offset + bytes_written) / fs->block_size;
        block_off = (offset + bytes_written) % fs->block_size;
        to_write = fs->block_size - block_off;

        if (to_write > size - bytes_written) {
            to_write = size - bytes_written;
        }

        phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);

        if (phys_block == 0) {
            new_block = ext4_alloc_block(fs, 0);
            if (new_block < 0) {
                break;
            }

            if (ext4_inode_set_block(fs, &ic->inode, block_num, new_block) != 0) {
                ext4_free_block(fs, new_block);
                break;
            }

            ic->inode.i_blocks_lo += fs->sectors_per_block;
            phys_block = new_block;
        }

        if (block_off == 0 && to_write == fs->block_size) {
            block = ext4_get_block_overwrite(fs, phys_block);
        } else {
            block = ext4_get_block(fs, phys_block);
        }
        if (!block) {
            break;
        }

        memcpy(block + block_off, buffer + bytes_written, to_write);
        ext4_mark_block_dirty(fs, phys_block);
        ext4_release_block(fs, phys_block);

        bytes_written += to_write;
    }

    if (offset + bytes_written > file_size) {
        ext4_inode_set_size(&ic->inode, offset + bytes_written);
    }

    ext4_mark_inode_dirty(ic);
    ext4_release_inode(ic);

    return bytes_written;
}

int ext4_file_truncate(ext4_fs_t *fs, uint32_t ino, uint64_t new_size) {
    ext4_inode_cache_t *ic;
    uint64_t old_size;
    uint64_t new_blocks;
    uint64_t old_blocks;
    uint64_t i;
    uint64_t phys_block;

    ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFREG) {
        ext4_release_inode(ic);
        return -1;
    }

    old_size = ext4_inode_get_size(&ic->inode);

    if (new_size == old_size) {
        ext4_release_inode(ic);
        return 0;
    }

    if (new_size < old_size) {
        new_blocks = (new_size + fs->block_size - 1) / fs->block_size;
        old_blocks = (old_size + fs->block_size - 1) / fs->block_size;

        for (i = new_blocks; i < old_blocks; i++) {
            phys_block = ext4_inode_get_block(fs, &ic->inode, i);
            if (phys_block != 0) {
                ext4_free_block(fs, phys_block);

                if (!(ic->inode.i_flags & EXT4_INODE_FLAG_EXTENTS) && i < EXT4_NDIR_BLOCKS) {
                    ic->inode.i_block[i] = 0;
                }

                if (ic->inode.i_blocks_lo >= fs->sectors_per_block) {
                    ic->inode.i_blocks_lo -= fs->sectors_per_block;
                }
            }
        }
    }

    ext4_inode_set_size(&ic->inode, new_size);
    ext4_mark_inode_dirty(ic);
    ext4_release_inode(ic);

    return 0;
}

int ext4_create_file(ext4_fs_t *fs, uint32_t parent_ino, const char *name, uint16_t mode) {
    uint32_t existing;
    int new_ino;
    uint8_t file_type;

    if (!name) {
        return -1;
    }

    if (ext4_dir_lookup(fs, parent_ino, name, &existing) == 0) {
        return -1;
    }

    new_ino = ext4_alloc_inode(fs, mode);
    if (new_ino < 0) {
        return -1;
    }

    file_type = ext4_mode_to_type(mode);

    if (ext4_dir_add_entry(fs, parent_ino, name, new_ino, file_type) != 0) {
        ext4_free_inode(fs, new_ino);
        return -1;
    }

    return new_ino;
}

int ext4_unlink_file(ext4_fs_t *fs, uint32_t parent_ino, const char *name) {
    uint32_t ino;
    ext4_inode_cache_t *ic;
    uint64_t file_size;
    uint64_t blocks;
    uint64_t i;
    uint64_t phys_block;

    if (!name) {
        return -1;
    }

    if (ext4_dir_lookup(fs, parent_ino, name, &ino) != 0) {
        return -1;
    }

    ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) == EXT4_S_IFDIR) {
        ext4_release_inode(ic);
        return -1;
    }

    ic->inode.i_links_count--;

    if (ic->inode.i_links_count == 0) {
        file_size = ext4_inode_get_size(&ic->inode);
        blocks = (file_size + fs->block_size - 1) / fs->block_size;

        for (i = 0; i < blocks; i++) {
            phys_block = ext4_inode_get_block(fs, &ic->inode, i);
            if (phys_block != 0) {
                ext4_free_block(fs, phys_block);
            }
        }

        ext4_release_inode(ic);
        ext4_free_inode(fs, ino);
    } else {
        ext4_mark_inode_dirty(ic);
        ext4_release_inode(ic);
    }

    return ext4_dir_remove_entry(fs, parent_ino, name);
}

int ext4_rename_file(ext4_fs_t *fs, uint32_t old_parent_ino, const char *old_name,
                     uint32_t new_parent_ino, const char *new_name) {
    uint32_t ino;
    uint32_t existing_ino;
    ext4_inode_cache_t *ic;
    uint8_t file_type;

    if (!old_name || !new_name) {
        return -1;
    }

    if (ext4_dir_lookup(fs, old_parent_ino, old_name, &ino) != 0) {
        return -1;
    }

    ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return -1;
    }
    file_type = ext4_mode_to_type(ic->inode.i_mode);
    ext4_release_inode(ic);

    if (ext4_dir_lookup(fs, new_parent_ino, new_name, &existing_ino) == 0) {
        ext4_unlink_file(fs, new_parent_ino, new_name);
    }

    if (ext4_dir_add_entry(fs, new_parent_ino, new_name, ino, file_type) != 0) {
        return -1;
    }

    if (ext4_dir_remove_entry(fs, old_parent_ino, old_name) != 0) {
        return -1;
    }

    return 0;
}

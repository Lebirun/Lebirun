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
    uint8_t *inline_data;

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

    if (size > file_size - offset) {
        size = file_size - offset;
    }

    bytes_read = 0;

    if ((ic->inode.i_mode & 0xF000) == EXT4_S_IFLNK &&
        file_size <= sizeof(ic->inode.i_block) &&
        ic->inode.i_blocks_lo == 0) {
        inline_data = (uint8_t *)ic->inode.i_block;
        memcpy(buffer, inline_data + offset, size);
        ext4_release_inode(ic);
        return size;
    }

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
    int64_t new_block;
    uint8_t *dind_block;
    uint32_t *dptrs;
    uint32_t ind_idx;
    uint32_t ind_off;
    uint32_t ind_block_num;

    if (inode->i_flags & EXT4_INODE_FLAG_EXTENTS) {
        return -1;
    }
    if (phys_block > UINT32_MAX) return -1;

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
            if ((uint64_t)new_block > UINT32_MAX) {
                ext4_free_block(fs, (uint64_t)new_block);
                return -1;
            }
            inode->i_block[EXT4_IND_BLOCK] = (uint32_t)new_block;
            if (ext4_zero_block(fs, (uint64_t)new_block) != 0) {
                inode->i_block[EXT4_IND_BLOCK] = 0;
                ext4_free_block(fs, (uint64_t)new_block);
                return -1;
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
            if ((uint64_t)new_block > UINT32_MAX) {
                ext4_free_block(fs, (uint64_t)new_block);
                return -1;
            }
            inode->i_block[EXT4_DIND_BLOCK] = (uint32_t)new_block;
            if (ext4_zero_block(fs, (uint64_t)new_block) != 0) {
                inode->i_block[EXT4_DIND_BLOCK] = 0;
                ext4_free_block(fs, (uint64_t)new_block);
                return -1;
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
            if ((uint64_t)new_block > UINT32_MAX) {
                ext4_free_block(fs, (uint64_t)new_block);
                ext4_release_block(fs, inode->i_block[EXT4_DIND_BLOCK]);
                return -1;
            }
            dptrs[ind_idx] = (uint32_t)new_block;
            ext4_mark_block_dirty(fs, inode->i_block[EXT4_DIND_BLOCK]);
            if (ext4_zero_block(fs, (uint64_t)new_block) != 0) {
                dptrs[ind_idx] = 0;
                ext4_free_block(fs, (uint64_t)new_block);
                ext4_release_block(fs,
                                   inode->i_block[EXT4_DIND_BLOCK]);
                return -1;
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

uint32_t ext4_file_write_workspace(ext4_fs_t *fs, uint32_t ino,
                                   uint32_t offset, uint32_t size,
                                   const uint8_t *buffer, uint8_t *scratch,
                                   uint32_t scratch_capacity) {
    ext4_inode_cache_t *ic;
    uint64_t file_size;
    uint64_t new_size;
    int64_t new_block;
    uint32_t bytes_written;
    uint64_t block_num;
    uint32_t block_off;
    uint32_t to_write;
    uint64_t phys_block;
    uint8_t *block;
    int allocated_now;
    uint8_t *inline_data;
    uint32_t max_run_blocks;
    uint32_t run_blocks;
    uint32_t run_bytes;
    uint64_t next_phys_block;
    int64_t next_new_block;
    uint32_t allocated_blocks;
    uint32_t requested_blocks;
    uint32_t set_index;
    uint64_t allocated_first;

    if (!buffer || size == 0) {
        return 0;
    }

    ic = ext4_get_inode(fs, ino);
    if (!ic) {
        return 0;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFREG &&
        (ic->inode.i_mode & 0xF000) != EXT4_S_IFLNK) {
        ext4_release_inode(ic);
        return 0;
    }

    file_size = ext4_inode_get_size(&ic->inode);
    new_size = offset + size;
    if (new_size < offset) {
        ext4_release_inode(ic);
        return 0;
    }

    if ((ic->inode.i_mode & 0xF000) == EXT4_S_IFLNK &&
        new_size <= sizeof(ic->inode.i_block) &&
        file_size <= sizeof(ic->inode.i_block) &&
        ic->inode.i_blocks_lo == 0) {
        inline_data = (uint8_t *)ic->inode.i_block;
        memset(inline_data, 0, sizeof(ic->inode.i_block));
        memcpy(inline_data + offset, buffer, size);
        if (new_size > file_size) {
            ext4_inode_set_size(&ic->inode, new_size);
        }
        ext4_mark_inode_dirty(ic);
        ext4_release_inode(ic);
        return size;
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
        allocated_now = 0;

        if (phys_block == 0 && block_off == 0 &&
            to_write == fs->block_size) {
            max_run_blocks = 256 / fs->sectors_per_block;
            if (max_run_blocks == 0) max_run_blocks = 1;
            requested_blocks = (size - bytes_written) / fs->block_size;
            if (requested_blocks > max_run_blocks)
                requested_blocks = max_run_blocks;
            allocated_blocks = ext4_alloc_block_run(fs, 0,
                requested_blocks, &allocated_first);
            if (allocated_blocks == 0) break;

            run_blocks = 0;
            for (set_index = 0; set_index < allocated_blocks; set_index++) {
                if (ext4_inode_set_block(fs, &ic->inode,
                        block_num + set_index,
                        allocated_first + set_index) != 0) {
                    while (set_index < allocated_blocks) {
                        ext4_free_block(fs, allocated_first + set_index);
                        set_index++;
                    }
                    break;
                }
                ic->inode.i_blocks_lo += fs->sectors_per_block;
                run_blocks++;
            }
            if (run_blocks == 0) break;
            run_bytes = run_blocks * fs->block_size;
            if (ext4_write_blocks(fs, allocated_first, run_blocks,
                    buffer + bytes_written) != 0)
                break;
            bytes_written += run_bytes;
            continue;
        }

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
            allocated_now = 1;
        }

        if (block_off == 0 && to_write == fs->block_size) {
            max_run_blocks = 256 / fs->sectors_per_block;
            if (max_run_blocks == 0) max_run_blocks = 1;
            run_blocks = 1;
            while (run_blocks < max_run_blocks &&
                   bytes_written + (run_blocks + 1) * fs->block_size <= size) {
                next_phys_block = ext4_inode_get_block(fs, &ic->inode, block_num + run_blocks);
                if (next_phys_block == 0) {
                    next_new_block = ext4_alloc_block(fs,
                        phys_block + run_blocks - 1);
                    if (next_new_block < 0) {
                        break;
                    }
                    if (ext4_inode_set_block(fs, &ic->inode, block_num + run_blocks, next_new_block) != 0) {
                        ext4_free_block(fs, next_new_block);
                        break;
                    }
                    ic->inode.i_blocks_lo += fs->sectors_per_block;
                    next_phys_block = (uint64_t)next_new_block;
                }
                if (next_phys_block != phys_block + run_blocks) {
                    break;
                }
                run_blocks++;
            }
            run_bytes = run_blocks * fs->block_size;
            if (ext4_write_blocks(fs, phys_block, run_blocks, buffer + bytes_written) != 0) {
                break;
            }
            bytes_written += run_bytes;
            continue;
        }

        if (allocated_now && block_off == 0 && fs->block_size != 4096 &&
            scratch &&
            scratch_capacity >= fs->block_size) {
            memset(scratch, 0, fs->block_size);
            memcpy(scratch, buffer + bytes_written, to_write);
            if (ext4_write_block(fs, phys_block, scratch) != 0)
                break;
            bytes_written += to_write;
            continue;
        }

        if (allocated_now) {
            block = ext4_get_block_overwrite(fs, phys_block);
            if (block) {
                memset(block, 0, fs->block_size);
            }
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

uint32_t ext4_file_write(ext4_fs_t *fs, uint32_t ino, uint32_t offset,
                         uint32_t size, const uint8_t *buffer) {
    return ext4_file_write_workspace(fs, ino, offset, size, buffer, NULL, 0);
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

        ext4_inode_set_size(&ic->inode, new_size);
        ext4_mark_inode_dirty(ic);
        if (ext4_sync(fs) != 0) {
            ext4_release_inode(ic);
            return -1;
        }
        if (ic->inode.i_blocks_lo == 0) {
            ext4_release_inode(ic);
            return 0;
        }

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
        ext4_mark_inode_dirty(ic);
        ext4_release_inode(ic);
        return ext4_sync(fs);
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
    uint32_t old_orphan;
    int inline_symlink;
    int orphaned;

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

    if (ic->inode.i_links_count == 0) {
        ext4_release_inode(ic);
        return -1;
    }

    orphaned = ic->inode.i_links_count == 1;
    old_orphan = 0;
    if (orphaned) {
        old_orphan = fs->sb.s_last_orphan;
        ic->inode.i_dtime = old_orphan;
        ext4_mark_inode_dirty(ic);
        fs->sb.s_last_orphan = ino;
        fs->super_dirty = true;
        if (ext4_sync(fs) != 0) {
            ext4_release_inode(ic);
            return -1;
        }
    }

    if (ext4_dir_remove_entry(fs, parent_ino, name) != 0) {
        if (orphaned) {
            ic->inode.i_dtime = 0;
            ext4_mark_inode_dirty(ic);
            fs->sb.s_last_orphan = old_orphan;
            fs->super_dirty = true;
            ext4_sync(fs);
        }
        ext4_release_inode(ic);
        return -1;
    }

    ic->inode.i_links_count--;
    ext4_mark_inode_dirty(ic);
    if (ext4_sync(fs) != 0) {
        ext4_release_inode(ic);
        return -1;
    }

    if (ic->inode.i_links_count == 0) {
        file_size = ext4_inode_get_size(&ic->inode);
        inline_symlink = ((ic->inode.i_mode & 0xF000) == EXT4_S_IFLNK &&
                          file_size <= sizeof(ic->inode.i_block) &&
                          ic->inode.i_blocks_lo == 0);
        if ((ic->inode.i_mode & 0xF000) == EXT4_S_IFREG) {
            if (ext4_file_truncate(fs, ino, 0) != 0) {
                ext4_release_inode(ic);
                return -1;
            }
        } else if (!inline_symlink) {
            blocks = (file_size + fs->block_size - 1) / fs->block_size;

            for (i = 0; i < blocks; i++) {
                phys_block = ext4_inode_get_block(fs, &ic->inode, i);
                if (phys_block != 0) {
                    ext4_free_block(fs, phys_block);
                }
            }
        }

        ext4_release_inode(ic);
        if (ext4_free_inode(fs, ino) != 0) return -1;
        fs->sb.s_last_orphan = old_orphan;
        fs->super_dirty = true;
        return ext4_sync(fs);
    } else {
        ext4_release_inode(ic);
    }

    return 0;
}

int ext4_link_file(ext4_fs_t *fs, uint32_t ino, uint32_t parent_ino,
                   const char *name) {
    uint32_t existing;
    ext4_inode_cache_t *ic;
    uint8_t file_type;

    if (!fs || !name) return -1;
    if (ext4_dir_lookup(fs, parent_ino, name, &existing) == 0) return -1;
    ic = ext4_get_inode(fs, ino);
    if (!ic) return -1;
    if ((ic->inode.i_mode & 0xF000) == EXT4_S_IFDIR ||
        ic->inode.i_links_count == UINT16_MAX) {
        ext4_release_inode(ic);
        return -1;
    }
    file_type = ext4_mode_to_type(ic->inode.i_mode);
    ic->inode.i_links_count++;
    ext4_mark_inode_dirty(ic);
    if (ext4_dir_add_entry(fs, parent_ino, name, ino, file_type) != 0) {
        ic->inode.i_links_count--;
        ext4_mark_inode_dirty(ic);
        ext4_release_inode(ic);
        return -1;
    }
    ext4_release_inode(ic);
    return 0;
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

    if (old_parent_ino == new_parent_ino &&
        strcmp(old_name, new_name) == 0)
        return 0;

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
        if (ext4_unlink_file(fs, new_parent_ino, new_name) != 0)
            return -1;
    }

    if (ext4_dir_add_entry(fs, new_parent_ino, new_name, ino, file_type) != 0) {
        return -1;
    }

    if (ext4_dir_remove_entry(fs, old_parent_ino, old_name) != 0) {
        ext4_dir_remove_entry(fs, new_parent_ino, new_name);
        return -1;
    }
    return 0;
}

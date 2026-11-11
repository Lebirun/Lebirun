#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>
#include <stdint.h>

extern void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block);
extern void ext4_mark_inode_dirty(ext4_inode_cache_t *ic);

#define EXT4_RUN_SECTORS 65536u

static int ext4_no_data_blocks(ext4_fs_t *fs, ext4_inode_t *inode) {
    return !(inode->i_flags & EXT4_INODE_FLAG_EXTENTS) && !inode->i_blocks_high &&
           inode->i_blocks_lo == ((inode->i_file_acl_lo || inode->i_file_acl_high) ?
                                  fs->sectors_per_block : 0);
}

uint64_t ext4_file_read(ext4_fs_t *fs, uint32_t ino, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ext4_inode_cache_t *ic;
    uint64_t file_size;
    uint64_t bytes_read;
    uint64_t block_num;
    uint32_t block_off;
    uint32_t to_read;
    uint64_t phys_block;
    uint64_t next_phys_block;
    uint32_t max_run_blocks;
    uint32_t run_blocks;
    uint32_t run_bytes;
    uint8_t *block;
    uint8_t *inline_data;
    uint32_t i;
    int result;

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
        ext4_no_data_blocks(fs, &ic->inode)) {
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
            if (block_off == 0 && to_read == fs->block_size) {
                max_run_blocks = EXT4_RUN_SECTORS / fs->sectors_per_block;
                if (max_run_blocks == 0) max_run_blocks = 1;
                run_blocks = 1;
                while (run_blocks < max_run_blocks &&
                       bytes_read + (run_blocks + 1) * fs->block_size <= size) {
                    next_phys_block = ext4_inode_get_block(fs, &ic->inode,
                                                           block_num + run_blocks);
                    if (next_phys_block == 0 ||
                        next_phys_block != phys_block + run_blocks) {
                        break;
                    }
                    run_blocks++;
                }
                run_bytes = run_blocks * fs->block_size;
                result = ext4_read_blocks_unlocked(fs, phys_block, run_blocks,
                                                   buffer + bytes_read);
                if (result >= 0) {
                    if (ic->ino != ino) break;
                    for (i = 0; result > 0 && i < run_blocks; i++)
                        if (ext4_inode_get_block(fs, &ic->inode, block_num + i) !=
                            phys_block + i) break;
                    if (result == 0 || i == run_blocks) bytes_read += run_bytes;
                    continue;
                }
            }
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

int ext4_inode_set_block(ext4_fs_t *fs, ext4_inode_cache_t *ic,
                         uint64_t logical_block, uint64_t phys_block) {
    uint64_t per;
    uint64_t span;
    uint64_t owner;
    uint64_t child;
    uint32_t *slot;
    int64_t new_block;
    int depth;

    if (ic->inode.i_flags & EXT4_INODE_FLAG_EXTENTS)
        return ext4_ext_map(fs, ic, logical_block, phys_block);
    if (phys_block > UINT32_MAX) return -1;
    if (logical_block < EXT4_NDIR_BLOCKS) {
        ic->inode.i_block[logical_block] = (uint32_t)phys_block;
        return 0;
    }
    logical_block -= EXT4_NDIR_BLOCKS;
    per = fs->block_size / 4;
    span = 1;
    for (depth = 1; depth <= 3; depth++) {
        span *= per;
        if (logical_block < span) break;
        logical_block -= span;
    }
    if (depth > 3) return -1;
    owner = 0;
    slot = (uint32_t *)(void *)ic->inode.i_block + EXT4_IND_BLOCK + depth - 1;
    for (; depth > 0; depth--) {
        if (*slot == 0) {
            new_block = ext4_alloc_block(fs, phys_block);
            if (new_block < 0 || (uint64_t)new_block > UINT32_MAX ||
                ext4_zero_block(fs, (uint64_t)new_block) != 0) {
                if (new_block >= 0) ext4_free_block(fs, (uint64_t)new_block);
                if (owner) ext4_release_block(fs, owner);
                return -1;
            }
            *slot = (uint32_t)new_block;
            ext4_inode_add_blocks(fs, &ic->inode, 1);
            if (owner) ext4_mark_block_dirty(fs, owner);
        }
        child = *slot;
        if (owner) ext4_release_block(fs, owner);
        slot = (uint32_t *)ext4_get_block(fs, child);
        if (!slot) return -1;
        owner = child;
        span /= per;
        slot += logical_block / span;
        logical_block %= span;
    }
    *slot = (uint32_t)phys_block;
    ext4_mark_block_dirty(fs, owner);
    ext4_release_block(fs, owner);
    return 0;
}

uint64_t ext4_file_write_workspace(ext4_fs_t *fs, uint32_t ino,
                                   uint64_t offset, uint64_t size,
                                   const uint8_t *buffer, uint8_t *scratch,
                                   uint64_t scratch_capacity) {
    ext4_inode_cache_t *ic;
    uint64_t file_size;
    uint64_t new_size;
    int64_t new_block;
    uint64_t bytes_written;
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
        ext4_no_data_blocks(fs, &ic->inode)) {
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
            max_run_blocks = EXT4_RUN_SECTORS / fs->sectors_per_block;
            if (max_run_blocks == 0) max_run_blocks = 1;
            requested_blocks = (size - bytes_written) / fs->block_size;
            if (requested_blocks > max_run_blocks)
                requested_blocks = max_run_blocks;
            allocated_blocks = ext4_alloc_block_run(fs, 0,
                requested_blocks, &allocated_first);
            if (allocated_blocks == 0) break;

            run_blocks = 0;
            for (set_index = 0; set_index < allocated_blocks; set_index++) {
                if (ext4_inode_set_block(fs, ic,
                        block_num + set_index,
                        allocated_first + set_index) != 0) {
                    while (set_index < allocated_blocks) {
                        ext4_free_block(fs, allocated_first + set_index);
                        set_index++;
                    }
                    break;
                }
                ext4_inode_add_blocks(fs, &ic->inode, 1);
                run_blocks++;
            }
            if (run_blocks == 0) break;
            run_bytes = run_blocks * fs->block_size;
            if (ext4_write_blocks_unlocked(fs, allocated_first, run_blocks,
                    buffer + bytes_written) != 0 || ic->ino != ino)
                break;
            bytes_written += run_bytes;
            continue;
        }

        if (phys_block == 0) {
            new_block = ext4_alloc_block(fs, 0);
            if (new_block < 0) {
                break;
            }

            if (ext4_inode_set_block(fs, ic, block_num, new_block) != 0) {
                ext4_free_block(fs, new_block);
                break;
            }

            ext4_inode_add_blocks(fs, &ic->inode, 1);
            phys_block = new_block;
            allocated_now = 1;
        }

        if (block_off == 0 && to_write == fs->block_size) {
            max_run_blocks = EXT4_RUN_SECTORS / fs->sectors_per_block;
            if (max_run_blocks == 0) max_run_blocks = 1;
            run_blocks = 1;
            while (run_blocks < max_run_blocks &&
                   bytes_written + (run_blocks + 1) * fs->block_size <= size) {
                next_phys_block = ext4_inode_get_block(fs, &ic->inode, block_num + run_blocks);
                if (next_phys_block != phys_block + run_blocks) {
                    break;
                }
                run_blocks++;
            }
            run_bytes = run_blocks * fs->block_size;
            if (ext4_write_blocks_unlocked(fs, phys_block, run_blocks,
                                           buffer + bytes_written) != 0 ||
                ic->ino != ino) {
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

uint64_t ext4_file_write(ext4_fs_t *fs, uint32_t ino, uint64_t offset,
                         uint64_t size, const uint8_t *buffer) {
    return ext4_file_write_workspace(fs, ino, offset, size, buffer, NULL, 0);
}

static int ext4_truncate_blocks(ext4_fs_t *fs, ext4_inode_cache_t *ic,
                                 uint32_t *entries, uint32_t count,
                                 unsigned int depth, uint64_t first,
                                 uint64_t new_size, uint64_t owner,
                                 uint64_t *retained) {
    uint32_t *removed;
    uint32_t *children;
    uint8_t *block;
    uint32_t pointers;
    uint32_t removed_count;
    uint32_t i;
    uint32_t j;
    uint32_t physical;
    uint32_t entry_index;
    uint32_t tail;
    uint64_t span;
    uint64_t logical;
    uint64_t keep;
    int empty;
    int result;

    pointers = fs->block_size / sizeof(uint32_t);
    removed = kmalloc((size_t)count * sizeof(uint32_t));
    if (!removed) return -1;
    removed_count = 0;
    result = -1;
    span = 1;
    for (i = 0; i < depth; i++) span *= pointers;
    keep = new_size / fs->block_size;
    tail = new_size % fs->block_size;
    if (tail) keep++;

    for (i = 0; i < count; i++) {
        entry_index = !owner && depth ? EXT4_NDIR_BLOCKS + depth - 1 : i;
        physical = owner ? entries[entry_index] : ic->inode.i_block[entry_index];
        if (!physical) continue;
        logical = first + i * span;
        empty = logical >= keep;
        if (depth) {
            children = (uint32_t *)ext4_get_block(fs, physical);
            if (!children) goto out;
            if (ext4_truncate_blocks(fs, ic, children, pointers,
                    depth - 1, logical, new_size, physical, retained) != 0) {
                ext4_release_block(fs, physical);
                goto out;
            }
            empty = 1;
            for (j = 0; j < pointers; j++) {
                if (children[j]) {
                    empty = 0;
                    break;
                }
            }
            ext4_release_block(fs, physical);
        } else if (tail && logical == keep - 1) {
            block = ext4_get_block(fs, physical);
            if (!block) goto out;
            memset(block + tail, 0, fs->block_size - tail);
            ext4_mark_block_dirty(fs, physical);
            ext4_release_block(fs, physical);
        }
        if (empty) {
            if (owner) {
                entries[entry_index] = 0;
                ext4_mark_block_dirty(fs, owner);
            } else {
                ic->inode.i_block[entry_index] = 0;
                ext4_mark_inode_dirty(ic);
            }
            removed[removed_count++] = physical;
        } else {
            (*retained)++;
        }
    }

    if (removed_count) {
        if (ext4_sync(fs) != 0) goto out;
        for (i = 0; i < removed_count; i++) {
            if (ext4_free_block(fs, removed[i]) != 0) goto out;
        }
    }
    result = 0;
out:
    kfree(removed);
    return result;
}

static int ext4_free_detached_blocks(ext4_fs_t *fs, uint32_t physical,
                                      unsigned int depth) {
    uint32_t *children;
    uint32_t count;
    uint32_t i;

    if (!physical) return 0;
    if (depth) {
        children = (uint32_t *)ext4_get_block(fs, physical);
        if (!children) return -1;
        count = fs->block_size / sizeof(uint32_t);
        for (i = 0; i < count; i++) {
            if (ext4_free_detached_blocks(fs, children[i], depth - 1) != 0) {
                ext4_release_block(fs, physical);
                return -1;
            }
        }
        ext4_release_block(fs, physical);
    }
    return ext4_free_block(fs, physical);
}

int ext4_file_truncate(ext4_fs_t *fs, uint32_t ino, uint64_t new_size) {
    ext4_inode_cache_t *ic;
    int result;

    ic = ext4_get_inode(fs, ino);
    if (!ic) return -1;
    result = -1;
    if ((ic->inode.i_mode & 0xF000) == EXT4_S_IFREG)
        result = ext4_truncate_inode(fs, ic, new_size);
    ext4_release_inode(ic);
    return result;
}

int ext4_truncate_inode(ext4_fs_t *fs, ext4_inode_cache_t *ic, uint64_t new_size) {
    uint32_t detached[EXT4_N_BLOCKS];
    uint64_t old_size;
    uint64_t retained;
    uint64_t first;
    uint64_t span;
    uint64_t sectors;
    unsigned int depth;
    unsigned int i;
    int result;

    result = -1;
    old_size = ext4_inode_get_size(&ic->inode);
    if (new_size == old_size && new_size != 0) return 0;
    if (ic->inode.i_flags & EXT4_INODE_FLAG_EXTENTS) {
        result = ext4_ext_truncate(fs, ic, new_size);
        if (result == 0) result = ext4_sync(fs);
        return result;
    }
    if (new_size == 0 && old_size == 0) {
        for (i = 0; i < EXT4_N_BLOCKS; i++) {
            if (ic->inode.i_block[i]) break;
        }
        if (i == EXT4_N_BLOCKS) {
            result = 0;
            goto out;
        }
    }
    if (new_size == 0) {
        memcpy(detached, ic->inode.i_block, sizeof(detached));
        memset(ic->inode.i_block, 0, sizeof(ic->inode.i_block));
        ext4_inode_set_size(&ic->inode, 0);
        ic->inode.i_blocks_lo =
            (ic->inode.i_file_acl_lo || ic->inode.i_file_acl_high) ?
            fs->sectors_per_block : 0;
        ic->inode.i_blocks_high = 0;
        ext4_mark_inode_dirty(ic);
        if (ext4_sync(fs) != 0) goto out;
        for (i = 0; i < EXT4_N_BLOCKS; i++) {
            depth = i < EXT4_NDIR_BLOCKS ? 0 : i - EXT4_NDIR_BLOCKS + 1;
            if (ext4_free_detached_blocks(fs, detached[i], depth) != 0)
                goto out;
        }
        result = ext4_sync(fs);
        goto out;
    }
    if (new_size <= old_size) {
        ext4_inode_set_size(&ic->inode, new_size);
        ext4_mark_inode_dirty(ic);
        if (ext4_sync(fs) != 0) goto out;
        retained = 0;
        if (ext4_truncate_blocks(fs, ic, NULL,
                EXT4_NDIR_BLOCKS, 0, 0, new_size, 0, &retained) != 0)
            goto out;
        first = EXT4_NDIR_BLOCKS;
        span = fs->block_size / sizeof(uint32_t);
        for (depth = 1; depth <= 3; depth++) {
            if (ext4_truncate_blocks(fs, ic, NULL,
                    1, depth, first, new_size, 0, &retained) != 0)
                goto out;
            first += span;
            span *= fs->block_size / sizeof(uint32_t);
        }
        if (ic->inode.i_file_acl_lo || ic->inode.i_file_acl_high)
            retained++;
        sectors = retained * fs->sectors_per_block;
        ic->inode.i_blocks_lo = (uint32_t)sectors;
        ic->inode.i_blocks_high = (uint16_t)(sectors >> 32);
    }
    ext4_inode_set_size(&ic->inode, new_size);
    ext4_mark_inode_dirty(ic);
    result = ext4_sync(fs);
out:
    return result;
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
    ext4_inode_cache_t *parent;
    uint64_t file_size;
    uint32_t old_orphan;
    int inline_symlink;
    int orphaned;
    int is_dir;

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

    is_dir = (ic->inode.i_mode & 0xF000) == EXT4_S_IFDIR;
    if (ic->inode.i_links_count == 0 || (is_dir && !ext4_dir_is_empty(fs, ino))) {
        ext4_release_inode(ic);
        return -1;
    }

    orphaned = !is_dir && ic->inode.i_links_count == 1;
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

    if (is_dir) {
        ic->inode.i_links_count = 0;
        parent = ext4_get_inode(fs, parent_ino);
        if (parent) {
            if (parent->inode.i_links_count > 2) parent->inode.i_links_count--;
            ext4_mark_inode_dirty(parent);
            ext4_release_inode(parent);
        }
    } else {
        ic->inode.i_links_count--;
    }
    ext4_mark_inode_dirty(ic);
    if (ext4_sync(fs) != 0) {
        ext4_release_inode(ic);
        return -1;
    }

    if (ic->inode.i_links_count == 0) {
        file_size = ext4_inode_get_size(&ic->inode);
        inline_symlink = ((ic->inode.i_mode & 0xF000) == EXT4_S_IFLNK &&
                          file_size <= sizeof(ic->inode.i_block) &&
                          ext4_no_data_blocks(fs, &ic->inode));
        if (!inline_symlink && ext4_truncate_inode(fs, ic, 0) != 0) {
            ext4_release_inode(ic);
            return -1;
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

static void ext4_reparent_dir(ext4_fs_t *fs, uint32_t ino,
                              uint32_t old_parent, uint32_t new_parent) {
    ext4_inode_cache_t *ic;
    ext4_dir_entry_t *dotdot;
    uint64_t phys;
    uint8_t *block;

    ic = ext4_get_inode(fs, ino);
    if (!ic) return;
    phys = ext4_inode_get_block(fs, &ic->inode, 0);
    block = phys ? ext4_get_block(fs, phys) : NULL;
    if (block) {
        dotdot = (ext4_dir_entry_t *)(block + ((ext4_dir_entry_t *)block)->rec_len);
        if ((uint8_t *)dotdot + 12 <= block + fs->block_size &&
            dotdot->name_len == 2 && dotdot->name[0] == '.' &&
            dotdot->name[1] == '.' && dotdot->inode == old_parent) {
            dotdot->inode = new_parent;
            ext4_dir_csum(fs, ino, &ic->inode, block);
            ext4_mark_block_dirty(fs, phys);
        }
        ext4_release_block(fs, phys);
    }
    ext4_release_inode(ic);
    ic = ext4_get_inode(fs, old_parent);
    if (ic) {
        if (ic->inode.i_links_count > 2) ic->inode.i_links_count--;
        ext4_mark_inode_dirty(ic);
        ext4_release_inode(ic);
    }
    ic = ext4_get_inode(fs, new_parent);
    if (ic) {
        if (ic->inode.i_links_count > 1 && ic->inode.i_links_count < 65000)
            ic->inode.i_links_count++;
        ext4_mark_inode_dirty(ic);
        ext4_release_inode(ic);
    }
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
    if (file_type == EXT4_FT_DIR && old_parent_ino != new_parent_ino)
        ext4_reparent_dir(fs, ino, old_parent_ino, new_parent_ino);
    return 0;
}

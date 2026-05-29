#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

extern void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block);
extern void ext4_mark_inode_dirty(ext4_inode_cache_t *ic);

int ext4_dir_lookup(ext4_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *result_ino) {
    if (!name || !result_ino) {
        return -1;
    }

    ext4_inode_cache_t *ic = ext4_get_inode(fs, dir_ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFDIR) {
        ext4_release_inode(ic);
        return -1;
    }

    uint64_t dir_size = ext4_inode_get_size(&ic->inode);
    uint64_t offset = 0;
    size_t name_len = strlen(name);

    while (offset < dir_size) {
        uint64_t block_num = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint64_t phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);
        if (phys_block == 0) {
            break;
        }

        uint8_t *block = ext4_get_block(fs, phys_block);
        if (!block) {
            break;
        }

        while (block_off < fs->block_size && offset < dir_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block + block_off);

            if (entry->rec_len == 0) {
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return -1;
            }

            if (entry->inode != 0 && entry->name_len == name_len) {
                if (memcmp(entry->name, name, name_len) == 0) {
                    *result_ino = entry->inode;
                    ext4_release_block(fs, phys_block);
                    ext4_release_inode(ic);
                    return 0;
                }
            }

            offset += entry->rec_len;
            block_off += entry->rec_len;
        }

        ext4_release_block(fs, phys_block);
    }

    ext4_release_inode(ic);
    return -1;
}

int ext4_dir_iterate(ext4_fs_t *fs, uint32_t dir_ino, int (*callback)(ext4_dir_entry_t *, void *), void *ctx) {
    ext4_inode_cache_t *ic = ext4_get_inode(fs, dir_ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFDIR) {
        ext4_release_inode(ic);
        return -1;
    }

    uint64_t dir_size = ext4_inode_get_size(&ic->inode);
    uint64_t offset = 0;

    while (offset < dir_size) {
        uint64_t block_num = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint64_t phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);
        if (phys_block == 0) {
            break;
        }

        uint8_t *block = ext4_get_block(fs, phys_block);
        if (!block) {
            break;
        }

        while (block_off < fs->block_size && offset < dir_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block + block_off);

            if (entry->rec_len == 0) {
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return -1;
            }

            if (entry->inode != 0) {
                int result = callback(entry, ctx);
                if (result != 0) {
                    ext4_release_block(fs, phys_block);
                    ext4_release_inode(ic);
                    return result;
                }
            }

            offset += entry->rec_len;
            block_off += entry->rec_len;
        }

        ext4_release_block(fs, phys_block);
    }

    ext4_release_inode(ic);
    return 0;
}

typedef struct {
    uint32_t target_index;
    uint32_t current_index;
    ext4_dir_entry_t *out_entry;
    int found;
} dir_get_entry_ctx_t;

static int dir_get_entry_callback(ext4_dir_entry_t *entry, void *ctx) {
    dir_get_entry_ctx_t *gctx = (dir_get_entry_ctx_t *)ctx;
    uint8_t name_len;

    if (gctx->current_index == gctx->target_index) {
        name_len = entry->name_len;
        memcpy(gctx->out_entry, entry, sizeof(ext4_dir_entry_t) - EXT4_NAME_LEN + name_len);
        if (name_len < EXT4_NAME_LEN) {
            gctx->out_entry->name[name_len] = '\0';
        }
        gctx->found = 1;
        return 1;
    }

    gctx->current_index++;
    return 0;
}

int ext4_dir_get_entry(ext4_fs_t *fs, uint32_t dir_ino, uint32_t index, ext4_dir_entry_t *entry) {
    dir_get_entry_ctx_t ctx;
    ctx.target_index = index;
    ctx.current_index = 0;
    ctx.out_entry = entry;
    ctx.found = 0;

    ext4_dir_iterate(fs, dir_ino, dir_get_entry_callback, &ctx);

    return ctx.found ? 0 : -1;
}

int ext4_dir_add_entry(ext4_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t file_type) {
    if (!name) {
        return -1;
    }

    size_t name_len = strlen(name);
    if (name_len > EXT4_NAME_LEN) {
        return -1;
    }

    uint32_t needed_len = ((sizeof(ext4_dir_entry_t) - EXT4_NAME_LEN + name_len + 3) / 4) * 4;

    ext4_inode_cache_t *ic = ext4_get_inode(fs, dir_ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFDIR) {
        ext4_release_inode(ic);
        return -1;
    }

    uint64_t dir_size = ext4_inode_get_size(&ic->inode);
    uint64_t offset = 0;

    while (offset < dir_size) {
        uint64_t block_num = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint64_t phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);
        if (phys_block == 0) {
            break;
        }

        uint8_t *block = ext4_get_block(fs, phys_block);
        if (!block) {
            break;
        }

        while (block_off < fs->block_size && offset < dir_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block + block_off);

            if (entry->rec_len == 0) {
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return -1;
            }

            uint32_t actual_len = ((sizeof(ext4_dir_entry_t) - EXT4_NAME_LEN + entry->name_len + 3) / 4) * 4;
            uint32_t free_space = entry->rec_len - actual_len;

            if (entry->inode == 0 && entry->rec_len >= needed_len) {
                entry->inode = ino;
                entry->name_len = name_len;
                entry->file_type = file_type;
                memcpy(entry->name, name, name_len);

                ext4_mark_block_dirty(fs, phys_block);
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return 0;
            }

            if (entry->inode != 0 && free_space >= needed_len) {
                uint16_t old_rec_len = entry->rec_len;
                entry->rec_len = actual_len;

                ext4_dir_entry_t *new_entry = (ext4_dir_entry_t *)(block + block_off + actual_len);
                new_entry->inode = ino;
                new_entry->rec_len = old_rec_len - actual_len;
                new_entry->name_len = name_len;
                new_entry->file_type = file_type;
                memcpy(new_entry->name, name, name_len);

                ext4_mark_block_dirty(fs, phys_block);
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return 0;
            }

            offset += entry->rec_len;
            block_off += entry->rec_len;
        }

        ext4_release_block(fs, phys_block);
    }

    int new_block = ext4_alloc_block(fs, 0);
    if (new_block < 0) {
        ext4_release_inode(ic);
        return -1;
    }

    uint8_t *new_block_data = (uint8_t *)kmalloc(fs->block_size);
    if (!new_block_data) {
        ext4_free_block(fs, new_block);
        ext4_release_inode(ic);
        return -1;
    }

    memset(new_block_data, 0, fs->block_size);

    ext4_dir_entry_t *new_entry = (ext4_dir_entry_t *)new_block_data;
    new_entry->inode = ino;
    new_entry->rec_len = fs->block_size;
    new_entry->name_len = name_len;
    new_entry->file_type = file_type;
    memcpy(new_entry->name, name, name_len);

    if (ext4_write_block(fs, new_block, new_block_data) != 0) {
        kfree(new_block_data);
        ext4_free_block(fs, new_block);
        ext4_release_inode(ic);
        return -1;
    }

    kfree(new_block_data);

    uint64_t block_idx = dir_size / fs->block_size;
    
    if (!(ic->inode.i_flags & EXT4_INODE_FLAG_EXTENTS)) {
        if (block_idx < EXT4_NDIR_BLOCKS) {
            ic->inode.i_block[block_idx] = new_block;
        } else {
            ext4_free_block(fs, new_block);
            ext4_release_inode(ic);
            return -1;
        }
    } else {
        ext4_free_block(fs, new_block);
        ext4_release_inode(ic);
        return -1;
    }

    ext4_inode_set_size(&ic->inode, dir_size + fs->block_size);
    ic->inode.i_blocks_lo += fs->sectors_per_block;
    ext4_mark_inode_dirty(ic);
    ext4_release_inode(ic);

    return 0;
}

int ext4_dir_remove_entry(ext4_fs_t *fs, uint32_t dir_ino, const char *name) {
    if (!name) {
        return -1;
    }

    ext4_inode_cache_t *ic = ext4_get_inode(fs, dir_ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFDIR) {
        ext4_release_inode(ic);
        return -1;
    }

    uint64_t dir_size = ext4_inode_get_size(&ic->inode);
    uint64_t offset = 0;
    size_t name_len = strlen(name);

    ext4_dir_entry_t *prev_entry = NULL;
    uint64_t prev_phys_block = 0;

    while (offset < dir_size) {
        uint64_t block_num = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint64_t phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);
        if (phys_block == 0) {
            break;
        }

        uint8_t *block = ext4_get_block(fs, phys_block);
        if (!block) {
            break;
        }

        prev_entry = NULL;

        while (block_off < fs->block_size && offset < dir_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block + block_off);

            if (entry->rec_len == 0) {
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return -1;
            }

            if (entry->inode != 0 && entry->name_len == name_len) {
                if (memcmp(entry->name, name, name_len) == 0) {
                    if (prev_entry && prev_phys_block == phys_block) {
                        prev_entry->rec_len += entry->rec_len;
                    } else {
                        entry->inode = 0;
                    }

                    ext4_mark_block_dirty(fs, phys_block);
                    ext4_release_block(fs, phys_block);
                    ext4_release_inode(ic);
                    return 0;
                }
            }

            prev_entry = entry;
            prev_phys_block = phys_block;
            offset += entry->rec_len;
            block_off += entry->rec_len;
        }

        ext4_release_block(fs, phys_block);
    }

    ext4_release_inode(ic);
    return -1;
}

int ext4_dir_is_empty(ext4_fs_t *fs, uint32_t dir_ino) {
    ext4_inode_cache_t *ic = ext4_get_inode(fs, dir_ino);
    if (!ic) {
        return 0;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFDIR) {
        ext4_release_inode(ic);
        return 0;
    }

    uint64_t dir_size = ext4_inode_get_size(&ic->inode);
    uint64_t offset = 0;
    int entry_count = 0;

    while (offset < dir_size) {
        uint64_t block_num = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint64_t phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);
        if (phys_block == 0) {
            break;
        }

        uint8_t *block = ext4_get_block(fs, phys_block);
        if (!block) {
            break;
        }

        while (block_off < fs->block_size && offset < dir_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block + block_off);

            if (entry->rec_len == 0) {
                ext4_release_block(fs, phys_block);
                ext4_release_inode(ic);
                return 0;
            }

            if (entry->inode != 0) {
                if (!(entry->name_len == 1 && entry->name[0] == '.') &&
                    !(entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.')) {
                    entry_count++;
                }
            }

            offset += entry->rec_len;
            block_off += entry->rec_len;
        }

        ext4_release_block(fs, phys_block);
    }

    ext4_release_inode(ic);
    return (entry_count == 0) ? 1 : 0;
}

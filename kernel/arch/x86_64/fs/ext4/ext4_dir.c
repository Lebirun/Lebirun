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

static int ext4_dir_add_to_block(ext4_fs_t *fs, ext4_inode_cache_t *ic,
                                 uint64_t phys_block,
                                 const char *name, size_t name_len,
                                 uint32_t ino, uint8_t file_type,
                                 uint32_t needed_len) {
    uint8_t *block;
    uint32_t block_off;
    uint32_t limit;
    ext4_dir_entry_t *entry;
    ext4_dir_entry_t *new_entry;
    uint32_t actual_len;
    uint32_t free_space;
    uint16_t old_rec_len;

    block = ext4_get_block(fs, phys_block);
    if (!block) return -1;
    limit = fs->block_size - ext4_dir_tail_size(fs, block);
    block_off = 0;
    while (block_off < limit) {
        entry = (ext4_dir_entry_t *)(block + block_off);
        if (entry->rec_len == 0 ||
            entry->rec_len > limit - block_off) {
            ext4_release_block(fs, phys_block);
            return -1;
        }
        actual_len = ((sizeof(ext4_dir_entry_t) - EXT4_NAME_LEN +
                       entry->name_len + 3) / 4) * 4;
        if (actual_len > entry->rec_len) {
            ext4_release_block(fs, phys_block);
            return -1;
        }
        free_space = entry->rec_len - actual_len;
        new_entry = NULL;
        if (entry->inode == 0 && entry->rec_len >= needed_len) {
            old_rec_len = entry->rec_len;
            if (old_rec_len >= needed_len + 12) {
                entry->rec_len = old_rec_len - needed_len;
                new_entry = (ext4_dir_entry_t *)(block + block_off +
                                                  entry->rec_len);
                new_entry->rec_len = needed_len;
            } else {
                new_entry = entry;
            }
        } else if (entry->inode != 0 && free_space >= needed_len) {
            old_rec_len = entry->rec_len;
            entry->rec_len = actual_len;
            new_entry = (ext4_dir_entry_t *)(block + block_off + actual_len);
            new_entry->rec_len = old_rec_len - actual_len;
        }
        if (new_entry) {
            new_entry->inode = ino;
            new_entry->name_len = name_len;
            new_entry->file_type = file_type;
            memcpy(new_entry->name, name, name_len);
            ext4_dir_csum(fs, ic->ino, &ic->inode, block);
            ext4_mark_block_dirty(fs, phys_block);
            ext4_release_block(fs, phys_block);
            return 1;
        }
        block_off += entry->rec_len;
    }
    ext4_release_block(fs, phys_block);
    return 0;
}

static int ext4_dir_deindex(ext4_fs_t *fs, ext4_inode_cache_t *ic) {
    uint64_t blocks;
    uint64_t i;
    uint64_t phys;
    uint8_t *block;
    ext4_dir_entry_t *entry;
    uint32_t limit;

    if (!(ic->inode.i_flags & EXT4_INODE_FLAG_INDEX)) return 0;
    limit = fs->block_size - (fs->metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
    blocks = ext4_inode_get_size(&ic->inode) / fs->block_size;
    for (i = 0; i < blocks; i++) {
        phys = ext4_inode_get_block(fs, &ic->inode, i);
        if (!phys) continue;
        block = ext4_get_block(fs, phys);
        if (!block) return -1;
        entry = (ext4_dir_entry_t *)block;
        if (i == 0 && entry->rec_len == 12) {
            entry = (ext4_dir_entry_t *)(block + 12);
            if (entry->rec_len == fs->block_size - 12) {
                entry->rec_len = limit - 12;
                ext4_dir_set_tail(fs, block);
                ext4_dir_csum(fs, ic->ino, &ic->inode, block);
                ext4_mark_block_dirty(fs, phys);
            }
        } else if (entry->inode == 0 && entry->rec_len == fs->block_size) {
            memset(block, 0, fs->block_size);
            entry->rec_len = limit;
            ext4_dir_set_tail(fs, block);
            ext4_dir_csum(fs, ic->ino, &ic->inode, block);
            ext4_mark_block_dirty(fs, phys);
        }
        ext4_release_block(fs, phys);
    }
    ic->inode.i_flags &= ~EXT4_INODE_FLAG_INDEX;
    ext4_mark_inode_dirty(ic);
    return 0;
}

int ext4_dir_add_entry(ext4_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t file_type) {
    size_t name_len;
    uint32_t needed_len;
    ext4_inode_cache_t *ic;
    uint64_t dir_size;
    uint64_t block_num;
    uint64_t phys_block;
    ext4_dir_entry_t *new_entry;
    int64_t new_block;
    uint8_t *new_block_data;
    uint64_t block_count;
    uint64_t i;
    int result;

    if (!name) {
        return -1;
    }

    name_len = strlen(name);
    if (name_len == 0 || name_len > EXT4_NAME_LEN) {
        return -1;
    }

    needed_len = ((sizeof(ext4_dir_entry_t) - EXT4_NAME_LEN +
                   name_len + 3) / 4) * 4;

    ic = ext4_get_inode(fs, dir_ino);
    if (!ic) {
        return -1;
    }

    if ((ic->inode.i_mode & 0xF000) != EXT4_S_IFDIR ||
        ext4_dir_deindex(fs, ic) != 0) {
        ext4_release_inode(ic);
        return -1;
    }

    dir_size = ext4_inode_get_size(&ic->inode);
    block_count = (dir_size + fs->block_size - 1) / fs->block_size;
    for (i = 0; i < block_count; i++) {
        block_num = (i + block_count - 1) % block_count;
        phys_block = ext4_inode_get_block(fs, &ic->inode, block_num);
        if (phys_block == 0) continue;
        result = ext4_dir_add_to_block(fs, ic, phys_block, name, name_len,
                                       ino, file_type, needed_len);
        if (result != 0) {
            ext4_release_inode(ic);
            return result > 0 ? 0 : -1;
        }
    }

    new_block = ext4_alloc_block(fs, 0);
    if (new_block < 0) {
        ext4_release_inode(ic);
        return -1;
    }

    new_block_data = (uint8_t *)kmalloc(fs->block_size);
    if (!new_block_data) {
        ext4_free_block(fs, new_block);
        ext4_release_inode(ic);
        return -1;
    }

    memset(new_block_data, 0, fs->block_size);
    ext4_dir_set_tail(fs, new_block_data);
    new_entry = (ext4_dir_entry_t *)new_block_data;
    new_entry->inode = ino;
    new_entry->rec_len = fs->block_size -
                         (fs->metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
    new_entry->name_len = name_len;
    new_entry->file_type = file_type;
    memcpy(new_entry->name, name, name_len);
    ext4_dir_csum(fs, ic->ino, &ic->inode, new_block_data);

    block_num = dir_size / fs->block_size;
    if (ext4_write_block(fs, new_block, new_block_data) != 0 ||
        ext4_inode_set_block(fs, ic, block_num, (uint64_t)new_block) != 0) {
        kfree(new_block_data);
        ext4_free_block(fs, new_block);
        ext4_release_inode(ic);
        return -1;
    }
    kfree(new_block_data);

    ext4_inode_set_size(&ic->inode, (block_num + 1) * fs->block_size);
    ext4_inode_add_blocks(fs, &ic->inode, 1);
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

                    ext4_dir_csum(fs, dir_ino, &ic->inode, block);
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

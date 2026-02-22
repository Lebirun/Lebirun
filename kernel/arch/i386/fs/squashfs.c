#include <kernel/squashfs.h>
#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/vfs.h>
#include <kernel/debug.h>
#include <string.h>
#include <stdio.h>

static squashfs_context_t squashfs_ctx;
static int squashfs_initialized = 0;
static vfs_node_t *squashfs_vfs_root = NULL;
static vfs_fs_type_t squashfs_fs_type;
static dirent_t squashfs_dirent;

#define SQFS_NODE_CACHE_SIZE 64
static struct {
    uint64_t inode_ref;
    vfs_node_t *node;
} sqfs_node_cache[SQFS_NODE_CACHE_SIZE];
static uint32_t sqfs_node_cache_count = 0;

static vfs_node_t *sqfs_cache_lookup(uint64_t inode_ref) {
    uint32_t i;
    for (i = 0; i < sqfs_node_cache_count; i++) {
        if (sqfs_node_cache[i].inode_ref == inode_ref)
            return sqfs_node_cache[i].node;
    }
    return NULL;
}

static void sqfs_cache_insert(uint64_t inode_ref, vfs_node_t *node) {
    if (sqfs_node_cache_count < SQFS_NODE_CACHE_SIZE) {
        sqfs_node_cache[sqfs_node_cache_count].inode_ref = inode_ref;
        sqfs_node_cache[sqfs_node_cache_count].node = node;
        sqfs_node_cache_count++;
    }
}

static uint32_t squashfs_vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static void squashfs_vfs_open(vfs_node_t *node, uint32_t flags);
static void squashfs_vfs_close(vfs_node_t *node);
static dirent_t *squashfs_vfs_readdir(vfs_node_t *node, uint32_t index);
static vfs_node_t *squashfs_vfs_finddir(vfs_node_t *node, const char *name);
static int squashfs_load_inode_metadata(uint64_t inode_ref, uint8_t **out_meta, uint32_t *out_meta_size, uint32_t *out_offset, squashfs_base_inode_t **out_base, int *out_need_free);
static int squashfs_read_fragment_entry(uint32_t fragment_index, squashfs_fragment_entry_t *out_entry);

static uint16_t read_u16(uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | 
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(uint8_t *p) {
    uint32_t lo;
    uint32_t hi;

    lo = read_u32(p);
    hi = read_u32(p + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

int squashfs_decompress(uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_len, uint16_t comp_id) {
    (void)comp_id;
    
    if (src_len > dst_len) {
        return -1;
    }
    memcpy(dst, src, src_len);
    return (int)src_len;
}

static uint8_t *squashfs_read_metadata_block(uint64_t block_offset, uint32_t *out_size) {
    uint8_t *base;
    uint16_t header;
    uint32_t data_size;
    int compressed;
    uint8_t *result;
    uint8_t *src;
    int decomp_ret;

    base = squashfs_ctx.base;
    if (block_offset + 2 > squashfs_ctx.size) {
        DEBUG_FS_OTHER("block_offset 0x%llX + 2 > size 0x%X\n", (unsigned long long)block_offset, squashfs_ctx.size);
        return NULL;
    }
    
    header = read_u16(base + block_offset);
    compressed = !(header & 0x8000);
    data_size = header & 0x7FFF;
    
    DEBUG_FS_OTHER("metadata header=0x%04X compressed=%d data_size=%u\n", header, compressed, data_size);
    
    if (block_offset + 2 + data_size > squashfs_ctx.size) {
        DEBUG_FS_OTHER("block data exceeds image size\n");
        return NULL;
    }
    
    src = base + block_offset + 2;
    if (!compressed) {
        if (out_size) *out_size = data_size;
        result = kmalloc(data_size);
        if (!result) return NULL;
        memcpy(result, src, data_size);
        return result;
    }

    result = kmalloc(8192);
    if (!result) {
        DEBUG_FS_OTHER("kmalloc failed for metadata block\n");
        return NULL;
    }
    
    decomp_ret = squashfs_decompress(src, data_size, result, 8192, squashfs_ctx.compression_id);
    if (decomp_ret < 0) {
        DEBUG_FS_OTHER("decompression failed\n");
        kfree(result);
        return NULL;
    }
    if (out_size) *out_size = (uint32_t)decomp_ret;
    
    return result;
}

static int squashfs_load_inode_metadata(uint64_t inode_ref, uint8_t **out_meta, uint32_t *out_meta_size, uint32_t *out_offset, squashfs_base_inode_t **out_base, int *out_need_free) {
    uint32_t block;
    uint16_t offset;
    uint64_t block_offset;
    uint8_t *metadata;
    uint32_t meta_size;
    squashfs_base_inode_t *base;
    uint16_t meta_header;
    uint32_t meta_data_size;
    int need_free;

    if (!out_meta || !out_meta_size || !out_offset || !out_base || !out_need_free) {
        return -1;
    }

    block = (uint32_t)(inode_ref >> 16);
    offset = (uint16_t)(inode_ref & 0xFFFF);
    block_offset = squashfs_ctx.inode_table_start + block;

    need_free = 0;
    if (block_offset + 2 <= squashfs_ctx.size) {
        meta_header = read_u16(squashfs_ctx.base + block_offset);
        if (meta_header & 0x8000) {
            meta_data_size = meta_header & 0x7FFF;
            if (block_offset + 2 + meta_data_size <= squashfs_ctx.size) {
                metadata = squashfs_ctx.base + block_offset + 2;
                meta_size = meta_data_size;
            } else {
                metadata = squashfs_read_metadata_block(block_offset, &meta_size);
                if (!metadata) return -1;
                need_free = 1;
            }
        } else {
            metadata = squashfs_read_metadata_block(block_offset, &meta_size);
            if (!metadata) return -1;
            need_free = 1;
        }
    } else {
        metadata = squashfs_read_metadata_block(block_offset, &meta_size);
        if (!metadata) return -1;
        need_free = 1;
    }

    if (offset >= meta_size) {
        if (need_free) kfree(metadata);
        return -1;
    }

    base = (squashfs_base_inode_t *)(metadata + offset);
    *out_meta = metadata;
    *out_meta_size = meta_size;
    *out_offset = offset;
    *out_base = base;
    *out_need_free = need_free;
    return 0;
}

static int squashfs_read_fragment_entry(uint32_t fragment_index, squashfs_fragment_entry_t *out_entry) {
    uint32_t entries_per_block;
    uint32_t table_index;
    uint32_t entry_index;
    uint64_t table_offset;
    uint64_t block_offset;
    uint8_t *metadata;
    uint32_t meta_size;
    squashfs_fragment_entry_t *entry;
    uint16_t meta_header;
    uint32_t meta_data_size;
    int need_free;

    if (!out_entry) {
        return -1;
    }
    if (!squashfs_ctx.fragment_count || fragment_index >= squashfs_ctx.fragment_count) {
        return -1;
    }

    entries_per_block = 8192 / sizeof(squashfs_fragment_entry_t);
    table_index = fragment_index / entries_per_block;
    entry_index = fragment_index % entries_per_block;
    table_offset = squashfs_ctx.fragment_table_start + ((uint64_t)table_index * 8);

    if (table_offset + 8 > squashfs_ctx.size) {
        return -1;
    }

    block_offset = read_u64(squashfs_ctx.base + table_offset);
    if (block_offset == SQUASHFS_INVALID_BLK) {
        return -1;
    }

    need_free = 0;
    if (block_offset + 2 <= squashfs_ctx.size) {
        meta_header = read_u16(squashfs_ctx.base + block_offset);
        if (meta_header & 0x8000) {
            meta_data_size = meta_header & 0x7FFF;
            if (block_offset + 2 + meta_data_size <= squashfs_ctx.size) {
                metadata = squashfs_ctx.base + block_offset + 2;
                meta_size = meta_data_size;
            } else {
                metadata = squashfs_read_metadata_block(block_offset, &meta_size);
                if (!metadata) return -1;
                need_free = 1;
            }
        } else {
            metadata = squashfs_read_metadata_block(block_offset, &meta_size);
            if (!metadata) return -1;
            need_free = 1;
        }
    } else {
        metadata = squashfs_read_metadata_block(block_offset, &meta_size);
        if (!metadata) return -1;
        need_free = 1;
    }

    if ((entry_index + 1) * sizeof(squashfs_fragment_entry_t) > meta_size) {
        if (need_free) kfree(metadata);
        return -1;
    }

    entry = (squashfs_fragment_entry_t *)(metadata + entry_index * sizeof(squashfs_fragment_entry_t));
    memcpy(out_entry, entry, sizeof(squashfs_fragment_entry_t));
    if (need_free) kfree(metadata);
    return 0;
}

static void *squashfs_read_inode(uint64_t inode_ref) {
    uint32_t block;
    uint16_t offset;
    uint64_t block_offset;
    uint8_t *metadata;
    uint32_t meta_size;
    squashfs_base_inode_t *base;
    squashfs_symlink_inode_t *stmp;
    size_t inode_size;
    void *inode_copy;
    uint16_t meta_header;
    uint32_t meta_data_size;
    int need_free;

    block = (uint32_t)(inode_ref >> 16);
    offset = (uint16_t)(inode_ref & 0xFFFF);
    
    DEBUG_FS_OTHER("read_inode ref=0x%llX block=%u offset=0x%X\n", (unsigned long long)inode_ref, block, offset);
    
    block_offset = squashfs_ctx.inode_table_start + block;

    need_free = 0;
    if (block_offset + 2 <= squashfs_ctx.size) {
        meta_header = read_u16(squashfs_ctx.base + block_offset);
        if (meta_header & 0x8000) {
            meta_data_size = meta_header & 0x7FFF;
            if (block_offset + 2 + meta_data_size <= squashfs_ctx.size) {
                metadata = squashfs_ctx.base + block_offset + 2;
                meta_size = meta_data_size;
            } else {
                metadata = squashfs_read_metadata_block(block_offset, &meta_size);
                if (!metadata) return NULL;
                need_free = 1;
            }
        } else {
            metadata = squashfs_read_metadata_block(block_offset, &meta_size);
            if (!metadata) return NULL;
            need_free = 1;
        }
    } else {
        metadata = squashfs_read_metadata_block(block_offset, &meta_size);
        if (!metadata) return NULL;
        need_free = 1;
    }
    
    if (offset >= meta_size) {
        DEBUG_FS_OTHER("inode offset 0x%X >= metadata size 0x%X\n", offset, meta_size);
        if (need_free) kfree(metadata);
        return NULL;
    }
    
    base = (squashfs_base_inode_t *)(metadata + offset);
    DEBUG_FS_OTHER("inode at offset 0x%X: type=%u mode=0x%X\n", offset, base->inode_type, base->mode);
    
    switch (base->inode_type) {
        case SQUASHFS_DIR_TYPE:
            inode_size = sizeof(squashfs_dir_inode_t);
            break;
        case SQUASHFS_LDIR_TYPE:
            inode_size = sizeof(squashfs_ldir_inode_t);
            break;
        case SQUASHFS_REG_TYPE:
            inode_size = sizeof(squashfs_reg_inode_t);
            break;
        case SQUASHFS_LREG_TYPE:
            inode_size = sizeof(squashfs_lreg_inode_t);
            break;
        case SQUASHFS_SYMLINK_TYPE:
            stmp = (squashfs_symlink_inode_t *)(metadata + offset);
            inode_size = sizeof(squashfs_symlink_inode_t) + stmp->symlink_size;
            break;
        case SQUASHFS_LSYMLINK_TYPE:
            stmp = (squashfs_symlink_inode_t *)(metadata + offset);
            inode_size = sizeof(squashfs_symlink_inode_t) + stmp->symlink_size + 4;
            break;
        default:
            inode_size = sizeof(squashfs_base_inode_t);
            break;
    }
    
    inode_copy = kmalloc(inode_size);
    if (!inode_copy) {
        if (need_free) kfree(metadata);
        return NULL;
    }
    
    if (offset + inode_size > meta_size) {
        inode_size = meta_size - offset;
    }
    memcpy(inode_copy, metadata + offset, inode_size);
    
    if (need_free) kfree(metadata);
    return inode_copy;
}

static vfs_node_t *squashfs_create_vfs_node(uint64_t inode_ref, const char *name) {
    squashfs_vfs_node_t *snode;
    squashfs_base_inode_t *base;
    squashfs_dir_inode_t *dir;
    squashfs_ldir_inode_t *ldir;
    vfs_node_t *cached;

    cached = sqfs_cache_lookup(inode_ref);
    if (cached) return cached;
    squashfs_reg_inode_t *reg;
    squashfs_lreg_inode_t *lreg;
    squashfs_symlink_inode_t *sym;
    size_t name_len;

    snode = (squashfs_vfs_node_t *)kmalloc(sizeof(squashfs_vfs_node_t));
    if (!snode) {
        return NULL;
    }
    memset(snode, 0, sizeof(squashfs_vfs_node_t));
    
    snode->inode_ref = inode_ref;
    
    base = (squashfs_base_inode_t *)squashfs_read_inode(inode_ref);
    if (!base) {
        DEBUG_FS_OTHER("Failed to read inode 0x%llX for '%s'\n", (unsigned long long)inode_ref, name);
        kfree(snode);
        return NULL;
    }
    
    name_len = strlen(name);
    if (name_len >= VFS_MAX_NAME) {
        name_len = VFS_MAX_NAME - 1;
    }
    memcpy(snode->vfs.name, name, name_len);
    snode->vfs.name[name_len] = '\0';
    
    snode->vfs.mask = base->mode & 0777;
    snode->vfs.uid = base->uid_idx;
    snode->vfs.gid = base->gid_idx;
    snode->vfs.mtime = base->mtime;
    snode->vfs.inode = base->inode_number;
    snode->vfs.private_data = snode;
    
    switch (base->inode_type) {
        case SQUASHFS_DIR_TYPE:
            dir = (squashfs_dir_inode_t *)base;
            snode->vfs.flags = VFS_DIRECTORY;
            snode->vfs.length = dir->file_size;
            snode->start_block = dir->start_block;
            snode->dir_block_offset = dir->offset;
            snode->parent_inode = dir->parent_inode;
            snode->vfs.readdir = squashfs_vfs_readdir;
            snode->vfs.finddir = squashfs_vfs_finddir;
            break;
            
        case SQUASHFS_LDIR_TYPE:
            ldir = (squashfs_ldir_inode_t *)base;
            snode->vfs.flags = VFS_DIRECTORY;
            snode->vfs.length = ldir->file_size;
            snode->start_block = ldir->start_block;
            snode->dir_block_offset = ldir->offset;
            snode->parent_inode = ldir->parent_inode;
            snode->vfs.readdir = squashfs_vfs_readdir;
            snode->vfs.finddir = squashfs_vfs_finddir;
            break;
            
        case SQUASHFS_REG_TYPE:
            reg = (squashfs_reg_inode_t *)base;
            snode->vfs.flags = VFS_FILE;
            snode->vfs.length = reg->file_size;
            snode->start_block = reg->start_block;
            snode->vfs.read = squashfs_vfs_read;
            break;
            
        case SQUASHFS_LREG_TYPE:
            lreg = (squashfs_lreg_inode_t *)base;
            snode->vfs.flags = VFS_FILE;
            snode->vfs.length = (uint32_t)lreg->file_size;
            snode->start_block = (uint32_t)lreg->start_block;
            snode->vfs.read = squashfs_vfs_read;
            break;
            
        case SQUASHFS_SYMLINK_TYPE:
        case SQUASHFS_LSYMLINK_TYPE:
            sym = (squashfs_symlink_inode_t *)base;
            snode->vfs.flags = VFS_SYMLINK;
            snode->vfs.length = sym->symlink_size;
            snode->vfs.read = squashfs_vfs_read;
            break;
            
        default:
            snode->vfs.flags = VFS_FILE;
            snode->vfs.length = 0;
            break;
    }
    
    snode->vfs.open = squashfs_vfs_open;
    snode->vfs.close = squashfs_vfs_close;
    
    kfree(base);
    sqfs_cache_insert(inode_ref, &snode->vfs);
    return &snode->vfs;
}

static uint32_t squashfs_read_file_data(uint64_t inode_ref, uint32_t offset, uint32_t size, uint8_t *buffer) {
    uint8_t *metadata;
    uint32_t meta_size;
    uint32_t inode_offset;
    squashfs_base_inode_t *base;
    squashfs_reg_inode_t *reg;
    squashfs_lreg_inode_t *lreg;
    uint32_t file_size;
    uint32_t start_block;
    uint32_t fragment;
    uint32_t frag_offset;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t tail_size;
    uint32_t to_read;
    uint32_t bytes_read;
    uint32_t i;
    uint32_t cur_offset;
    uint32_t data_pos;
    uint32_t entry_size;
    uint32_t stored_size;
    uint32_t uncompressed_size;
    uint32_t block_offset;
    uint32_t copy_len;
    uint32_t block_list_offset;
    uint8_t *block_list;
    int compressed;
    uint8_t *temp;
    int decomp_ret;
    squashfs_fragment_entry_t frag_entry;
    uint32_t frag_stored_size;
    uint32_t frag_size;
    int frag_compressed;
    uint32_t frag_file_offset;
    uint32_t frag_offset_in_file;
    uint32_t frag_copy_len;
    int meta_need_free;

    metadata = NULL;
    meta_size = 0;
    inode_offset = 0;
    base = NULL;
    block_size = squashfs_ctx.block_size;
    meta_need_free = 0;

    DEBUG_FS_OTHER("read_file_data inode_ref=0x%llX off=%u size=%u\n", (unsigned long long)inode_ref, offset, size);

    if (squashfs_load_inode_metadata(inode_ref, &metadata, &meta_size, &inode_offset, &base, &meta_need_free) != 0) {
        DEBUG_FS_OTHER("read_file_data: load_inode_metadata failed\n");
        return 0;
    }

    DEBUG_FS_OTHER("read_file_data: inode_type=%u meta_size=%u inode_offset=%u\n", base->inode_type, meta_size, inode_offset);

    if (base->inode_type == SQUASHFS_REG_TYPE) {
        reg = (squashfs_reg_inode_t *)base;
        file_size = reg->file_size;
        start_block = reg->start_block;
        fragment = reg->fragment;
        frag_offset = reg->offset;
        block_list_offset = inode_offset + sizeof(squashfs_reg_inode_t);
        DEBUG_FS_OTHER("REG file_size=%u start_block=0x%X frag=%u frag_off=%u\n", file_size, start_block, fragment, frag_offset);
    } else if (base->inode_type == SQUASHFS_LREG_TYPE) {
        lreg = (squashfs_lreg_inode_t *)base;
        file_size = (uint32_t)lreg->file_size;
        start_block = (uint32_t)lreg->start_block;
        fragment = lreg->fragment;
        frag_offset = lreg->offset;
        block_list_offset = inode_offset + sizeof(squashfs_lreg_inode_t);
        DEBUG_FS_OTHER("LREG file_size=%u start_block=0x%X frag=%u frag_off=%u\n", file_size, start_block, fragment, frag_offset);
    } else {
        DEBUG_FS_OTHER("read_file_data: not a regular file (type=%u)\n", base->inode_type);
        if (meta_need_free) kfree(metadata);
        return 0;
    }

    if (offset >= file_size) {
        if (meta_need_free) kfree(metadata);
        return 0;
    }

    to_read = size;
    if (offset + to_read > file_size) {
        to_read = file_size - offset;
    }

    if (fragment == SQUASHFS_INVALID_FRAG) {
        block_count = (file_size + block_size - 1) / block_size;
    } else {
        block_count = file_size / block_size;
    }
    tail_size = file_size % block_size;

    DEBUG_FS_OTHER("block_count=%u tail_size=%u block_size=%u block_list_offset=%u\n", block_count, tail_size, block_size, block_list_offset);

    if (block_count > 0) {
        if (block_list_offset + block_count * 4 > meta_size) {
            DEBUG_FS_OTHER("block_list overflow: offset=%u + count*4=%u > meta_size=%u\n", block_list_offset, block_count * 4, meta_size);
            if (meta_need_free) kfree(metadata);
            return 0;
        }
        block_list = metadata + block_list_offset;
    } else {
        block_list = NULL;
    }

    bytes_read = 0;
    cur_offset = 0;
    data_pos = start_block;

    for (i = 0; i < block_count && bytes_read < to_read; i++) {
        entry_size = read_u32(block_list + i * 4);
        compressed = !(entry_size & 0x01000000);
        stored_size = entry_size & 0x00FFFFFF;
        uncompressed_size = block_size;

        if (stored_size == 0 || stored_size > block_size * 2) {
            DEBUG_FS_OTHER("invalid block[%u] stored_size=%u (entry=0x%08X)\n", i, stored_size, entry_size);
            break;
        }

        if (data_pos + stored_size > squashfs_ctx.size) {
            DEBUG_FS_OTHER("block[%u] exceeds image: pos=0x%X + size=%u > 0x%X\n", i, data_pos, stored_size, squashfs_ctx.size);
            break;
        }

        if (fragment == SQUASHFS_INVALID_FRAG && tail_size && i == block_count - 1) {
            uncompressed_size = tail_size;
        }

        if (offset < cur_offset + uncompressed_size && offset + to_read > cur_offset) {
            if (offset > cur_offset) {
                block_offset = offset - cur_offset;
            } else {
                block_offset = 0;
            }

            copy_len = uncompressed_size - block_offset;
            if (copy_len > to_read - bytes_read) {
                copy_len = to_read - bytes_read;
            }

            if (compressed) {
                temp = (uint8_t *)kmalloc(uncompressed_size);
                if (!temp) {
                    DEBUG_FS_OTHER("OOM allocating %u bytes for block decompression\n", uncompressed_size);
                    break;
                }
                decomp_ret = squashfs_decompress(squashfs_ctx.base + data_pos, stored_size, temp, uncompressed_size, squashfs_ctx.compression_id);
                if (decomp_ret < 0) {
                    printf("SQUASHFS: decompression failed for block[%u] stored=%u uncomp=%u\n", i, stored_size, uncompressed_size);
                    kfree(temp);
                    break;
                }
                if (block_offset + copy_len > (uint32_t)decomp_ret) {
                    if ((uint32_t)decomp_ret > block_offset) {
                        copy_len = (uint32_t)decomp_ret - block_offset;
                    } else {
                        copy_len = 0;
                    }
                }
                if (copy_len > 0) {
                    memcpy(buffer + bytes_read, temp + block_offset, copy_len);
                }
                kfree(temp);
            } else {
                if (data_pos + block_offset + copy_len > squashfs_ctx.size) {
                    printf("SQUASHFS: uncompressed read exceeds image\n");
                    break;
                }
                memcpy(buffer + bytes_read, squashfs_ctx.base + data_pos + block_offset, copy_len);
            }
            bytes_read += copy_len;
        }

        data_pos += stored_size;
        cur_offset += uncompressed_size;
    }

    if (bytes_read < to_read && fragment != SQUASHFS_INVALID_FRAG && tail_size) {
        frag_file_offset = block_count * block_size;
        if (offset < frag_file_offset + tail_size && offset + to_read > frag_file_offset) {
            if (offset > frag_file_offset) {
                frag_offset_in_file = offset - frag_file_offset;
            } else {
                frag_offset_in_file = 0;
            }

            frag_copy_len = tail_size - frag_offset_in_file;
            if (frag_copy_len > to_read - bytes_read) {
                frag_copy_len = to_read - bytes_read;
            }

            if (squashfs_read_fragment_entry(fragment, &frag_entry) == 0) {
                frag_stored_size = frag_entry.size;
                frag_compressed = !(frag_stored_size & 0x01000000);
                frag_size = frag_stored_size & 0x00FFFFFF;

                if (frag_size == 0 || frag_size > block_size * 2) {
                    printf("SQUASHFS: invalid fragment size=%u (raw=0x%08X)\n", frag_size, frag_stored_size);
                } else if (frag_compressed) {
                    temp = (uint8_t *)kmalloc(block_size);
                    if (temp) {
                        decomp_ret = squashfs_decompress(squashfs_ctx.base + (uint32_t)frag_entry.start_block, frag_size, temp, block_size, squashfs_ctx.compression_id);
                        if (decomp_ret >= 0) {
                            if ((uint32_t)decomp_ret >= frag_offset + frag_offset_in_file + frag_copy_len) {
                                memcpy(buffer + bytes_read, temp + frag_offset + frag_offset_in_file, frag_copy_len);
                                bytes_read += frag_copy_len;
                            }
                        } else {
                            printf("SQUASHFS: fragment decompression failed\n");
                        }
                        kfree(temp);
                    }
                } else {
                    if ((uint64_t)frag_entry.start_block + frag_offset + frag_offset_in_file + frag_copy_len <= squashfs_ctx.size) {
                        memcpy(buffer + bytes_read, squashfs_ctx.base + (uint32_t)frag_entry.start_block + frag_offset + frag_offset_in_file, frag_copy_len);
                        bytes_read += frag_copy_len;
                    } else {
                        printf("SQUASHFS: fragment read exceeds image\n");
                    }
                }
            }
        }
    }

    if (bytes_read == 0 && to_read > 0 && file_size > 0) {
        printf("SQUASHFS: ERROR: 0-byte read for non-empty file (file_size=%u, requested=%u at offset=%u)\n", file_size, to_read, offset);
        printf("SQUASHFS: ERROR: start_block=0x%X block_count=%u frag=%u inode_type=%u\n", start_block, block_count, fragment, base->inode_type);
    }

    if (bytes_read < to_read && (offset + bytes_read) < file_size) {
        printf("SQUASHFS: WARNING: short read %u/%u at offset %u (file_size=%u, EOF not reached)\n", bytes_read, to_read, offset, file_size);
    }

    if (meta_need_free) kfree(metadata);
    return bytes_read;
}

static uint32_t squashfs_vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    squashfs_vfs_node_t *snode;
    squashfs_symlink_inode_t *sym;
    char *symlink_target;
    uint32_t copy_len;

    if (!node || !buffer) return 0;
    
    snode = (squashfs_vfs_node_t *)node->private_data;
    if (!snode) return 0;
    
    if (VFS_GET_TYPE(node->flags) == VFS_SYMLINK) {
        sym = (squashfs_symlink_inode_t *)squashfs_read_inode(snode->inode_ref);
        if (!sym) return 0;
        
        symlink_target = (char *)((uint8_t *)sym + sizeof(squashfs_symlink_inode_t));
        copy_len = sym->symlink_size;
        if (offset >= copy_len) {
            kfree(sym);
            return 0;
        }
        copy_len -= offset;
        if (copy_len > size) copy_len = size;
        memcpy(buffer, symlink_target + offset, copy_len);
        kfree(sym);
        return copy_len;
    }
    
    return squashfs_read_file_data(snode->inode_ref, offset, size, buffer);
}

static void squashfs_vfs_open(vfs_node_t *node, uint32_t flags) {
    (void)node;
    (void)flags;
}

static void squashfs_vfs_close(vfs_node_t *node) {
    (void)node;
}

static dirent_t *squashfs_vfs_readdir(vfs_node_t *node, uint32_t index) {
    squashfs_vfs_node_t *snode;
    uint64_t dir_block_start;
    uint8_t *dir_data;
    uint32_t dir_size;
    uint32_t pos;
    uint32_t entry_count;
    squashfs_dir_header_t *hdr;
    uint32_t i;
    squashfs_dir_entry_t *entry;
    uint16_t actual_name_len;
    uint16_t copy_name_len;
    uint32_t dir_data_len;
    uint32_t end_pos;
    int need_free;
    uint16_t meta_header;
    uint32_t meta_data_size;

    if (!node || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    
    snode = (squashfs_vfs_node_t *)node->private_data;
    if (!snode) return NULL;

    if (snode->vfs.length <= 3) return NULL;
    dir_data_len = snode->vfs.length - 3;

    dir_block_start = squashfs_ctx.directory_table_start + snode->start_block;

    need_free = 0;
    if (dir_block_start + 2 <= squashfs_ctx.size) {
        meta_header = read_u16(squashfs_ctx.base + dir_block_start);
        if (meta_header & 0x8000) {
            meta_data_size = meta_header & 0x7FFF;
            if (dir_block_start + 2 + meta_data_size <= squashfs_ctx.size) {
                dir_data = squashfs_ctx.base + dir_block_start + 2;
                dir_size = meta_data_size;
            } else {
                dir_data = squashfs_read_metadata_block(dir_block_start, &dir_size);
                if (!dir_data) return NULL;
                need_free = 1;
            }
        } else {
            dir_data = squashfs_read_metadata_block(dir_block_start, &dir_size);
            if (!dir_data) return NULL;
            need_free = 1;
        }
    } else {
        dir_data = squashfs_read_metadata_block(dir_block_start, &dir_size);
        if (!dir_data) return NULL;
        need_free = 1;
    }

    end_pos = snode->dir_block_offset + dir_data_len;
    if (end_pos > dir_size) end_pos = dir_size;
    
    pos = snode->dir_block_offset;
    entry_count = 0;
    
    while (pos + sizeof(squashfs_dir_header_t) <= end_pos) {
        hdr = (squashfs_dir_header_t *)(dir_data + pos);
        pos += sizeof(squashfs_dir_header_t);
        
        for (i = 0; i <= hdr->count && pos + sizeof(squashfs_dir_entry_t) <= end_pos; i++) {
            entry = (squashfs_dir_entry_t *)(dir_data + pos);
            actual_name_len = entry->name_size + 1;
            copy_name_len = actual_name_len;
            
            if (entry_count == index) {
                if (copy_name_len >= VFS_MAX_NAME) {
                    copy_name_len = VFS_MAX_NAME - 1;
                }
                memcpy(squashfs_dirent.name, dir_data + pos + sizeof(squashfs_dir_entry_t), copy_name_len);
                squashfs_dirent.name[copy_name_len] = '\0';
                squashfs_dirent.inode = hdr->inode_number + entry->inode_offset;
                
                switch (entry->type) {
                    case SQUASHFS_DIR_TYPE:
                    case SQUASHFS_LDIR_TYPE:
                        squashfs_dirent.type = VFS_DIRECTORY;
                        break;
                    case SQUASHFS_SYMLINK_TYPE:
                    case SQUASHFS_LSYMLINK_TYPE:
                        squashfs_dirent.type = VFS_SYMLINK;
                        break;
                    default:
                        squashfs_dirent.type = VFS_FILE;
                        break;
                }
                
                if (need_free) kfree(dir_data);
                return &squashfs_dirent;
            }
            
            entry_count++;
            pos += sizeof(squashfs_dir_entry_t) + actual_name_len;
        }
    }
    
    if (need_free) kfree(dir_data);
    return NULL;
}

static vfs_node_t *squashfs_vfs_finddir(vfs_node_t *node, const char *name) {
    squashfs_vfs_node_t *snode;
    uint64_t dir_block_start;
    uint8_t *dir_data;
    uint32_t dir_size;
    uint32_t pos;
    squashfs_dir_header_t *hdr;
    uint32_t i;
    squashfs_dir_entry_t *entry;
    uint16_t actual_name_len;
    uint16_t copy_name_len;
    char entry_name[VFS_MAX_NAME];
    uint64_t inode_ref;
    vfs_node_t *result;
    uint32_t dir_data_len;
    uint32_t end_pos;
    int need_free;
    uint16_t meta_header;
    uint32_t meta_data_size;

    if (!node || !name || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    
    snode = (squashfs_vfs_node_t *)node->private_data;
    if (!snode) return NULL;

    if (snode->vfs.length <= 3) return NULL;
    dir_data_len = snode->vfs.length - 3;

    dir_block_start = squashfs_ctx.directory_table_start + snode->start_block;

    need_free = 0;
    if (dir_block_start + 2 <= squashfs_ctx.size) {
        meta_header = read_u16(squashfs_ctx.base + dir_block_start);
        if (meta_header & 0x8000) {
            meta_data_size = meta_header & 0x7FFF;
            if (dir_block_start + 2 + meta_data_size <= squashfs_ctx.size) {
                dir_data = squashfs_ctx.base + dir_block_start + 2;
                dir_size = meta_data_size;
                need_free = 0;
            } else {
                dir_data = squashfs_read_metadata_block(dir_block_start, &dir_size);
                if (!dir_data) return NULL;
                need_free = 1;
            }
        } else {
            dir_data = squashfs_read_metadata_block(dir_block_start, &dir_size);
            if (!dir_data) return NULL;
            need_free = 1;
        }
    } else {
        dir_data = squashfs_read_metadata_block(dir_block_start, &dir_size);
        if (!dir_data) return NULL;
        need_free = 1;
    }

    end_pos = snode->dir_block_offset + dir_data_len;
    if (end_pos > dir_size) end_pos = dir_size;
    
    pos = snode->dir_block_offset;
    
    while (pos + sizeof(squashfs_dir_header_t) <= end_pos) {
        hdr = (squashfs_dir_header_t *)(dir_data + pos);
        pos += sizeof(squashfs_dir_header_t);
        
        for (i = 0; i <= hdr->count && pos + sizeof(squashfs_dir_entry_t) <= end_pos; i++) {
            entry = (squashfs_dir_entry_t *)(dir_data + pos);
            actual_name_len = entry->name_size + 1;
            copy_name_len = actual_name_len;
            
            if (copy_name_len >= VFS_MAX_NAME) {
                copy_name_len = VFS_MAX_NAME - 1;
            }
            memcpy(entry_name, dir_data + pos + sizeof(squashfs_dir_entry_t), copy_name_len);
            entry_name[copy_name_len] = '\0';
            
            if (strcmp(entry_name, name) == 0) {
                inode_ref = ((uint64_t)hdr->start_block << 16) | entry->offset;
                result = squashfs_create_vfs_node(inode_ref, name);
                if (result) {
                    result->parent = node;
                }
                if (need_free) kfree(dir_data);
                return result;
            }
            
            pos += sizeof(squashfs_dir_entry_t) + actual_name_len;
        }
    }
    
    if (need_free) kfree(dir_data);
    return NULL;
}

static vfs_node_t *squashfs_vfs_do_mount(const char *device, const char *mountpoint) {
    (void)device;
    (void)mountpoint;
    
    if (!squashfs_initialized || !squashfs_vfs_root) {
        printf("SQUASHFS: Not initialized\n");
        return NULL;
    }
    
    printf("SQUASHFS: Mounted on %s\n", mountpoint);
    return squashfs_vfs_root;
}

void squashfs_init(uint32_t mod_start, uint32_t mod_end) {
    uint32_t mod_size;
    uint32_t start_page;
    uint32_t end_page;
    uint32_t phys;
    uint32_t virt;
    squashfs_super_t *super;
    
    mod_size = mod_end - mod_start;
    printf("SQUASHFS: Initializing from phys 0x%08X - 0x%08X (%u bytes)\n", 
           mod_start, mod_end, mod_size);
    
    if (mod_size < sizeof(squashfs_super_t)) {
        printf("SQUASHFS: Image too small\n");
        return;
    }
    
    start_page = mod_start & ~0xFFF;
    end_page = (mod_end + 0xFFF) & ~0xFFF;
    for (phys = start_page; phys < end_page; phys += 0x1000) {
        virt = phys + 0xC0000000;
        vmm_map_page(virt, phys, 0x003);
    }
    
    squashfs_ctx.base = (uint8_t *)(mod_start + 0xC0000000);
    squashfs_ctx.size = mod_size;
    
    super = (squashfs_super_t *)squashfs_ctx.base;
    
    if (super->magic != SQUASHFS_MAGIC && super->magic != SQUASHFS_MAGIC_SWAP) {
        printf("SQUASHFS: Invalid magic 0x%08X (expected 0x%08X)\n", 
               super->magic, SQUASHFS_MAGIC);
        return;
    }
    
    if (super->version_major != 4) {
        printf("SQUASHFS: Unsupported version %u.%u (need 4.x)\n",
               super->version_major, super->version_minor);
        return;
    }
    
    squashfs_ctx.super = super;
    squashfs_ctx.block_size = super->block_size;
    squashfs_ctx.compression_id = super->compression_id;
    squashfs_ctx.inode_table_start = super->inode_table_start;
    squashfs_ctx.directory_table_start = super->directory_table_start;
    squashfs_ctx.fragment_table_start = super->fragment_table_start;
    squashfs_ctx.id_table_start = super->id_table_start;
    squashfs_ctx.fragment_count = super->fragment_entry_count;
    
    printf("SQUASHFS: block_size=%u compression=%u inodes=%u\n",
           squashfs_ctx.block_size, squashfs_ctx.compression_id, super->inode_count);
    printf("SQUASHFS: root_inode=0x%llX\n", (unsigned long long)super->root_inode);
    
    printf("SQUASHFS: Attempting to create root node from inode 0x%llX\n", (unsigned long long)super->root_inode);
    printf("SQUASHFS: inode_table_start=0x%llX\n", (unsigned long long)squashfs_ctx.inode_table_start);
    squashfs_vfs_root = squashfs_create_vfs_node(super->root_inode, "/");
    if (!squashfs_vfs_root) {
        printf("SQUASHFS: Failed to create root node\n");
        return;
    }
    
    squashfs_initialized = 1;
    printf("SQUASHFS: Initialized successfully\n");
}

void squashfs_vfs_register(void) {
    squashfs_fs_type.name = "squashfs";
    squashfs_fs_type.mount = squashfs_vfs_do_mount;
    squashfs_fs_type.unmount = NULL;
    squashfs_fs_type.next = NULL;
    
    vfs_register_fs(&squashfs_fs_type);
}

squashfs_context_t *squashfs_get_context(void) {
    if (!squashfs_initialized) {
        return NULL;
    }
    return &squashfs_ctx;
}

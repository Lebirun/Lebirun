#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/mem_map.h>
#include <string.h>

extern void ext4_mark_block_dirty(ext4_fs_t *fs, uint64_t block);
extern void ext4_mark_inode_dirty(ext4_inode_cache_t *ic);

#define EXT_INIT_MAX_LEN 32768u

typedef struct {
    ext4_extent_header_t *hdr;
    uint64_t block;
    uint16_t index;
} ext_level_t;

typedef struct {
    uint64_t start;
    uint64_t count;
} ext_range_t;

typedef struct {
    ext_range_t *ranges;
    uint32_t count;
    uint32_t capacity;
    uint64_t blocks;
} ext_free_list_t;

static uint64_t ext_idx_leaf(const ext4_extent_idx_t *idx) {
    return idx->ei_leaf_lo | ((uint64_t)idx->ei_leaf_hi << 32);
}

static uint64_t ext_start(const ext4_extent_t *e) {
    return e->ee_start_lo | ((uint64_t)e->ee_start_hi << 32);
}

static void ext_set_start(ext4_extent_t *e, uint64_t phys) {
    e->ee_start_lo = (uint32_t)phys;
    e->ee_start_hi = (uint16_t)(phys >> 32);
}

static void ext_set_leaf(ext4_extent_idx_t *idx, uint64_t phys) {
    idx->ei_leaf_lo = (uint32_t)phys;
    idx->ei_leaf_hi = (uint16_t)(phys >> 32);
}

static uint16_t ext_node_max(ext4_fs_t *fs) {
    return (uint16_t)((fs->block_size - sizeof(ext4_extent_header_t)) /
                      sizeof(ext4_extent_t));
}

void ext4_inode_add_blocks(ext4_fs_t *fs, ext4_inode_t *inode, int64_t blocks) {
    uint64_t count;

    count = inode->i_blocks_lo | ((uint64_t)inode->i_blocks_high << 32);
    if (!(inode->i_flags & EXT4_INODE_FLAG_HUGE_FILE))
        blocks *= (int64_t)fs->sectors_per_block;
    count = blocks < 0 && (uint64_t)-blocks > count ? 0 : count + (uint64_t)blocks;
    inode->i_blocks_lo = (uint32_t)count;
    inode->i_blocks_high = (uint16_t)(count >> 32);
}

static void ext_dirty(ext4_fs_t *fs, ext4_inode_cache_t *ic, ext_level_t *lv) {
    if (!lv->block) {
        ext4_mark_inode_dirty(ic);
        return;
    }
    ext4_extent_csum(fs, ic->ino, &ic->inode, (uint8_t *)lv->hdr);
    ext4_mark_block_dirty(fs, lv->block);
}

static void ext_release(ext4_fs_t *fs, ext_level_t *path, int levels) {
    int i;

    for (i = 1; i < levels; i++)
        if (path[i].block) ext4_release_block(fs, path[i].block);
}

static int ext_walk(ext4_fs_t *fs, ext4_inode_cache_t *ic, uint64_t logical,
                    ext_level_t *path) {
    ext4_extent_header_t *hdr;
    ext4_extent_idx_t *idx;
    uint16_t depth;
    uint16_t i;
    int level;
    uint64_t child;

    hdr = (ext4_extent_header_t *)ic->inode.i_block;
    depth = hdr->eh_depth;
    if (hdr->eh_magic != EXT4_EXTENT_MAGIC || depth > EXT4_EXT_MAX_DEPTH) return -1;
    path[0].hdr = hdr;
    path[0].block = 0;
    for (level = 0; level < depth; level++) {
        hdr = path[level].hdr;
        if (hdr->eh_entries == 0 || hdr->eh_depth != depth - level) {
            ext_release(fs, path, level + 1);
            return -1;
        }
        idx = (ext4_extent_idx_t *)(hdr + 1);
        for (i = 1; i < hdr->eh_entries && idx[i].ei_block <= logical; i++);
        path[level].index = i - 1;
        child = ext_idx_leaf(&idx[i - 1]);
        hdr = (ext4_extent_header_t *)ext4_get_block(fs, child);
        if (!hdr) {
            ext_release(fs, path, level + 1);
            return -1;
        }
        path[level + 1].hdr = hdr;
        path[level + 1].block = child;
        if (hdr->eh_magic != EXT4_EXTENT_MAGIC || hdr->eh_entries > hdr->eh_max) {
            ext_release(fs, path, level + 2);
            return -1;
        }
    }
    return depth + 1;
}

static void ext_fix_keys(ext4_fs_t *fs, ext4_inode_cache_t *ic,
                         ext_level_t *path, int level, uint32_t key) {
    ext4_extent_idx_t *idx;

    while (level > 0) {
        level--;
        idx = (ext4_extent_idx_t *)(path[level].hdr + 1) + path[level].index;
        if (idx->ei_block == key) return;
        idx->ei_block = key;
        ext_dirty(fs, ic, &path[level]);
        if (path[level].index != 0) return;
    }
}

static uint64_t ext_new_node(ext4_fs_t *fs, ext4_inode_cache_t *ic,
                             uint64_t hint, uint16_t depth,
                             ext4_extent_header_t **out) {
    int64_t block;
    ext4_extent_header_t *hdr;

    block = ext4_alloc_block(fs, hint);
    if (block < 0) return 0;
    hdr = (ext4_extent_header_t *)ext4_get_block_overwrite(fs, (uint64_t)block);
    if (!hdr) {
        ext4_free_block(fs, (uint64_t)block);
        return 0;
    }
    memset(hdr, 0, fs->block_size);
    hdr->eh_magic = EXT4_EXTENT_MAGIC;
    hdr->eh_max = ext_node_max(fs);
    hdr->eh_depth = depth;
    ext4_inode_add_blocks(fs, &ic->inode, 1);
    *out = hdr;
    return (uint64_t)block;
}

static int ext_grow_root(ext4_fs_t *fs, ext4_inode_cache_t *ic) {
    ext4_extent_header_t *root;
    ext4_extent_header_t *hdr;
    ext4_extent_idx_t *idx;
    uint64_t block;
    uint32_t key;

    root = (ext4_extent_header_t *)ic->inode.i_block;
    if (root->eh_depth >= EXT4_EXT_MAX_DEPTH) return -1;
    block = ext_new_node(fs, ic, 0, root->eh_depth, &hdr);
    if (!block) return -1;
    memcpy(hdr + 1, root + 1, (size_t)root->eh_entries * sizeof(ext4_extent_t));
    hdr->eh_entries = root->eh_entries;
    key = root->eh_entries ? ((ext4_extent_idx_t *)(root + 1))->ei_block : 0;
    ext4_extent_csum(fs, ic->ino, &ic->inode, (uint8_t *)hdr);
    ext4_mark_block_dirty(fs, block);
    ext4_release_block(fs, block);
    root->eh_depth++;
    root->eh_entries = 1;
    idx = (ext4_extent_idx_t *)(root + 1);
    memset(idx, 0, sizeof(*idx));
    idx->ei_block = key;
    ext_set_leaf(idx, block);
    ext4_mark_inode_dirty(ic);
    return 0;
}

static int ext_split(ext4_fs_t *fs, ext4_inode_cache_t *ic, ext_level_t *path,
                     int level, int append, uint32_t logical) {
    ext4_extent_header_t *node;
    ext4_extent_header_t *hdr;
    ext4_extent_header_t *parent;
    ext4_extent_idx_t *pidx;
    uint64_t block;
    uint16_t keep;
    uint16_t pos;
    uint32_t key;

    node = path[level].hdr;
    parent = path[level - 1].hdr;
    block = ext_new_node(fs, ic, path[level].block, node->eh_depth, &hdr);
    if (!block) return -1;
    keep = append ? node->eh_entries : node->eh_entries / 2;
    hdr->eh_entries = node->eh_entries - keep;
    memcpy(hdr + 1, (ext4_extent_t *)(node + 1) + keep,
           (size_t)hdr->eh_entries * sizeof(ext4_extent_t));
    key = hdr->eh_entries ? ((ext4_extent_idx_t *)(hdr + 1))->ei_block : logical;
    node->eh_entries = keep;
    ext4_extent_csum(fs, ic->ino, &ic->inode, (uint8_t *)hdr);
    ext4_mark_block_dirty(fs, block);
    ext4_release_block(fs, block);
    ext_dirty(fs, ic, &path[level]);
    pidx = (ext4_extent_idx_t *)(parent + 1);
    pos = path[level - 1].index + 1;
    memmove(pidx + pos + 1, pidx + pos,
            (size_t)(parent->eh_entries - pos) * sizeof(*pidx));
    memset(pidx + pos, 0, sizeof(*pidx));
    pidx[pos].ei_block = key;
    ext_set_leaf(&pidx[pos], block);
    parent->eh_entries++;
    ext_dirty(fs, ic, &path[level - 1]);
    return 0;
}

int ext4_ext_map(ext4_fs_t *fs, ext4_inode_cache_t *ic, uint64_t logical,
                 uint64_t phys) {
    ext_level_t path[EXT4_EXT_MAX_DEPTH + 1];
    ext4_extent_header_t *leaf;
    ext4_extent_t *ext;
    ext4_extent_t *prev;
    uint16_t pos;
    uint16_t len;
    int levels;
    int level;
    int result;

    if (logical > UINT32_MAX) return -1;
    for (;;) {
        levels = ext_walk(fs, ic, logical, path);
        if (levels < 0) return -1;
        leaf = path[levels - 1].hdr;
        ext = (ext4_extent_t *)(leaf + 1);
        for (pos = 0; pos < leaf->eh_entries && ext[pos].ee_block <= logical; pos++);
        prev = pos ? &ext[pos - 1] : NULL;
        if (prev) {
            len = prev->ee_len > EXT_INIT_MAX_LEN ?
                  prev->ee_len - EXT_INIT_MAX_LEN : prev->ee_len;
            if (logical < (uint64_t)prev->ee_block + len) {
                ext_release(fs, path, levels);
                return -1;
            }
            if (prev->ee_len < EXT_INIT_MAX_LEN &&
                (uint64_t)prev->ee_block + len == logical &&
                ext_start(prev) + len == phys) {
                prev->ee_len++;
                ext_dirty(fs, ic, &path[levels - 1]);
                ext_release(fs, path, levels);
                return 0;
            }
        }
        if (leaf->eh_entries < leaf->eh_max) {
            memmove(ext + pos + 1, ext + pos,
                    (size_t)(leaf->eh_entries - pos) * sizeof(*ext));
            memset(ext + pos, 0, sizeof(*ext));
            ext[pos].ee_block = (uint32_t)logical;
            ext[pos].ee_len = 1;
            ext_set_start(&ext[pos], phys);
            leaf->eh_entries++;
            ext_dirty(fs, ic, &path[levels - 1]);
            if (pos == 0) ext_fix_keys(fs, ic, path, levels - 1, (uint32_t)logical);
            ext_release(fs, path, levels);
            return 0;
        }
        for (level = levels - 1; level > 0 &&
             path[level - 1].hdr->eh_entries >= path[level - 1].hdr->eh_max; level--);
        if (level == 0)
            result = ext_grow_root(fs, ic);
        else
            result = ext_split(fs, ic, path, level,
                               level == levels - 1 && pos == leaf->eh_entries,
                               (uint32_t)logical);
        ext_release(fs, path, levels);
        if (result != 0) return -1;
    }
}

static int ext_free_add(ext_free_list_t *list, uint64_t start, uint64_t count) {
    ext_range_t *ranges;
    uint32_t capacity;

    if (!count) return 0;
    list->blocks += count;
    if (list->count && list->ranges[list->count - 1].start +
                       list->ranges[list->count - 1].count == start) {
        list->ranges[list->count - 1].count += count;
        return 0;
    }
    if (list->count == list->capacity) {
        capacity = list->capacity ? list->capacity * 2 : 8;
        ranges = (ext_range_t *)krealloc(list->ranges, capacity * sizeof(*ranges));
        if (!ranges) return -1;
        list->ranges = ranges;
        list->capacity = capacity;
    }
    list->ranges[list->count].start = start;
    list->ranges[list->count].count = count;
    list->count++;
    return 0;
}

static int ext_trunc_node(ext4_fs_t *fs, ext4_inode_cache_t *ic,
                          ext_level_t *lv, uint64_t keep,
                          ext_free_list_t *list) {
    ext4_extent_header_t *hdr;
    ext4_extent_t *ext;
    ext4_extent_idx_t *idx;
    ext_level_t child;
    uint32_t len;
    uint32_t newlen;
    uint16_t unwritten;
    int changed;
    int result;

    hdr = lv->hdr;
    changed = 0;
    if (hdr->eh_depth == 0) {
        ext = (ext4_extent_t *)(hdr + 1);
        while (hdr->eh_entries) {
            ext = (ext4_extent_t *)(hdr + 1) + hdr->eh_entries - 1;
            unwritten = ext->ee_len > EXT_INIT_MAX_LEN ? EXT_INIT_MAX_LEN : 0;
            len = ext->ee_len - unwritten;
            if (ext->ee_block >= keep) {
                if (ext_free_add(list, ext_start(ext), len) != 0) return -1;
                hdr->eh_entries--;
                changed = 1;
                continue;
            }
            if ((uint64_t)ext->ee_block + len > keep) {
                newlen = (uint32_t)(keep - ext->ee_block);
                if (ext_free_add(list, ext_start(ext) + newlen, len - newlen) != 0)
                    return -1;
                ext->ee_len = (uint16_t)(newlen + (newlen ? unwritten : 0));
                changed = 1;
            }
            break;
        }
        if (changed) ext_dirty(fs, ic, lv);
        return 0;
    }
    idx = (ext4_extent_idx_t *)(hdr + 1);
    while (hdr->eh_entries) {
        idx = (ext4_extent_idx_t *)(hdr + 1) + hdr->eh_entries - 1;
        child.block = ext_idx_leaf(idx);
        child.hdr = (ext4_extent_header_t *)ext4_get_block(fs, child.block);
        if (!child.hdr) return -1;
        result = child.hdr->eh_magic == EXT4_EXTENT_MAGIC ?
                 ext_trunc_node(fs, ic, &child, idx->ei_block >= keep ? 0 : keep, list) : -1;
        ext4_release_block(fs, child.block);
        if (result != 0) return -1;
        if (child.hdr->eh_entries) break;
        if (ext_free_add(list, child.block, 1) != 0) return -1;
        hdr->eh_entries--;
        changed = 1;
    }
    if (changed) ext_dirty(fs, ic, lv);
    return 0;
}

int ext4_ext_truncate(ext4_fs_t *fs, ext4_inode_cache_t *ic, uint64_t size) {
    ext_free_list_t list;
    ext_level_t root;
    uint64_t keep;
    uint64_t phys;
    uint8_t *block;
    uint32_t tail;
    uint32_t i;
    int result;

    keep = (size + fs->block_size - 1) / fs->block_size;
    tail = (uint32_t)(size % fs->block_size);
    if (tail) {
        phys = ext4_inode_get_block(fs, &ic->inode, keep - 1);
        block = phys ? ext4_get_block(fs, phys) : NULL;
        if (block) {
            memset(block + tail, 0, fs->block_size - tail);
            ext4_mark_block_dirty(fs, phys);
            ext4_release_block(fs, phys);
        }
    }
    memset(&list, 0, sizeof(list));
    root.hdr = (ext4_extent_header_t *)ic->inode.i_block;
    root.block = 0;
    result = root.hdr->eh_magic == EXT4_EXTENT_MAGIC ?
             ext_trunc_node(fs, ic, &root, keep, &list) : -1;
    if (result == 0 && root.hdr->eh_entries == 0) {
        root.hdr->eh_depth = 0;
        root.hdr->eh_max = (sizeof(ic->inode.i_block) - sizeof(*root.hdr)) /
                           sizeof(ext4_extent_t);
    }
    ext4_inode_set_size(&ic->inode, size);
    ext4_inode_add_blocks(fs, &ic->inode, -(int64_t)list.blocks);
    ext4_mark_inode_dirty(ic);
    if (result == 0 && list.count) result = ext4_sync(fs);
    for (i = 0; result == 0 && i < list.count; i++)
        result = ext4_free_blocks(fs, list.ranges[i].start, list.ranges[i].count);
    kfree(list.ranges);
    return result;
}

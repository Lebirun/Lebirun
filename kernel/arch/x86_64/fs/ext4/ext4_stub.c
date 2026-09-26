#include <stdint.h>
#include <stddef.h>
#include <lebirun/vfs.h>

#if !CONFIG_FS_EXT4
uint64_t ext4_transfer_write(vfs_node_t *node, uint64_t offset,
                             uint64_t size, uint8_t *buffer,
                             uint8_t *scratch,
                             uint64_t scratch_capacity) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    (void)scratch;
    (void)scratch_capacity;
    return UINT64_MAX;
}

int ext4_set_times_node(vfs_node_t *node, uint64_t atime, uint64_t mtime,
                        uint64_t ctime) {
    (void)node;
    (void)atime;
    (void)mtime;
    (void)ctime;
    return -1;
}

int ext4_mknod_node(vfs_node_t *parent, const char *name, uint64_t mode) {
    (void)parent;
    (void)name;
    (void)mode;
    return -1;
}

int ext4_exchange_nodes(vfs_node_t *old_parent, const char *old_name,
                        vfs_node_t *new_parent, const char *new_name) {
    (void)old_parent;
    (void)old_name;
    (void)new_parent;
    (void)new_name;
    return -1;
}

void ext4_reclaim_mounted_caches(uint32_t max_blocks) {
    (void)max_blocks;
}

int ext4_get_stats(uint64_t *total_blocks, uint64_t *free_blocks,
                   uint32_t *block_size) {
    (void)total_blocks;
    (void)free_blocks;
    (void)block_size;
    return -1;
}

int ext4_vfs_symlink_node(const char *target, const char *linkpath,
                          uint64_t flags) {
    (void)target;
    (void)linkpath;
    (void)flags;
    return -1;
}

int ext4_vfs_link_node(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -1;
}
#endif

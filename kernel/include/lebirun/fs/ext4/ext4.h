#ifndef EXT4_H
#define EXT4_H

#include <stdint.h>
#include <stdbool.h>
#include <lebirun/vfs.h>
#include <lebirun/mutex.h>
#include <lebirun/fs/ext4/ext4_types.h>

#define EXT4_MAX_BLOCK_SIZE     65536
#define EXT4_MIN_BLOCK_SIZE     1024
#define EXT4_CACHE_BLOCKS       4
#define EXT4_CACHE_BLOCKS_MAX   32
#define EXT4_INODE_CACHE_INIT   16
#define EXT4_INODE_CACHE_MAX    256

struct ext4_fs;

typedef struct {
    uint32_t block_num;
    uint8_t *data;
    bool dirty;
    uint32_t ref_count;
    uint32_t last_access;
} ext4_block_cache_entry_t;

typedef struct {
    uint32_t ino;
    ext4_inode_t inode;
    bool dirty;
    uint32_t ref_count;
    struct ext4_fs *fs;
    vfs_node_t *vfs_node;
} ext4_inode_cache_t;

typedef struct ext4_fs {
    uint32_t port_index;
    uint64_t partition_start_lba;
    ext4_superblock_t sb;
    uint32_t block_size;
    uint32_t inodes_per_block;
    uint32_t sectors_per_block;
    uint32_t groups_count;
    uint32_t desc_per_block;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint64_t total_blocks;
    uint64_t total_inodes;
    bool use_extents;
    bool is_64bit;
    uint32_t desc_size;
    ext4_group_desc_t *group_descs;
    ext4_block_cache_entry_t *block_cache;
    uint32_t block_cache_count;
    ext4_inode_cache_t *inode_cache;
    uint32_t inode_cache_count;
    uint32_t inode_cache_capacity;
    mutex_t lock;
    uint32_t cache_tick;
    uint32_t alloc_last_group;
    uint32_t alloc_last_bit;
    bool super_dirty;
    vfs_node_t *root_node;
    vfs_node_t *vfs_nodes;
    char mountpoint[VFS_MAX_PATH];
    struct ext4_fs *next_mount;
} ext4_fs_t;

int ext4_read_superblock(ext4_fs_t *fs);
int ext4_write_superblock(ext4_fs_t *fs);
void ext4_sync_inodes(ext4_fs_t *fs);
int ext4_validate_superblock(ext4_superblock_t *sb);
void ext4_print_superblock(ext4_superblock_t *sb);

int ext4_read_block(ext4_fs_t *fs, uint64_t block, void *buffer);
int ext4_write_block(ext4_fs_t *fs, uint64_t block, const void *buffer);
uint8_t *ext4_get_block(ext4_fs_t *fs, uint64_t block);
uint8_t *ext4_get_block_overwrite(ext4_fs_t *fs, uint64_t block);
void ext4_release_block(ext4_fs_t *fs, uint64_t block);
int ext4_reclaim_clean_blocks(ext4_fs_t *fs, uint32_t max_blocks);
int ext4_sync_blocks(ext4_fs_t *fs);
int ext4_sync_some_blocks(ext4_fs_t *fs, uint32_t max_blocks);
int ext4_alloc_block(ext4_fs_t *fs, uint32_t hint);
int ext4_free_block(ext4_fs_t *fs, uint64_t block);

int ext4_read_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode);
int ext4_write_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode);
ext4_inode_cache_t *ext4_get_inode(ext4_fs_t *fs, uint32_t ino);
void ext4_release_inode(ext4_inode_cache_t *ic);
int ext4_alloc_inode(ext4_fs_t *fs, uint16_t mode);
int ext4_free_inode(ext4_fs_t *fs, uint32_t ino);
uint64_t ext4_inode_get_size(ext4_inode_t *inode);
void ext4_inode_set_size(ext4_inode_t *inode, uint64_t size);
uint64_t ext4_inode_get_block(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t logical_block);

int ext4_dir_lookup(ext4_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *result_ino);
int ext4_dir_add_entry(ext4_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t file_type);
int ext4_dir_remove_entry(ext4_fs_t *fs, uint32_t dir_ino, const char *name);
int ext4_dir_iterate(ext4_fs_t *fs, uint32_t dir_ino, int (*callback)(ext4_dir_entry_t *, void *), void *ctx);
int ext4_dir_get_entry(ext4_fs_t *fs, uint32_t dir_ino, uint32_t index, ext4_dir_entry_t *entry);

uint32_t ext4_file_read(ext4_fs_t *fs, uint32_t ino, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t ext4_file_write(ext4_fs_t *fs, uint32_t ino, uint32_t offset, uint32_t size, const uint8_t *buffer);
int ext4_file_truncate(ext4_fs_t *fs, uint32_t ino, uint64_t size);

void ext4_init(void);
void ext4_vfs_register(void);
void ext4_drop_vfs_node(ext4_fs_t *fs, vfs_node_t *node);
ext4_fs_t *ext4_mount_disk(uint32_t port_index, const char *mountpoint);
int ext4_unmount(ext4_fs_t *fs);
int ext4_sync(ext4_fs_t *fs);
ext4_fs_t *ext4_get_mounted_fs(void);
void ext4_background_writeback(uint32_t max_blocks);

uint8_t ext4_type_to_vfs(uint8_t ext4_type);
uint8_t ext4_mode_to_type(uint16_t mode);
int ext4_get_stats(uint64_t *total_blocks, uint64_t *free_blocks, uint32_t *block_size);

#endif

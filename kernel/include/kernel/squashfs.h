#ifndef SQUASHFS_H
#define SQUASHFS_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/vfs.h>

#define SQUASHFS_MAGIC      0x73717368
#define SQUASHFS_MAGIC_SWAP 0x68737173

#define SQUASHFS_ZLIB  1
#define SQUASHFS_LZMA  2
#define SQUASHFS_LZO   3
#define SQUASHFS_XZ    4
#define SQUASHFS_LZ4   5
#define SQUASHFS_ZSTD  6
#define SQUASHFS_NONE  0

#define SQUASHFS_DIR_TYPE           1
#define SQUASHFS_REG_TYPE           2
#define SQUASHFS_SYMLINK_TYPE       3
#define SQUASHFS_BLKDEV_TYPE        4
#define SQUASHFS_CHRDEV_TYPE        5
#define SQUASHFS_FIFO_TYPE          6
#define SQUASHFS_SOCKET_TYPE        7
#define SQUASHFS_LDIR_TYPE          8
#define SQUASHFS_LREG_TYPE          9
#define SQUASHFS_LSYMLINK_TYPE      10

#define SQUASHFS_INVALID_FRAG       0xFFFFFFFF
#define SQUASHFS_INVALID_BLK        (-1ULL)

typedef struct {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t modification_time;
    uint32_t block_size;
    uint32_t fragment_entry_count;
    uint16_t compression_id;
    uint16_t block_log;
    uint16_t flags;
    uint16_t id_count;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t root_inode;
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t xattr_id_table_start;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t export_table_start;
} __attribute__((packed)) squashfs_super_t;

typedef struct {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
} __attribute__((packed)) squashfs_base_inode_t;

typedef struct {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t start_block;
    uint32_t nlink;
    uint16_t file_size;
    uint16_t offset;
    uint32_t parent_inode;
} __attribute__((packed)) squashfs_dir_inode_t;

typedef struct {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t start_block;
    uint32_t nlink;
    uint32_t file_size;
    uint16_t offset;
    uint32_t parent_inode;
    uint32_t xattr;
} __attribute__((packed)) squashfs_ldir_inode_t;

typedef struct {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t start_block;
    uint32_t fragment;
    uint32_t offset;
    uint32_t file_size;
} __attribute__((packed)) squashfs_reg_inode_t;

typedef struct {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
    uint64_t start_block;
    uint64_t file_size;
    uint32_t sparse;
    uint32_t nlink;
    uint32_t fragment;
    uint32_t offset;
    uint32_t xattr;
} __attribute__((packed)) squashfs_lreg_inode_t;

typedef struct {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
    uint32_t nlink;
    uint32_t symlink_size;
} __attribute__((packed)) squashfs_symlink_inode_t;

typedef struct {
    uint32_t count;
    uint32_t start_block;
    uint32_t inode_number;
} __attribute__((packed)) squashfs_dir_header_t;

typedef struct {
    uint16_t offset;
    int16_t inode_offset;
    uint16_t type;
    uint16_t name_size;
} __attribute__((packed)) squashfs_dir_entry_t;

typedef struct {
    uint64_t start_block;
    uint32_t size;
    uint32_t unused;
} __attribute__((packed)) squashfs_fragment_entry_t;

typedef struct squashfs_context {
    uint8_t *base;
    uint32_t size;
    squashfs_super_t *super;
    uint32_t block_size;
    uint16_t compression_id;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t id_table_start;
    uint32_t *id_table;
    squashfs_fragment_entry_t *fragment_table;
    uint32_t fragment_count;
} squashfs_context_t;

typedef struct squashfs_vfs_node {
    vfs_node_t vfs;
    uint64_t inode_ref;
    uint32_t start_block;
    uint32_t dir_block_offset;
    uint32_t parent_inode;
} squashfs_vfs_node_t;

void squashfs_init(uint32_t mod_start, uint32_t mod_end);
void squashfs_vfs_register(void);
squashfs_context_t *squashfs_get_context(void);
int squashfs_decompress(uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_len, uint16_t comp_id);

#endif

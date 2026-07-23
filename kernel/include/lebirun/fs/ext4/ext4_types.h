#ifndef EXT4_TYPES_H
#define EXT4_TYPES_H

#include <stdint.h>

#define EXT4_SUPER_MAGIC        0xEF53
#define EXT4_SUPERBLOCK_OFFSET  1024
#define EXT4_SUPERBLOCK_SIZE    1024

#define EXT4_NDIR_BLOCKS        12
#define EXT4_IND_BLOCK          12
#define EXT4_DIND_BLOCK         13
#define EXT4_TIND_BLOCK         14
#define EXT4_N_BLOCKS           15

#define EXT4_NAME_LEN           255

#define EXT4_ROOT_INO           2
#define EXT4_BAD_INO            1
#define EXT4_USR_QUOTA_INO      3
#define EXT4_GRP_QUOTA_INO      4
#define EXT4_BOOT_LOADER_INO    5
#define EXT4_UNDEL_DIR_INO      6
#define EXT4_RESIZE_INO         7
#define EXT4_JOURNAL_INO        8

#define EXT4_GOOD_OLD_FIRST_INO 11
#define EXT4_GOOD_OLD_INODE_SIZE 128

#define EXT4_FT_UNKNOWN         0
#define EXT4_FT_REG_FILE        1
#define EXT4_FT_DIR             2
#define EXT4_FT_CHRDEV          3
#define EXT4_FT_BLKDEV          4
#define EXT4_FT_FIFO            5
#define EXT4_FT_SOCK            6
#define EXT4_FT_SYMLINK         7
#define EXT4_FT_MAX             8

#define EXT4_S_IFSOCK           0xC000
#define EXT4_S_IFLNK            0xA000
#define EXT4_S_IFREG            0x8000
#define EXT4_S_IFBLK            0x6000
#define EXT4_S_IFDIR            0x4000
#define EXT4_S_IFCHR            0x2000
#define EXT4_S_IFIFO            0x1000

#define EXT4_S_ISUID            0x0800
#define EXT4_S_ISGID            0x0400
#define EXT4_S_ISVTX            0x0200

#define EXT4_S_IRUSR            0x0100
#define EXT4_S_IWUSR            0x0080
#define EXT4_S_IXUSR            0x0040
#define EXT4_S_IRGRP            0x0020
#define EXT4_S_IWGRP            0x0010
#define EXT4_S_IXGRP            0x0008
#define EXT4_S_IROTH            0x0004
#define EXT4_S_IWOTH            0x0002
#define EXT4_S_IXOTH            0x0001

#define EXT4_FEATURE_COMPAT_DIR_PREALLOC    0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES   0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL     0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR        0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE    0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX       0x0020
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2   0x0200

#define EXT4_FEATURE_INCOMPAT_COMPRESSION   0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE      0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER       0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV   0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG       0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS       0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT         0x0080
#define EXT4_FEATURE_INCOMPAT_MMP           0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG       0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE      0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA       0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED     0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR      0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA   0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT       0x10000

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR    0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE    0x0008

#define EXT4_VALID_FS                       0x0001
#define EXT4_ERROR_FS                       0x0002
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM     0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK    0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE  0x0040
#define EXT4_FEATURE_RO_COMPAT_QUOTA        0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC     0x0200
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#define EXT4_FEATURE_RO_COMPAT_READONLY     0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT      0x2000

#define EXT4_EXTENT_MAGIC       0xF30A

#define EXT4_EXT_MAX_DEPTH      5

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint8_t  s_first_error_func[32];
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint32_t s_last_error_line;
    uint64_t s_last_error_block;
    uint8_t  s_last_error_func[32];
    uint8_t  s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_blocks;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint32_t s_reserved[98];
    uint32_t s_checksum;
} __attribute__((packed)) ext4_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed)) ext4_group_desc_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT4_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed)) ext4_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[EXT4_NAME_LEN];
} __attribute__((packed)) ext4_dir_entry_t;

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed)) ext4_extent_header_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) ext4_extent_t;

#define EXT4_INODE_FLAG_SECRM        0x00000001
#define EXT4_INODE_FLAG_UNRM         0x00000002
#define EXT4_INODE_FLAG_COMPR        0x00000004
#define EXT4_INODE_FLAG_SYNC         0x00000008
#define EXT4_INODE_FLAG_IMMUTABLE    0x00000010
#define EXT4_INODE_FLAG_APPEND       0x00000020
#define EXT4_INODE_FLAG_NODUMP       0x00000040
#define EXT4_INODE_FLAG_NOATIME      0x00000080
#define EXT4_INODE_FLAG_DIRTY        0x00000100
#define EXT4_INODE_FLAG_COMPRBLK     0x00000200
#define EXT4_INODE_FLAG_NOCOMPR      0x00000400
#define EXT4_INODE_FLAG_ENCRYPT      0x00000800
#define EXT4_INODE_FLAG_INDEX        0x00001000
#define EXT4_INODE_FLAG_IMAGIC       0x00002000
#define EXT4_INODE_FLAG_JOURNAL_DATA 0x00004000
#define EXT4_INODE_FLAG_NOTAIL       0x00008000
#define EXT4_INODE_FLAG_DIRSYNC      0x00010000
#define EXT4_INODE_FLAG_TOPDIR       0x00020000
#define EXT4_INODE_FLAG_HUGE_FILE    0x00040000
#define EXT4_INODE_FLAG_EXTENTS      0x00080000
#define EXT4_INODE_FLAG_EA_INODE     0x00200000
#define EXT4_INODE_FLAG_INLINE_DATA  0x10000000

#endif

#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/vfs.h>
#include <kernel/mutex.h>

#define RAMFS_MAX_TOTAL_SIZE   (2 * 1024 * 1024)
#define RAMFS_MAX_FILE_SIZE    RAMFS_MAX_TOTAL_SIZE
#define RAMFS_BLOCK_SIZE       4096
#define RAMFS_MAX_NAME_LEN     VFS_MAX_NAME

#define RAMFS_ERR_OK           0
#define RAMFS_ERR_NOMEM        -1
#define RAMFS_ERR_NOENT        -2
#define RAMFS_ERR_EXIST        -3
#define RAMFS_ERR_NOTDIR       -4
#define RAMFS_ERR_ISDIR        -5
#define RAMFS_ERR_NOTEMPTY     -6
#define RAMFS_ERR_NOSPC        -7
#define RAMFS_ERR_INVAL        -8
#define RAMFS_ERR_NAMETOOLONG  -9
#define RAMFS_ERR_PERM         -10
#define RAMFS_ERR_BUSY         -11

typedef struct ramfs_node {
    char name[RAMFS_MAX_NAME_LEN];
    uint8_t type;
    uint8_t permissions;
    uint32_t uid;
    uint32_t gid;
    uint32_t length;
    uint8_t *data;
    uint32_t data_capacity;
    const uint8_t *backing_data;
    uint32_t backing_length;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    mutex_t lock;
    struct ramfs_node *parent;
    struct ramfs_node *children;
    struct ramfs_node *next_sibling;
    vfs_node_t *vfs_node;
} ramfs_node_t;

typedef struct {
    uint32_t total_size;
    uint32_t used_size;
    uint32_t file_count;
    uint32_t dir_count;
    mutex_t global_lock;
} ramfs_stats_t;

void ramfs_init(void);
void ramfs_vfs_register(void);

int ramfs_create_file(const char *path, uint8_t permissions);
int ramfs_create_dir(const char *path, uint8_t permissions);
int ramfs_create_symlink(const char *path, const char *target, uint8_t permissions);
int ramfs_unlink(const char *path);
int ramfs_write(const char *path, uint32_t offset, const uint8_t *data, uint32_t size);
int ramfs_read(const char *path, uint32_t offset, uint8_t *buffer, uint32_t size);
int ramfs_stat(const char *path, uint32_t *size, uint8_t *type, uint8_t *perms);
int ramfs_truncate(const char *path, uint32_t length);
int ramfs_rename(const char *old_path, const char *new_path);
int ramfs_chmod(const char *path, uint32_t mode);
int ramfs_chown(const char *path, uint32_t uid, uint32_t gid);
int ramfs_set_backing(const char *path, const uint8_t *data, uint32_t length);

ramfs_node_t *ramfs_get_root(void);
ramfs_node_t *ramfs_find_node(const char *path);
int ramfs_get_stats(ramfs_stats_t *stats);

uint32_t ramfs_get_time(void);

void ramfs_debug_check_root(const char *location);

#endif

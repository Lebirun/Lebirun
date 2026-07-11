#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/vfs.h>
#include <lebirun/mutex.h>

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
    char *name;
    uint8_t type;
    uint16_t permissions;
    uint64_t uid;
    uint64_t gid;
    uint64_t length;
    uint8_t *data;
    uint64_t data_capacity;
    const uint8_t *backing_data;
    uint64_t backing_length;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    mutex_t lock;
    struct ramfs_node *parent;
    struct ramfs_node *children;
    struct ramfs_node *next_sibling;
    vfs_node_t *vfs_node;
} ramfs_node_t;

typedef struct {
    uint64_t total_size;
    uint64_t used_size;
    uint64_t file_count;
    uint64_t dir_count;
    mutex_t global_lock;
} ramfs_stats_t;

void ramfs_init(void);
void ramfs_vfs_register(void);
void tmpfs_vfs_register(void);

int ramfs_create_file(const char *path, uint16_t permissions);
int ramfs_create_dir(const char *path, uint16_t permissions);
int ramfs_create_symlink(const char *path, const char *target, uint16_t permissions);
int ramfs_unlink(const char *path);
int ramfs_write(const char *path, uint64_t offset, const uint8_t *data, uint64_t size);
int ramfs_read(const char *path, uint64_t offset, uint8_t *buffer, uint64_t size);
int ramfs_stat(const char *path, uint64_t *size, uint8_t *type, uint8_t *perms);
int ramfs_truncate(const char *path, uint64_t length);
int ramfs_rename(const char *old_path, const char *new_path);
int ramfs_chmod(const char *path, uint64_t mode);
int ramfs_chown(const char *path, uint64_t uid, uint64_t gid);
int ramfs_set_backing(const char *path, const uint8_t *data, uint64_t length);

ramfs_node_t *ramfs_get_root(void);
ramfs_node_t *ramfs_find_node(const char *path);
int ramfs_get_stats(ramfs_stats_t *stats);

uint64_t ramfs_get_time(void);

void ramfs_debug_check_root(const char *location);
void ramfs_internalize_all(void);

#endif

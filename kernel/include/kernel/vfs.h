#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04
#define VFS_PIPE        0x05
#define VFS_SYMLINK     0x06
#define VFS_MOUNTPOINT  0x08

#define VFS_TYPE_MASK   0x07
#define VFS_GET_TYPE(flags) ((flags) & VFS_TYPE_MASK)

#define VFS_O_RDONLY    0x0000
#define VFS_O_WRONLY    0x0001
#define VFS_O_RDWR      0x0002
#define VFS_O_CREAT     0x0040
#define VFS_O_EXCL      0x0080
#define VFS_O_TRUNC     0x0200
#define VFS_O_APPEND    0x0400
#define VFS_O_NONBLOCK  0x0800
#define VFS_O_DIRECTORY 0x10000
#define VFS_O_CLOEXEC   0x80000

#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

#define VFS_PERM_READ   0x04
#define VFS_PERM_WRITE  0x02
#define VFS_PERM_EXEC   0x01

#define VFS_MAX_PATH    256
#define VFS_MAX_NAME    64
#define VFS_MAX_FDS     64
#define VFS_MAX_MOUNTS  16

struct vfs_node;
struct dirent;

typedef struct dirent {
    char name[VFS_MAX_NAME];
    uint32_t inode;
    uint8_t type;
} dirent_t;

typedef uint32_t (*read_type_t)(struct vfs_node *, uint32_t offset, uint32_t size, uint8_t *buffer);
typedef uint32_t (*write_type_t)(struct vfs_node *, uint32_t offset, uint32_t size, uint8_t *buffer);
typedef void (*open_type_t)(struct vfs_node *, uint32_t flags);
typedef void (*close_type_t)(struct vfs_node *);
typedef struct dirent *(*readdir_type_t)(struct vfs_node *, uint32_t index);
typedef struct vfs_node *(*finddir_type_t)(struct vfs_node *, const char *name);
typedef int (*create_type_t)(struct vfs_node *parent, const char *name, uint32_t flags);
typedef int (*unlink_type_t)(struct vfs_node *parent, const char *name);
typedef int (*mkdir_type_t)(struct vfs_node *parent, const char *name, uint32_t perms);
typedef int (*truncate_type_t)(struct vfs_node *, uint32_t length);
typedef int (*rename_type_t)(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name);
typedef int (*chmod_type_t)(struct vfs_node *, uint32_t mode);
typedef int (*chown_type_t)(struct vfs_node *, uint32_t uid, uint32_t gid);

typedef struct vfs_node {
    char name[VFS_MAX_NAME];
    uint32_t mask;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
    uint32_t inode;
    uint32_t length;
    uint32_t impl;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    
    read_type_t read;
    write_type_t write;
    open_type_t open;
    close_type_t close;
    readdir_type_t readdir;
    finddir_type_t finddir;
    create_type_t create;
    unlink_type_t unlink;
    mkdir_type_t mkdir;
    truncate_type_t truncate;
    rename_type_t rename;
    chmod_type_t chmod;
    chown_type_t chown;
    
    struct vfs_node *ptr;
    struct vfs_node *parent;
    uint32_t ref_count;
    void *private_data;
} vfs_node_t;

typedef struct {
    vfs_node_t *node;
    uint32_t offset;
    uint32_t flags;
    int in_use;
} vfs_fd_t;

typedef struct vfs_fs_type {
    const char *name;
    vfs_node_t *(*mount)(const char *device, const char *mountpoint);
    int (*unmount)(vfs_node_t *mountpoint);
    struct vfs_fs_type *next;
} vfs_fs_type_t;

typedef struct {
    char path[VFS_MAX_PATH];
    char device[VFS_MAX_PATH];
    vfs_node_t *root;
    vfs_fs_type_t *fs_type;
    int in_use;
} vfs_mount_t;

void vfs_init(void);

int vfs_register_fs(vfs_fs_type_t *fs);
int vfs_unregister_fs(const char *name);
vfs_fs_type_t *vfs_find_fs(const char *name);

int vfs_mount(const char *device, const char *mountpoint, const char *fs_type);
int vfs_unmount(const char *mountpoint);

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
void vfs_open(vfs_node_t *node, uint32_t flags);
void vfs_close(vfs_node_t *node);

dirent_t *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);
int vfs_create(vfs_node_t *parent, const char *name, uint32_t flags);
int vfs_unlink(vfs_node_t *parent, const char *name);
int vfs_mkdir(vfs_node_t *parent, const char *name, uint32_t perms);

vfs_node_t *vfs_namei(const char *path);
vfs_node_t *vfs_namei_nofollow(const char *path);
vfs_node_t *vfs_lookup(const char *path);
char *vfs_get_path(vfs_node_t *node, char *buf, size_t size);

int vfs_open_path(const char *path, int flags);
int vfs_close_fd(int fd);
int vfs_read_fd(int fd, void *buffer, uint32_t size);
int vfs_write_fd(int fd, const void *buffer, uint32_t size);
int vfs_seek(int fd, int32_t offset, int whence);
int vfs_tell(int fd);
int vfs_stat_fd(int fd, uint32_t *size, uint32_t *flags);
int vfs_readdir_fd(int fd, dirent_t *entry, uint32_t index);

vfs_node_t *vfs_get_root(void);
int vfs_get_mount_count(void);
vfs_mount_t *vfs_get_mount(int index);
void vfs_list_mounts(void);

#endif

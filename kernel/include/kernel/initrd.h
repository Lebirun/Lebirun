#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>
#include <stddef.h>

#define INITRD_MAGIC 0x4452544E
#define INITRD_VERSION 2

#define INITRD_TYPE_FILE 0
#define INITRD_TYPE_DIR  1

#define INITRD_PERM_READ  0x04
#define INITRD_PERM_WRITE 0x02
#define INITRD_PERM_EXEC  0x01

#define INITRD_MAX_FDS 64
#define INITRD_MAX_PATH 256

typedef struct {
    uint64_t magic;
    uint64_t version;
    uint64_t num_entries;
    uint64_t reserved;
} __attribute__((packed)) initrd_header_t;

typedef struct {
    char name[64];
    uint64_t offset;
    uint64_t length;
    uint8_t type;
    uint8_t permissions;
    uint16_t parent_index;
    uint64_t uid;
    uint64_t gid;
    uint64_t reserved;
} __attribute__((packed)) initrd_file_header_t;

typedef struct {
    char name[64];
    uint64_t length;
    uint64_t offset;
    uint8_t *data;
    uint8_t type;
    uint8_t permissions;
    uint16_t parent_index;
    uint64_t uid;
    uint64_t gid;
} initrd_file_t;

typedef struct {
    int in_use;
    uint64_t file_index;
    uint64_t offset;
    int flags;
} initrd_fd_t;

typedef struct {
    uint64_t mod_start;
    uint64_t mod_end;
    uint64_t cmdline;
    uint64_t reserved;
} __attribute__((packed)) multiboot_module_t;

void initrd_init(uint64_t mods_count, uint64_t mods_addr);
uint64_t initrd_get_file_count(void);
initrd_file_t *initrd_get_file(uint64_t index);
initrd_file_t *initrd_find_file(const char *name);
initrd_file_t *initrd_find_path(const char *path);
void initrd_list_files(void);

int initrd_open(const char *path, int flags);
int initrd_read(int fd, void *buf, uint64_t count);
int initrd_close(int fd);
int initrd_stat(const char *path, uint64_t *size, uint8_t *type, uint8_t *perms);

void initrd_init_fds(void);

void initrd_vfs_register(void);
struct vfs_node *initrd_get_vfs_root(void);

uint8_t *initrd_get_base(void);
uint64_t initrd_get_size(void);

void initrd_copy_to_root(void);

void rootfs_init(uint64_t mods_count, uint64_t mods_addr);

void initrd_free_pages(void);

#endif

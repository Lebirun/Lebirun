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

#define INITRD_MAX_FDS 16
#define INITRD_MAX_PATH 256

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_entries;
    uint32_t reserved;
} __attribute__((packed)) initrd_header_t;

typedef struct {
    char name[64];
    uint32_t offset;
    uint32_t length;
    uint8_t type;
    uint8_t permissions;
    uint16_t parent_index;
    uint32_t uid;
    uint32_t gid;
    uint32_t reserved;
} __attribute__((packed)) initrd_file_header_t;

typedef struct {
    char name[64];
    uint32_t length;
    uint32_t offset;
    uint8_t *data;
    uint8_t type;
    uint8_t permissions;
    uint16_t parent_index;
    uint32_t uid;
    uint32_t gid;
} initrd_file_t;

typedef struct {
    int in_use;
    uint32_t file_index;
    uint32_t offset;
    int flags;
} initrd_fd_t;

typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

void initrd_init(uint32_t mods_count, uint32_t mods_addr);
uint32_t initrd_get_file_count(void);
initrd_file_t *initrd_get_file(uint32_t index);
initrd_file_t *initrd_find_file(const char *name);
initrd_file_t *initrd_find_path(const char *path);
void initrd_list_files(void);

int initrd_open(const char *path, int flags);
int initrd_read(int fd, void *buf, uint32_t count);
int initrd_close(int fd);
int initrd_stat(const char *path, uint32_t *size, uint8_t *type, uint8_t *perms);

void initrd_init_fds(void);

#endif

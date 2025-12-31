#include "syscall_defs.h"

static int sys_initrd_count(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    return (int)initrd_get_file_count();
}

static int sys_initrd_stat(int index, const char *name_buf, int len_ptr) {
    initrd_file_t *f = initrd_get_file((uint32_t)index);
    if (!f) return -ENOENT;
    
    uint32_t name_addr = (uint32_t)name_buf;
    uint32_t len_addr = (uint32_t)len_ptr;
    
    if (name_addr && name_addr < 0xC0000000 && name_addr >= 0x1000) {
        int copylen = 63;
        int i = 0;
        for (; i < copylen && f->name[i]; i++) {
            ((char*)name_addr)[i] = f->name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }
    
    if (len_addr && len_addr < 0xC0000000 && len_addr >= 0x1000) {
        *(uint32_t*)len_addr = f->length;
    }
    
    return 0;
}

static int sys_initrd_read(int index, const char *buf, int maxlen) {
    initrd_file_t *f = initrd_get_file((uint32_t)index);
    if (!f) return -ENOENT;
    
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -EFAULT;
    
    uint32_t to_copy = (uint32_t)maxlen;
    if (to_copy > f->length) to_copy = f->length;
    
    memcpy((void*)buf_addr, f->data, to_copy);
    return (int)to_copy;
}

static int sys_open(int path_ptr, const char *flags_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -EFAULT;
    
    const char *path = (const char *)path_addr;
    int flags = (int)(uintptr_t)flags_ptr;
    
    return initrd_open(path, flags);
}

static int sys_close(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    return initrd_close(fd);
}

static int sys_fstat(int fd, const char *size_ptr, int type_ptr) {
    uint32_t size_addr = (uint32_t)size_ptr;
    uint32_t type_addr = (uint32_t)type_ptr;
    
    if (fd < 0 || fd >= INITRD_MAX_FDS) return -EBADF;
    
    extern initrd_fd_t fd_table[];
    if (!fd_table[fd].in_use) return -EBADF;
    
    initrd_file_t *f = initrd_get_file(fd_table[fd].file_index);
    if (!f) return -ENOENT;
    
    if (size_addr && size_addr < 0xC0000000 && size_addr >= 0x1000) {
        *(uint32_t *)size_addr = f->length;
    }
    if (type_addr && type_addr < 0xC0000000 && type_addr >= 0x1000) {
        *(uint8_t *)type_addr = f->type;
    }
    
    return 0;
}

void syscalls_initrd_init(void) {
    syscall_table[SYSCALL_INITRD_COUNT] = sys_initrd_count;
    syscall_table[SYSCALL_INITRD_STAT] = sys_initrd_stat;
    syscall_table[SYSCALL_INITRD_READ] = sys_initrd_read;
    syscall_table[SYSCALL_OPEN] = sys_open;
    syscall_table[SYSCALL_CLOSE] = sys_close;
    syscall_table[SYSCALL_FSTAT] = sys_fstat;
}

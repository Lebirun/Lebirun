#include "syscall_defs.h"

static int sys_initrd_count(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    return (int)initrd_get_file_count();
}

static int sys_initrd_stat(int index, const char *name_buf, int len_ptr) {
    uint64_t name_addr = (uint64_t)name_buf;
    uint64_t len_addr = (uint64_t)len_ptr;
    initrd_file_t *f;
    int copylen;
    int i;

    f = initrd_get_file((uint64_t)index);
    if (!f) return -ENOENT;
    
    if (name_addr && name_addr < KERNEL_VMA && name_addr >= 0x1000) {
        copylen = 63;
        i = 0;
        for (; i < copylen && f->name[i]; i++) {
            ((char*)name_addr)[i] = f->name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }
    
    if (len_addr && len_addr < KERNEL_VMA && len_addr >= 0x1000) {
        *(uint64_t*)len_addr = f->length;
    }
    
    return 0;
}

static int sys_initrd_read(int index, const char *buf, int maxlen) {
    uint64_t buf_addr = (uint64_t)buf;
    uint64_t to_copy;
    initrd_file_t *f;

    f = initrd_get_file((uint64_t)index);
    if (!f) return -ENOENT;

    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    
    to_copy = (uint64_t)maxlen;
    if (to_copy > f->length) to_copy = f->length;
    
    memcpy((void*)buf_addr, f->data, to_copy);
    return (int)to_copy;
}

static int sys_open(int path_ptr, const char *flags_ptr, int unused) {
    uint64_t path_addr = (uint64_t)path_ptr;
    const char *path = (const char *)path_addr;
    int flags = (int)(uintptr_t)flags_ptr;

    (void)unused;
    if (path_addr >= KERNEL_VMA || path_addr < 0x1000) return -EFAULT;
    
    return initrd_open(path, flags);
}

static int sys_close(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    return initrd_close(fd);
}

static int sys_fstat(int fd, const char *size_ptr, int type_ptr) {
    uint64_t size_addr = (uint64_t)size_ptr;
    uint64_t type_addr = (uint64_t)type_ptr;
    uint64_t size;
    uint8_t type;

    if (initrd_fstat_fd(fd, &size, &type) < 0) return -EBADF;
    
    if (size_addr && size_addr < KERNEL_VMA && size_addr >= 0x1000) {
        *(uint64_t *)size_addr = size;
    }
    if (type_addr && type_addr < KERNEL_VMA && type_addr >= 0x1000) {
        *(uint8_t *)type_addr = type;
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

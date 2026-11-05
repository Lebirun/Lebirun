#include "syscall_defs.h"

static int sys_initrd_count(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    return (int)initrd_get_file_count();
}

static int sys_initrd_stat(int index, const char *name_buf, uint64_t len_ptr) {
    initrd_file_t *f;
    char name[64];
    int i;

    f = initrd_get_file((uint64_t)index);
    if (!f) return -ENOENT;
    
    if (name_buf) {
        for (i = 0; i < 63 && f->name[i]; i++) name[i] = f->name[i];
        name[i] = '\0';
        if (copy_to_user((void *)name_buf, name, (size_t)i + 1) < 0)
            return -EFAULT;
    }
    
    if (len_ptr && copy_to_user((void *)(uintptr_t)len_ptr, &f->length,
                                sizeof(uint64_t)) < 0)
        return -EFAULT;
    
    return 0;
}

static int sys_initrd_read(int index, const char *buf, int maxlen) {
    uint64_t to_copy;
    initrd_file_t *f;

    f = initrd_get_file((uint64_t)index);
    if (!f) return -ENOENT;

    to_copy = (uint64_t)maxlen;
    if (to_copy > f->length) to_copy = f->length;
    
    if (copy_to_user((void *)buf, f->data, to_copy) < 0) return -EFAULT;
    return (int)to_copy;
}

void syscalls_initrd_init(void) {
    syscall_table_set(SYSCALL_INITRD_COUNT, (void *)(sys_initrd_count));
    syscall_table_set(SYSCALL_INITRD_STAT, (void *)(sys_initrd_stat));
    syscall_table_set(SYSCALL_INITRD_READ, (void *)(sys_initrd_read));
}

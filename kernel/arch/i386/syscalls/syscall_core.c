#include "syscall_defs.h"

extern mutex_t print_lock;

static int sys_exit(int code, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    asm volatile("cli");
    printf("sys_exit: user task exiting with code %d\n", code);
    asm volatile("sti");
    task_exit((uint32_t)code);
    return 0;
}

static int sys_write(int fd, const char *buf, int len) {
    if ((fd != 1 && fd != 2) || !buf || len < 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len >= 0xC0000000) return -1;

    framebuffer_t *fb = fb_get();
    int con_id = current_task ? current_task->console_id : 0;
    
    int written = 0;
    while (written < len) {
        int chunk = len - written;
        if (chunk > 64) chunk = 64;
        
        asm volatile("cli");
        if (fb && fb->font && console_is_initialized()) {
            console_write_to(con_id, (const char *)(buf_addr + written), (size_t)chunk);
        } else {
            for (int i = 0; i < chunk; i++) {
                terminal_putchar(((const char *)buf_addr)[written + i]);
            }
        }
        asm volatile("sti");
        
        written += chunk;
    }
    return len;
}

static int sys_read(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (fd == 0) {
        int con_id = current_task ? current_task->console_id : 0;
        
        while (!keyboard_has_data_for(con_id)) {
            wait_queue_t *wq = keyboard_get_waitq_for(con_id);
            if (!wq) return -1;
            waitq_add(wq, current_task);
            block_current();
        }
        clear_syscall_frame();

        int key = keyboard_getchar_nb_for(con_id);
        if (key >= 0) {
            char c = (char)key;
            memcpy((void*)buf_addr, &c, 1);
            return 1;
        }
        return 0;
    }
    
    return initrd_read(fd, (void *)buf_addr, (uint32_t)len);
}

static int sys_read_nb(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (fd == 0) {
        int con_id = current_task ? current_task->console_id : 0;
        
        if (!keyboard_has_data_for(con_id)) {
            return 0;
        }

        int key = keyboard_getchar_nb_for(con_id);
        if (key >= 0) {
            char c = (char)key;
            memcpy((void*)buf_addr, &c, 1);
            return 1;
        }
        return 0;
    }
    
    return -1;
}

static int sys_isatty(int fd, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
}

struct iovec {
    void *iov_base;
    uint32_t iov_len;
};

static int sys_writev(int fd, const char *iov_ptr, int iovcnt) {
    if ((fd != 1 && fd != 2) || !iov_ptr || iovcnt <= 0) return -1;
    
    uint32_t iov_addr = (uint32_t)iov_ptr;
    if (iov_addr >= 0xC0000000 || iov_addr < 0x1000) return -1;
    
    struct iovec *iov = (struct iovec *)iov_addr;
    int total = 0;
    
    for (int i = 0; i < iovcnt; i++) {
        uint32_t base = (uint32_t)iov[i].iov_base;
        uint32_t len = iov[i].iov_len;
        
        if (len == 0) continue;
        if (base >= 0xC0000000 || base < 0x1000) continue;
        if (base + len >= 0xC0000000) continue;
        
        int written = sys_write(fd, (const char *)base, (int)len);
        if (written > 0) total += written;
    }
    
    return total;
}

static int sys_lseek(int fd, const char *offset_ptr, int whence) {
    (void)fd; (void)offset_ptr; (void)whence;
    return 0;
}

void syscalls_core_init(void) {
    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_WRITE] = sys_write;
    syscall_table[SYSCALL_READ] = sys_read;
    syscall_table[SYSCALL_READ_NB] = sys_read_nb;
    syscall_table[SYSCALL_ISATTY] = sys_isatty;
    syscall_table[SYSCALL_WRITEV] = sys_writev;
    syscall_table[SYSCALL_LSEEK] = sys_lseek;
}

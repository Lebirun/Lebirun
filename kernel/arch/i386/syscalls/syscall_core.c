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
    int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
    
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
        int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        
        while (!keyboard_has_data_for(con_id)) {
            wait_queue_t *wq = keyboard_get_waitq_for(con_id);
            if (!wq) return -1;
            waitq_add(wq, current_task);
            block_current();
            con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
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
        int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        
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

void syscalls_core_init(void) {
    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_WRITE] = sys_write;
    syscall_table[SYSCALL_READ] = sys_read;
    syscall_table[SYSCALL_READ_NB] = sys_read_nb;
    syscall_table[SYSCALL_ISATTY] = sys_isatty;
}

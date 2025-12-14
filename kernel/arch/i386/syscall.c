#include <kernel/registers.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>
#include <kernel/keyboard.h>
#include <kernel/mutex.h>
#include <kernel/task.h>

extern mutex_t print_lock;

#define SYSCALL_EXIT 0
#define NR_SYSCALLS 4
#define SYSCALL_READ 3

static void *syscall_table[NR_SYSCALLS] = {0};

static int sys_exit(int code, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    mutex_lock(&print_lock);
    printf("sys_exit: user task exiting with code %d\n", code);
    mutex_unlock(&print_lock);
    task_exit((uint32_t)code);
    return 0;
}

static int sys_write(int fd, const char *buf, int len) {
    if (fd != 1 || !buf || len < 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    mutex_lock(&print_lock);
    for (int i = 0; i < len; i++) {
        terminal_putchar(buf[i]);
    }
    mutex_unlock(&print_lock);
    return len;
}

static int sys_read(int fd, char *buf, int len) {
    if (fd != 0 || !buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    while (!keyboard_has_data()) {
        wait_queue_t *wq = keyboard_get_waitq();
        waitq_add(wq, current_task);
        block_current();
    }
    clear_syscall_frame();

    int key = keyboard_getchar_nb();
    if (key >= 0) {
        char c = (char)key;
        memcpy((void*)buf_addr, &c, 1);
        return 1;
    }
    return 0;
}

void do_syscall(registers_t *regs) {
    int num = regs->eax;

    set_syscall_frame(regs);

    if (num < 0 || num >= NR_SYSCALLS || !syscall_table[num]) {
        clear_syscall_frame();
        regs->eax = -38;
        return;
    }

    regs->eax = ((int (*)(int, const char *, int))syscall_table[num])(
        regs->ebx, (const char *)regs->ecx, regs->edx);
    
    clear_syscall_frame();
}

void syscall_init(void) {
    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_WRITE] = sys_write;
    syscall_table[SYSCALL_READ] = sys_read;
}
#include "syscall_defs.h"

static int sys_console_switch(int console_num, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (console_num < 0 || console_num >= cmdline_get_consoles()) return -EINVAL;
    console_switch(console_num);
    return 0;
}

static int sys_console_getcur(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return console_get_current();
}

static int sys_console_clear(int console_num, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (console_num < 0 || console_num >= cmdline_get_consoles()) return -EINVAL;
    console_clear(console_num);
    return 0;
}

static int sys_console_setcursor(int x, const char *y_ptr, int unused) {
    int y;
    int con_id;

    (void)unused;
    y = (int)(uintptr_t)y_ptr;
    con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
    console_setcursor(con_id, x, y);
    return 0;
}

static int sys_console_setid(int console_num, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (console_num < 0 || console_num >= cmdline_get_consoles()) return -EINVAL;
    if (!current_task) return -EINVAL;
    if (console_alloc(console_num) < 0) return -ENOMEM;
    current_task->console_id = console_num;
    return 0;
}

void syscalls_console_init(void) {
    syscall_table[SYSCALL_CONSOLE_SWITCH] = sys_console_switch;
    syscall_table[SYSCALL_CONSOLE_GETCUR] = sys_console_getcur;
    syscall_table[SYSCALL_CONSOLE_CLEAR] = sys_console_clear;
    syscall_table[SYSCALL_CONSOLE_SETCURSOR] = sys_console_setcursor;
    syscall_table[SYSCALL_CONSOLE_SETID] = sys_console_setid;
}

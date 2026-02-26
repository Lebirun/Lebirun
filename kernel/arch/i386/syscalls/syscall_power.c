#include "syscall_defs.h"
#include <kernel/power.h>

static int sys_reboot(int cmd, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;

    if (cmd == POWER_CMD_SHUTDOWN) {
        power_shutdown();
    } else if (cmd == POWER_CMD_REBOOT) {
        power_reboot();
    }

    return -EINVAL;
}

void syscalls_power_init(void) {
    syscall_table[SYSCALL_REBOOT] = sys_reboot;
}

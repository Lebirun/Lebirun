#include "syscall_defs.h"
#include <lebirun/power.h>
#include <lebirun/panic.h>
#include <lebirun/watchdog.h>
#include <lebirun/creds.h>
#include <string.h>

#define PANIC_CMD_CUSTOM  0
#define PANIC_CMD_OOM     1
#define PANIC_CMD_ASSERT  2
#define PANIC_CMD_GENERIC 3

static int sys_reboot(int cmd, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;

    if (!current_task || !creds_has_capability(current_task, 22))
        return -EPERM;
    watchdog_disable();
    if (cmd == POWER_CMD_SHUTDOWN) {
        power_shutdown();
    } else if (cmd == POWER_CMD_REBOOT) {
        power_reboot();
    }

    return -EINVAL;
}

static int sys_panic(int cmd, const char *user_msg, int unused) {
    char safe_msg[128];
    char *copy;

    (void)unused;

    if (!current_task || !creds_has_capability(current_task, 22))
        return -EPERM;
    safe_msg[0] = '\0';
    if (user_msg && (copy = copy_string_from_user_alloc(user_msg))) {
        strncpy(safe_msg, copy, sizeof(safe_msg) - 1);
        safe_msg[sizeof(safe_msg) - 1] = '\0';
        kfree(copy);
    }

    switch (cmd) {
    case PANIC_CMD_CUSTOM:
        kernel_panic_custom("USER PANIC", "%s", safe_msg[0] ? safe_msg : "triggered by user");
        break;
    case PANIC_CMD_OOM:
        kernel_panic_custom("OUT OF MEMORY", "user-triggered OOM panic");
        break;
    case PANIC_CMD_ASSERT:
        kernel_panic_custom("ASSERT", "user-triggered assert panic");
        break;
    case PANIC_CMD_GENERIC:
    default:
        kernel_panic_msg("User-triggered kernel panic");
        break;
    }

    return 0;
}

void syscalls_power_init(void) {
    syscall_table_set(SYSCALL_REBOOT, (void *)(sys_reboot));
    syscall_table_set(SYSCALL_PANIC, (void *)(sys_panic));
}

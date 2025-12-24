#include "syscall_defs.h"

static uint32_t sys_boot_time = 1735000000;

static int sys_getticks(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint32_t tick_count;
    return (int)tick_count;
}

static int sys_time(int tloc_ptr, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    extern volatile uint32_t tick_count;
    extern uint32_t pit_freq;
    if (pit_freq == 0) return 0;
    uint32_t secs = sys_boot_time + (tick_count / pit_freq);
    if (tloc_ptr && (uint32_t)tloc_ptr < 0xC0000000 && (uint32_t)tloc_ptr >= 0x1000) {
        *(uint32_t *)tloc_ptr = secs;
    }
    return (int)secs;
}

void syscalls_time_init(void) {
    syscall_table[SYSCALL_GETTICKS] = sys_getticks;
    syscall_table[SYSCALL_TIME] = sys_time;
}

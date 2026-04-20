#include "syscall_defs.h"
#include <lebirun/rtc.h>

static uint64_t sys_boot_time = 0;
static uint64_t boot_tick_count = 0;

static int sys_getticks(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint64_t tick_count;
    return (int)tick_count;
}

static int sys_time(int tloc_ptr, const char *unused2, int unused3) {
    extern volatile uint64_t tick_count;
    extern uint64_t pit_freq;
    uint64_t secs;
    uint64_t elapsed_ticks;
    
    (void)unused2; (void)unused3;
    if (sys_boot_time == 0) {
        sys_boot_time = rtc_get_time();
        boot_tick_count = tick_count;
    }
    if (pit_freq == 0) return 0;
    elapsed_ticks = tick_count - boot_tick_count;
    secs = sys_boot_time + (elapsed_ticks / pit_freq);
    if (tloc_ptr && (uintptr_t)tloc_ptr < KERNEL_VMA && (uintptr_t)tloc_ptr >= 0x1000) {
        *(uint64_t *)(uintptr_t)tloc_ptr = secs;
    }
    return (int)secs;
}

void syscalls_time_init(void) {
    syscall_table[SYSCALL_GETTICKS] = sys_getticks;
    syscall_table[SYSCALL_TIME] = sys_time;
}

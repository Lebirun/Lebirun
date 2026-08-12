#include "syscall_defs.h"
#include <lebirun/rtc.h>
#include <lebirun/timekeeping.h>

static int sys_getticks(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint64_t tick_count;
    return (int)tick_count;
}

static int64_t sys_time(uint64_t tloc_ptr, const char *unused2, int unused3) {
    uint64_t nanoseconds;
    uint64_t secs;
    uint64_t *tloc;
    
    (void)unused2; (void)unused3;
    nanoseconds = timekeeping_realtime_ns();
    secs = nanoseconds / 1000000000ULL;
    tloc = (uint64_t *)(uintptr_t)tloc_ptr;
    if (tloc && copy_to_user(tloc, &secs, sizeof(secs)) != 0) {
        return -EFAULT;
    }
    return (int64_t)secs;
}

void syscalls_time_init(void) {
    syscall_table_set(SYSCALL_GETTICKS, (void *)(sys_getticks));
    syscall_table_set(SYSCALL_TIME, (void *)(sys_time));
}

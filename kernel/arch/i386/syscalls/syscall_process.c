#include "syscall_defs.h"

static int sys_getpid(int unused, const char *unused2, int unused3) {
    (void)unused;
    (void)unused2;
    (void)unused3;
    return (int)getpid();
}

static int sys_yield(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    schedule();
    return 0;
}

static int sys_sleep(int ms, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (ms <= 0) return -1;
    sleep_ms((uint32_t)ms);
    return 0;
}

static int sys_waitpid(int pid, const char *status_ptr, int unused) {
    (void)unused;
    if (pid <= 0) return -1;
    task_t* t = task_find((pid_t)pid);
    if (!t) return -1;
    uint32_t exit_code = 0;
    int r = task_join(t, &exit_code);
    if (r != 0) return -1;

    if (status_ptr) {
        uint32_t addr = (uint32_t)status_ptr;
        if (addr >= 0xC0000000 || addr < 0x1000) return -1;
        if (addr + sizeof(uint32_t) >= 0xC0000000) return -1;
        memcpy((void*)addr, &exit_code, sizeof(uint32_t));
    }
    return (int)pid;
}

static int sys_kill(int pid, const char *unused, int code) {
    (void)unused;
    task_t* t = task_find((pid_t)pid);
    if (!t) return -1;
    task_kill(t, (uint32_t)code);
    return 0;
}

static int sys_fork(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    if (!fork_regs_ptr) {
        printf("sys_fork: no registers pointer\n");
        return -1;
    }
    return (int)task_fork(fork_regs_ptr);
}

static int sys_exec(int bin_ptr, const char *size_ptr, int unused) {
    (void)unused;
    uint32_t bin_addr = (uint32_t)bin_ptr;
    uint32_t bin_size = (uint32_t)(uintptr_t)size_ptr;

    if (bin_addr >= 0xC0000000 || bin_addr < 0x1000) {
        printf("sys_exec: invalid binary pointer 0x%08X\n", bin_addr);
        return -1;
    }
    if (bin_size == 0 || bin_size > 0x100000) {
        printf("sys_exec: invalid size %u\n", bin_size);
        return -1;
    }

    if (!fork_regs_ptr) {
        printf("sys_exec: no registers pointer\n");
        return -1;
    }

    return task_exec((const uint8_t *)bin_addr, bin_size, fork_regs_ptr);
}

void syscalls_process_init(void) {
    syscall_table[SYSCALL_GETPID] = sys_getpid;
    syscall_table[SYSCALL_YIELD] = sys_yield;
    syscall_table[SYSCALL_SLEEP] = sys_sleep;
    syscall_table[SYSCALL_WAITPID] = sys_waitpid;
    syscall_table[SYSCALL_KILL] = sys_kill;
    syscall_table[SYSCALL_FORK] = sys_fork;
    syscall_table[SYSCALL_EXEC] = sys_exec;
}

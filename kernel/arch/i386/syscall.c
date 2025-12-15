#include <kernel/registers.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>
#include <kernel/keyboard.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/mem_map.h>

extern mutex_t print_lock;

#define SYSCALL_EXIT 0
#define SYSCALL_WRITE 1
#define SYSCALL_GETPID 2
#define SYSCALL_READ 3
#define SYSCALL_YIELD 4
#define SYSCALL_SLEEP 5
#define SYSCALL_WAITPID 6
#define SYSCALL_SBRK 7
#define SYSCALL_MMAP 8
#define SYSCALL_KILL 9
#define SYSCALL_GETTICKS 10

#define NR_SYSCALLS 11

static void *syscall_table[NR_SYSCALLS] = {0};

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

static int sys_sbrk(int inc, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (inc == 0) {
        return (int)current_task->user_brk;
    }
    if (!current_task) return -1;
    if ((int)inc < 0) return -1;
    uint32_t old = current_task->user_brk;
    uint32_t newbrk = (old + (uint32_t)inc + 0xFFF) & ~0xFFFu;
    vmm_map_range_alloc(old, newbrk - old, 0x7);
    current_task->user_brk = newbrk;
    return (int)old;
}

static int sys_mmap(int len, const char *unused, int prot) {
    (void)unused; (void)prot;
    if (len <= 0) return -1;
    uint32_t base = 0x00600000;
    uint32_t size = (len + 0xFFF) & ~0xFFFu;
    vmm_map_range_alloc(base, size, 0x7);
    return (int)base;
}

static int sys_kill(int pid, const char *unused, int code) {
    (void)unused;
    task_t* t = task_find((pid_t)pid);
    if (!t) return -1;
    task_kill(t, (uint32_t)code);
    return 0;
}

static int sys_getticks(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint32_t tick_count;
    mutex_lock(&print_lock);
    printf("SYSCALL GETTICKS: pid=%u tick_count=%u\n", current_task ? current_task->pid : 0, tick_count);
    mutex_unlock(&print_lock);
    return (int)tick_count;
}


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
    if ((fd != 1 && fd != 2) || !buf || len < 0) return -1;
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
    syscall_table[SYSCALL_GETPID] = sys_getpid;
    syscall_table[SYSCALL_READ] = sys_read;
    syscall_table[SYSCALL_YIELD] = sys_yield;
    syscall_table[SYSCALL_SLEEP] = sys_sleep;
    syscall_table[SYSCALL_WAITPID] = sys_waitpid;
    syscall_table[SYSCALL_SBRK] = sys_sbrk;
    syscall_table[SYSCALL_MMAP] = sys_mmap;
    syscall_table[SYSCALL_KILL] = sys_kill;
    syscall_table[SYSCALL_GETTICKS] = sys_getticks;
}
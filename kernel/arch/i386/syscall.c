#include <kernel/registers.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>
#include <kernel/keyboard.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/mem_map.h>
#include <kernel/initrd.h>

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
#define SYSCALL_TIME 11
#define SYSCALL_ISATTY 12
#define SYSCALL_FORK 13
#define SYSCALL_EXEC 14
#define SYSCALL_INITRD_COUNT 15
#define SYSCALL_INITRD_STAT 16
#define SYSCALL_INITRD_READ 17
#define SYSCALL_OPEN 18
#define SYSCALL_CLOSE 19
#define SYSCALL_FSTAT 20

#define NR_SYSCALLS 21

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
    if (newbrk >= 0x007F0000 || newbrk < old) return -1;
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
    return (int)tick_count;
}

static int sys_time(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint32_t tick_count;
    extern uint32_t pit_freq;
    if (pit_freq == 0) return 0;
    return (int)(tick_count / pit_freq);
}

static int sys_isatty(int fd, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
}

static registers_t *fork_regs_ptr = NULL;

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

static int sys_initrd_count(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    return (int)initrd_get_file_count();
}

static int sys_initrd_stat(int index, const char *name_buf, int len_ptr) {
    initrd_file_t *f = initrd_get_file((uint32_t)index);
    if (!f) return -1;
    
    uint32_t name_addr = (uint32_t)name_buf;
    uint32_t len_addr = (uint32_t)len_ptr;
    
    if (name_addr && name_addr < 0xC0000000 && name_addr >= 0x1000) {
        int copylen = 63;
        int i = 0;
        for (; i < copylen && f->name[i]; i++) {
            ((char*)name_addr)[i] = f->name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }
    
    if (len_addr && len_addr < 0xC0000000 && len_addr >= 0x1000) {
        *(uint32_t*)len_addr = f->length;
    }
    
    return 0;
}

static int sys_initrd_read(int index, const char *buf, int maxlen) {
    initrd_file_t *f = initrd_get_file((uint32_t)index);
    if (!f) return -1;
    
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    
    uint32_t to_copy = (uint32_t)maxlen;
    if (to_copy > f->length) to_copy = f->length;
    
    memcpy((void*)buf_addr, f->data, to_copy);
    return (int)to_copy;
}

static int sys_open(int path_ptr, const char *flags_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    
    const char *path = (const char *)path_addr;
    int flags = (int)(uintptr_t)flags_ptr;
    
    return initrd_open(path, flags);
}

static int sys_close(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    return initrd_close(fd);
}

static int sys_fstat(int fd, const char *size_ptr, int type_ptr) {
    uint32_t size_addr = (uint32_t)size_ptr;
    uint32_t type_addr = (uint32_t)type_ptr;
    
    if (fd < 0 || fd >= INITRD_MAX_FDS) return -1;
    
    extern initrd_fd_t fd_table[];
    if (!fd_table[fd].in_use) return -1;
    
    initrd_file_t *f = initrd_get_file(fd_table[fd].file_index);
    if (!f) return -1;
    
    if (size_addr && size_addr < 0xC0000000 && size_addr >= 0x1000) {
        *(uint32_t *)size_addr = f->length;
    }
    if (type_addr && type_addr < 0xC0000000 && type_addr >= 0x1000) {
        *(uint8_t *)type_addr = f->type;
    }
    
    return 0;
}


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
    if (buf_addr + len >= 0xC0000000) return -1;

    asm volatile("cli");
    for (int i = 0; i < len; i++) {
        terminal_putchar(buf[i]);
    }
    asm volatile("sti");
    return len;
}

static int sys_read(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (fd == 0) {
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
    
    return initrd_read(fd, (void *)buf_addr, (uint32_t)len);
}

void do_syscall(registers_t *regs) {
    int num = regs->eax;

    set_syscall_frame(regs);
    fork_regs_ptr = regs;

    if (num < 0 || num >= NR_SYSCALLS || !syscall_table[num]) {
        clear_syscall_frame();
        fork_regs_ptr = NULL;
        regs->eax = -38;
        return;
    }

    regs->eax = ((int (*)(int, const char *, int))syscall_table[num])(
        regs->ebx, (const char *)regs->ecx, regs->edx);
    
    clear_syscall_frame();
    fork_regs_ptr = NULL;
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
    syscall_table[SYSCALL_TIME] = sys_time;
    syscall_table[SYSCALL_ISATTY] = sys_isatty;
    syscall_table[SYSCALL_FORK] = sys_fork;
    syscall_table[SYSCALL_EXEC] = sys_exec;
    syscall_table[SYSCALL_INITRD_COUNT] = sys_initrd_count;
    syscall_table[SYSCALL_INITRD_STAT] = sys_initrd_stat;
    syscall_table[SYSCALL_INITRD_READ] = sys_initrd_read;
    syscall_table[SYSCALL_OPEN] = sys_open;
    syscall_table[SYSCALL_CLOSE] = sys_close;
    syscall_table[SYSCALL_FSTAT] = sys_fstat;
}
#include "syscall_defs.h"
#include <lebirun/gdt.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/kstack.h>
#include <lebirun/creds.h>
#include <stdio.h>
#include <string.h>

extern task_t* current_task;

static uint8_t syscall_table_storage[NR_SYSCALLS][3];

typedef struct {
    int number;
    void *handler;
} syscall_table_override_t;

static syscall_table_override_t *syscall_table_overrides;
static int syscall_table_override_count;

static void syscall_table_remove_override(int number) {
    int i;
    syscall_table_override_t *resized;

    for (i = 0; i < syscall_table_override_count; i++) {
        if (syscall_table_overrides[i].number != number) continue;
        syscall_table_override_count--;
        if (i < syscall_table_override_count)
            syscall_table_overrides[i] =
                syscall_table_overrides[syscall_table_override_count];
        if (syscall_table_override_count == 0) {
            kfree(syscall_table_overrides);
            syscall_table_overrides = NULL;
        } else {
            resized = (syscall_table_override_t *)krealloc(
                syscall_table_overrides,
                (size_t)syscall_table_override_count *
                    sizeof(syscall_table_override_t));
            if (resized) syscall_table_overrides = resized;
        }
        return;
    }
}

static int syscall_table_set_override(int number, void *handler) {
    int i;
    syscall_table_override_t *resized;

    for (i = 0; i < syscall_table_override_count; i++) {
        if (syscall_table_overrides[i].number != number) continue;
        syscall_table_overrides[i].handler = handler;
        return 0;
    }
    if (syscall_table_override_count == INT32_MAX) return -1;
    if ((size_t)syscall_table_override_count + 1 >
        SIZE_MAX / sizeof(syscall_table_override_t)) return -1;
    resized = (syscall_table_override_t *)krealloc(
        syscall_table_overrides,
        (size_t)(syscall_table_override_count + 1) *
            sizeof(syscall_table_override_t));
    if (!resized) return -1;
    syscall_table_overrides = resized;
    syscall_table_overrides[syscall_table_override_count].number = number;
    syscall_table_overrides[syscall_table_override_count].handler = handler;
    syscall_table_override_count++;
    return 0;
}

void syscall_table_set(int number, void *handler) {
    uintptr_t address;
    uint32_t offset;

    if (number < 0) return;
    if (number >= NR_SYSCALLS) {
        if (handler)
            syscall_table_set_override(number, handler);
        else
            syscall_table_remove_override(number);
        return;
    }
    address = (uintptr_t)handler;
    if (!handler) {
        syscall_table_remove_override(number);
        syscall_table_storage[number][0] = 0;
        syscall_table_storage[number][1] = 0;
        syscall_table_storage[number][2] = 0;
        return;
    }
    if (address < KERNEL_VMA || address - KERNEL_VMA >= 0x1000000u) {
        if (syscall_table_set_override(number, handler) == 0) {
            syscall_table_storage[number][0] = 0;
            syscall_table_storage[number][1] = 0;
            syscall_table_storage[number][2] = 0;
        }
        return;
    }
    syscall_table_remove_override(number);
    offset = (uint32_t)(address - KERNEL_VMA);
    syscall_table_storage[number][0] = (uint8_t)offset;
    syscall_table_storage[number][1] = (uint8_t)(offset >> 8);
    syscall_table_storage[number][2] = (uint8_t)(offset >> 16);
}

void *syscall_table_get(int number) {
    uint32_t offset;
    int i;

    if (number < 0) return NULL;
    for (i = 0; i < syscall_table_override_count; i++) {
        if (syscall_table_overrides[i].number == number)
            return syscall_table_overrides[i].handler;
    }
    if (number >= NR_SYSCALLS) return NULL;
    offset = (uint32_t)syscall_table_storage[number][0];
    offset |= (uint32_t)syscall_table_storage[number][1] << 8;
    offset |= (uint32_t)syscall_table_storage[number][2] << 16;
    if (!offset) return NULL;
    return (void *)(uintptr_t)(KERNEL_VMA + offset);
}

void syscall_set_exec_completed(void) {
    if (current_task) {
        current_task->exec_completed = 1;
    }
}

int syscall_check_exec_completed(void) {
    if (current_task) {
        return current_task->exec_completed;
    }
    return 0;
}

void syscall_clear_exec_completed(void) {
    if (current_task) {
        current_task->exec_completed = 0;
    }
}

struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int seg_32bit:1;
    unsigned int contents:2;
    unsigned int read_exec_only:1;
    unsigned int limit_in_pages:1;
    unsigned int seg_not_present:1;
    unsigned int useable:1;
};

static int do_set_thread_area(struct user_desc *u_info) {
    uint64_t base;

    if (!u_info)
        return -1;
    if ((uint64_t)(uintptr_t)u_info >= KERNEL_VMA)
        return -1;

    base = (uint64_t)(uintptr_t)u_info;
    __asm__ volatile (
        "wrmsr"
        :
        : "c"(0xC0000100u),
          "a"((uint32_t)(base & 0xFFFFFFFF)),
          "d"((uint32_t)(base >> 32))
        : "memory"
    );

    if (current_task) {
        current_task->tls_base = base;
        current_task->tls_limit = 0;
    }

    return 0;
}

static const uint16_t linux_syscall_map[437] = {
#define LINUX_SYSCALL(linux_nr, leb_nr) [linux_nr] = (leb_nr) + 1
    LINUX_SYSCALL(1, SYSCALL_EXIT),
    LINUX_SYSCALL(2, SYSCALL_FORK),
    LINUX_SYSCALL(3, SYSCALL_READ),
    LINUX_SYSCALL(4, SYSCALL_WRITE),
    LINUX_SYSCALL(5, SYSCALL_VFS_OPEN),
    LINUX_SYSCALL(6, SYSCALL_VFS_CLOSE),
    LINUX_SYSCALL(7, SYSCALL_WAITPID),
    LINUX_SYSCALL(8, SYSCALL_VFS_CREATE),
    LINUX_SYSCALL(9, SYSCALL_LINK),
    LINUX_SYSCALL(10, SYSCALL_VFS_UNLINK),
    LINUX_SYSCALL(11, SYSCALL_EXECVE),
    LINUX_SYSCALL(12, SYSCALL_CHDIR),
    LINUX_SYSCALL(13, SYSCALL_TIME),
    LINUX_SYSCALL(15, SYSCALL_FCHMOD),
    LINUX_SYSCALL(18, SYSCALL_STAT),
    LINUX_SYSCALL(19, SYSCALL_LSEEK),
    LINUX_SYSCALL(20, SYSCALL_GETPID),
    LINUX_SYSCALL(23, SYSCALL_SETUID),
    LINUX_SYSCALL(24, SYSCALL_GETUID),
    LINUX_SYSCALL(27, SYSCALL_ALARM),
    LINUX_SYSCALL(29, SYSCALL_PAUSE),
    LINUX_SYSCALL(33, SYSCALL_ACCESS),
    LINUX_SYSCALL(36, SYSCALL_SYNC),
    LINUX_SYSCALL(37, SYSCALL_KILL),
    LINUX_SYSCALL(38, SYSCALL_RENAME),
    LINUX_SYSCALL(39, SYSCALL_VFS_MKDIR),
    LINUX_SYSCALL(41, SYSCALL_DUP2),
    LINUX_SYSCALL(42, SYSCALL_PIPE),
    LINUX_SYSCALL(45, SYSCALL_SBRK),
    LINUX_SYSCALL(46, SYSCALL_SETGID),
    LINUX_SYSCALL(47, SYSCALL_GETGID),
    LINUX_SYSCALL(49, SYSCALL_GETEUID),
    LINUX_SYSCALL(50, SYSCALL_GETEGID),
    LINUX_SYSCALL(54, SYSCALL_IOCTL),
    LINUX_SYSCALL(55, SYSCALL_FCNTL),
    LINUX_SYSCALL(57, SYSCALL_SETPGID),
    LINUX_SYSCALL(60, SYSCALL_UMASK),
    LINUX_SYSCALL(61, SYSCALL_CHROOT),
    LINUX_SYSCALL(63, SYSCALL_DUP),
    LINUX_SYSCALL(64, SYSCALL_GETPPID),
    LINUX_SYSCALL(65, SYSCALL_GETPGRP),
    LINUX_SYSCALL(66, SYSCALL_SETSID),
    LINUX_SYSCALL(67, SYSCALL_SIGACTION),
    LINUX_SYSCALL(70, SYSCALL_SETREUID),
    LINUX_SYSCALL(71, SYSCALL_SETREGID),
    LINUX_SYSCALL(75, SYSCALL_SETRLIMIT),
    LINUX_SYSCALL(76, SYSCALL_GETRLIMIT),
    LINUX_SYSCALL(77, SYSCALL_GETRUSAGE),
    LINUX_SYSCALL(78, SYSCALL_GETTIMEOFDAY),
    LINUX_SYSCALL(79, SYSCALL_SETTIMEOFDAY),
    LINUX_SYSCALL(80, SYSCALL_GETGROUPS),
    LINUX_SYSCALL(81, SYSCALL_SETGROUPS),
    LINUX_SYSCALL(82, SYSCALL_SELECT),
    LINUX_SYSCALL(83, SYSCALL_SYMLINK),
    LINUX_SYSCALL(85, SYSCALL_READLINK),
    LINUX_SYSCALL(88, SYSCALL_REBOOT),
    LINUX_SYSCALL(90, SYSCALL_MMAP),
    LINUX_SYSCALL(91, SYSCALL_MUNMAP),
    LINUX_SYSCALL(92, SYSCALL_TRUNCATE),
    LINUX_SYSCALL(93, SYSCALL_FTRUNCATE),
    LINUX_SYSCALL(94, SYSCALL_FCHOWN),
    LINUX_SYSCALL(99, SYSCALL_STATFS),
    LINUX_SYSCALL(100, SYSCALL_FSTATFS),
    LINUX_SYSCALL(106, SYSCALL_STAT),
    LINUX_SYSCALL(108, SYSCALL_FSTAT),
    LINUX_SYSCALL(114, SYSCALL_WAIT4),
    LINUX_SYSCALL(116, SYSCALL_SYSINFO),
    LINUX_SYSCALL(118, SYSCALL_FSYNC),
    LINUX_SYSCALL(119, SYSCALL_RT_SIGRETURN),
    LINUX_SYSCALL(120, SYSCALL_CLONE),
    LINUX_SYSCALL(122, SYSCALL_UNAME),
    LINUX_SYSCALL(125, SYSCALL_MPROTECT),
    LINUX_SYSCALL(126, SYSCALL_SIGPROCMASK),
    LINUX_SYSCALL(132, SYSCALL_GETPGID),
    LINUX_SYSCALL(133, SYSCALL_FCHDIR),
    LINUX_SYSCALL(138, SYSCALL_SETFSUID),
    LINUX_SYSCALL(139, SYSCALL_SETFSGID),
    LINUX_SYSCALL(140, SYSCALL_LSEEK),
    LINUX_SYSCALL(141, SYSCALL_VFS_READDIR),
    LINUX_SYSCALL(142, SYSCALL_SELECT),
    LINUX_SYSCALL(143, SYSCALL_FLOCK),
    LINUX_SYSCALL(144, SYSCALL_MSYNC),
    LINUX_SYSCALL(145, SYSCALL_READV),
    LINUX_SYSCALL(146, SYSCALL_WRITEV),
    LINUX_SYSCALL(147, SYSCALL_GETSID),
    LINUX_SYSCALL(148, SYSCALL_FDATASYNC),
    LINUX_SYSCALL(162, SYSCALL_SLEEP),
    LINUX_SYSCALL(163, SYSCALL_MREMAP),
    LINUX_SYSCALL(164, SYSCALL_SETRESUID),
    LINUX_SYSCALL(165, SYSCALL_GETRESUID),
    LINUX_SYSCALL(168, SYSCALL_POLL),
    LINUX_SYSCALL(170, SYSCALL_SETRESGID),
    LINUX_SYSCALL(171, SYSCALL_GETRESGID),
    LINUX_SYSCALL(172, SYSCALL_PRCTL),
    LINUX_SYSCALL(173, SYSCALL_RT_SIGRETURN),
    LINUX_SYSCALL(174, SYSCALL_RT_SIGACTION),
    LINUX_SYSCALL(175, SYSCALL_RT_SIGPROCMASK),
    LINUX_SYSCALL(176, SYSCALL_RT_SIGPENDING),
    LINUX_SYSCALL(177, SYSCALL_RT_SIGSUSPEND),
    LINUX_SYSCALL(178, SYSCALL_RT_SIGTIMEDWAIT),
    LINUX_SYSCALL(179, SYSCALL_RT_SIGQUEUEINFO),
    LINUX_SYSCALL(180, SYSCALL_PREAD64),
    LINUX_SYSCALL(181, SYSCALL_PWRITE64),
    LINUX_SYSCALL(183, SYSCALL_GETCWD),
    LINUX_SYSCALL(184, SYSCALL_CAPGET),
    LINUX_SYSCALL(185, SYSCALL_CAPSET),
    LINUX_SYSCALL(186, SYSCALL_SIGALTSTACK),
    LINUX_SYSCALL(190, SYSCALL_VFORK),
    LINUX_SYSCALL(191, SYSCALL_GETRLIMIT),
    LINUX_SYSCALL(192, SYSCALL_MMAP2),
    LINUX_SYSCALL(195, SYSCALL_STAT),
    LINUX_SYSCALL(197, SYSCALL_FSTAT),
    LINUX_SYSCALL(199, SYSCALL_GETUID),
    LINUX_SYSCALL(200, SYSCALL_GETGID),
    LINUX_SYSCALL(201, SYSCALL_GETEUID),
    LINUX_SYSCALL(202, SYSCALL_GETEGID),
    LINUX_SYSCALL(203, SYSCALL_SETREUID),
    LINUX_SYSCALL(204, SYSCALL_SETREGID),
    LINUX_SYSCALL(205, SYSCALL_GETGROUPS),
    LINUX_SYSCALL(206, SYSCALL_SETGROUPS),
    LINUX_SYSCALL(208, SYSCALL_SETRESUID),
    LINUX_SYSCALL(209, SYSCALL_GETRESUID),
    LINUX_SYSCALL(210, SYSCALL_SETRESGID),
    LINUX_SYSCALL(211, SYSCALL_GETRESGID),
    LINUX_SYSCALL(213, SYSCALL_SETUID),
    LINUX_SYSCALL(214, SYSCALL_SETGID),
    LINUX_SYSCALL(215, SYSCALL_SETFSUID),
    LINUX_SYSCALL(216, SYSCALL_SETFSGID),
    LINUX_SYSCALL(218, SYSCALL_MINCORE),
    LINUX_SYSCALL(219, SYSCALL_MADVISE),
    LINUX_SYSCALL(220, SYSCALL_GETDENTS64),
    LINUX_SYSCALL(221, SYSCALL_IOCTL),
    LINUX_SYSCALL(224, SYSCALL_GETTID),
    LINUX_SYSCALL(238, SYSCALL_TKILL),
    LINUX_SYSCALL(240, SYSCALL_FUTEX),
    LINUX_SYSCALL(252, SYSCALL_EXIT),
    LINUX_SYSCALL(254, SYSCALL_EPOLL_CREATE),
    LINUX_SYSCALL(255, SYSCALL_EPOLL_CTL),
    LINUX_SYSCALL(256, SYSCALL_EPOLL_WAIT),
    LINUX_SYSCALL(258, SYSCALL_SET_TID_ADDRESS),
    LINUX_SYSCALL(264, SYSCALL_CLOCK_SETTIME),
    LINUX_SYSCALL(265, SYSCALL_CLOCK_GETTIME),
    LINUX_SYSCALL(266, SYSCALL_CLOCK_GETRES),
    LINUX_SYSCALL(267, SYSCALL_CLOCK_NANOSLEEP),
    LINUX_SYSCALL(268, SYSCALL_STATFS),
    LINUX_SYSCALL(269, SYSCALL_FSTATFS),
    LINUX_SYSCALL(270, SYSCALL_TGKILL),
    LINUX_SYSCALL(284, SYSCALL_WAITID),
    LINUX_SYSCALL(295, SYSCALL_OPENAT),
    LINUX_SYSCALL(296, SYSCALL_MKDIRAT),
    LINUX_SYSCALL(297, SYSCALL_MKNODAT),
    LINUX_SYSCALL(298, SYSCALL_FCHOWNAT),
    LINUX_SYSCALL(300, SYSCALL_FSTATAT),
    LINUX_SYSCALL(301, SYSCALL_UNLINKAT),
    LINUX_SYSCALL(302, SYSCALL_RENAMEAT),
    LINUX_SYSCALL(303, SYSCALL_LINKAT),
    LINUX_SYSCALL(304, SYSCALL_SYMLINKAT),
    LINUX_SYSCALL(305, SYSCALL_READLINKAT),
    LINUX_SYSCALL(306, SYSCALL_FCHMODAT),
    LINUX_SYSCALL(307, SYSCALL_FACCESSAT),
    LINUX_SYSCALL(309, SYSCALL_PPOLL),
    LINUX_SYSCALL(311, SYSCALL_SET_ROBUST_LIST),
    LINUX_SYSCALL(312, SYSCALL_GET_ROBUST_LIST),
    LINUX_SYSCALL(319, SYSCALL_EPOLL_PWAIT),
    LINUX_SYSCALL(320, SYSCALL_UTIMENSAT),
    LINUX_SYSCALL(323, SYSCALL_EVENTFD),
    LINUX_SYSCALL(324, SYSCALL_FALLOCATE),
    LINUX_SYSCALL(328, SYSCALL_EVENTFD2),
    LINUX_SYSCALL(329, SYSCALL_EPOLL_CREATE1),
    LINUX_SYSCALL(330, SYSCALL_DUP3),
    LINUX_SYSCALL(331, SYSCALL_PIPE2),
    LINUX_SYSCALL(340, SYSCALL_PRLIMIT64),
    LINUX_SYSCALL(344, SYSCALL_SYNCFS),
    LINUX_SYSCALL(353, SYSCALL_RENAMEAT2),
    LINUX_SYSCALL(355, SYSCALL_GETRANDOM),
    LINUX_SYSCALL(359, SYSCALL_SOCKET),
    LINUX_SYSCALL(360, SYSCALL_SOCKETPAIR),
    LINUX_SYSCALL(361, SYSCALL_BIND),
    LINUX_SYSCALL(362, SYSCALL_CONNECT),
    LINUX_SYSCALL(363, SYSCALL_LISTEN),
    LINUX_SYSCALL(364, SYSCALL_ACCEPT),
    LINUX_SYSCALL(365, SYSCALL_GETSOCKOPT),
    LINUX_SYSCALL(366, SYSCALL_SETSOCKOPT),
    LINUX_SYSCALL(367, SYSCALL_GETSOCKNAME),
    LINUX_SYSCALL(368, SYSCALL_GETPEERNAME),
    LINUX_SYSCALL(369, SYSCALL_SENDTO),
    LINUX_SYSCALL(370, SYSCALL_SENDMSG),
    LINUX_SYSCALL(371, SYSCALL_RECVFROM),
    LINUX_SYSCALL(372, SYSCALL_RECVMSG),
    LINUX_SYSCALL(373, SYSCALL_SHUTDOWN),
    LINUX_SYSCALL(377, SYSCALL_COPY_FILE_RANGE),
    LINUX_SYSCALL(403, SYSCALL_CLOCK_GETTIME),
    LINUX_SYSCALL(404, SYSCALL_CLOCK_SETTIME),
    LINUX_SYSCALL(406, SYSCALL_CLOCK_GETRES),
    LINUX_SYSCALL(407, SYSCALL_CLOCK_NANOSLEEP),
    LINUX_SYSCALL(436, SYSCALL_CLOSE_RANGE)
#undef LINUX_SYSCALL
};

static int linux_to_kernel_syscall(int linux_nr) {
    uint16_t mapped;

    if (linux_nr & LEBIRUN_SYSCALL_FLAG) {
        return linux_nr & ~LEBIRUN_SYSCALL_FLAG;
    }
    if (linux_nr == 243) return -243;
    if (linux_nr == 664) return SYSCALL_ACCEPT4;
    if (linux_nr < 0 || linux_nr >= (int)(sizeof(linux_syscall_map) /
        sizeof(linux_syscall_map[0]))) return -1;
    mapped = linux_syscall_map[linux_nr];
    return mapped ? (int)mapped - 1 : -1;
}

static int syscall_needs_expanded_stack(int num) {
    if (num == SYSCALL_SBRK || num == SYSCALL_MMAP ||
        num == SYSCALL_FORK || num == SYSCALL_EXEC) return 1;
    if (num >= SYSCALL_INITRD_STAT && num <= SYSCALL_VFS_UNLINK &&
        num != SYSCALL_VFS_READ) return 1;
    if (num >= SYSCALL_SATA_TEST && num <= SYSCALL_NET_HTTP_GET) return 1;
    if (num == SYSCALL_EXECVE || num == SYSCALL_COPY_FILE_RANGE) return 1;
    if (num >= SYSCALL_STAT && num <= SYSCALL_READLINK) return 1;
    if (num >= SYSCALL_OPENAT && num <= SYSCALL_RENAMEAT2) return 1;
    if (num >= SYSCALL_MMAP2 && num <= SYSCALL_MINCORE) return 1;
    if (num >= SYSCALL_FCHDIR && num <= SYSCALL_GETDENTS64) return 1;
    if (num == SYSCALL_CLONE || num == SYSCALL_VFORK) return 1;
    if (num >= SYSCALL_POSIX_OPENPT && num <= SYSCALL_LCHOWN) return 1;
    if (num >= SYSCALL_SHM_OPEN && num <= SYSCALL_DLERROR) return 1;
    if (num >= SYSCALL_STATFS && num <= SYSCALL_NET_HTTP_POST) return 1;
    if (num == SYSCALL_PIVOT_ROOT ||
        (num >= SYSCALL_VFS_MOUNT && num <= SYSCALL_LKE_LIST)) return 1;
    if (num == SYSCALL_VFS_READDIR2) return 1;
    return 0;
}

void do_syscall(registers_t *regs) {
    int linux_nr;
    int num;
    int err;
    int64_t result;
    struct user_desc *u_info;
    void *handler;

    syscall_clear_exec_completed();
    if (current_task) {
        current_task->exec_completed = 0;
    }

    linux_nr = regs->rax;
    num = linux_to_kernel_syscall(linux_nr);

    set_syscall_frame(regs);

    if (num == -243 || num == 243) {
        u_info = (struct user_desc *)regs->rbx;
        regs->rax = do_set_thread_area(u_info);
        clear_syscall_frame();
        return;
    }
    
    if (num < 0) {
        clear_syscall_frame();
        regs->rax = -ENOSYS;
        return;
    }

    handler = syscall_table_get(num);
    if (!handler) {
        clear_syscall_frame();
        regs->rax = -ENOSYS;
        return;
    }

    if (current_task && !creds_syscall_allowed(current_task, num)) {
        clear_syscall_frame();
        regs->rax = -EPERM;
        return;
    }

    if (syscall_needs_expanded_stack(num) && kstack_expand_syscall() != 0) {
        clear_syscall_frame();
        regs->rax = -ENOMEM;
        return;
    }

    if (num == SYSCALL_VFS_READDIR) {
        result = sys_vfs_readdir(regs);
    } else if (num == SYSCALL_SBRK || num == SYSCALL_LSEEK ||
               num == SYSCALL_MMAP ||
               num == SYSCALL_MMAP2 || num == SYSCALL_MREMAP ||
               num == SYSCALL_SHMAT || num == SYSCALL_GETCWD ||
               num == SYSCALL_DLSYM || num == SYSCALL_TIME) {
        result = ((int64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))handler)(
            regs->rbx, regs->rcx, regs->rdx,
            regs->rsi, regs->rdi, regs->rbp);
    } else {
        result = ((int (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))handler)(
            regs->rbx, regs->rcx, regs->rdx,
            regs->rsi, regs->rdi, regs->rbp);
    }

    if (!syscall_check_exec_completed()) {
        regs->rax = result;
    } else {
        __asm__ volatile ("" ::: "memory");
        if (regs->rip < 0x1000 || regs->rip >= KERNEL_VMA) {
            __asm__ volatile ("cli; hlt");
        }
    }

    if ((int)regs->rax < 0) {
        err = -(int)regs->rax;
        (void)err;
    }

    if (current_task && current_task->is_user
        && num != SYSCALL_RT_SIGRETURN
        && !syscall_check_exec_completed()) {
        extern int task_has_pending_signals(void);
        extern void signal_deliver_pending(registers_t *regs);
        if (task_has_pending_signals()) {
            if (kstack_expand_syscall() == 0) {
                signal_deliver_pending(regs);
            }
        }
    }

    clear_syscall_frame();
}

void KERNEL_INIT syscall_init(void) {
    memset(syscall_table_storage, 0, sizeof(syscall_table_storage));

    syscalls_core_init();
    syscalls_process_init();
    syscalls_mem_init();
    syscalls_time_init();
    syscalls_initrd_init();
    syscalls_fb_init();
    syscalls_console_init();
    syscalls_vfs_init();
    syscalls_sata_init();
    syscalls_net_init();
    syscalls_termios_init();
    syscalls_posix_init();
    syscalls_select_init();
    syscalls_socket_init();
    syscalls_at_init();
    syscalls_signal_init();
    syscalls_ids_init();
    syscalls_misc_init();
    syscalls_inotify_init();
    syscalls_epoll_init();
    syscalls_pthread_init();
    syscalls_shm_init();
    syscalls_dl_init();
    syscalls_power_init();
    syscalls_crypto_init();
}

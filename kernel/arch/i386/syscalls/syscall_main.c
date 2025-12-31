#include "syscall_defs.h"
#include <kernel/gdt.h>
#include <kernel/task.h>

extern task_t* current_task;

void *syscall_table[NR_SYSCALLS] = {0};
registers_t *fork_regs_ptr = NULL;

#define LEBIRUN_SYSCALL_FLAG 0x80000000

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
    if (!u_info) return -14;
    
    int entry = u_info->entry_number;
    if (entry == -1) {
        entry = GDT_TLS_ENTRY_1;
        u_info->entry_number = entry;
    }
    
    if (entry < GDT_TLS_ENTRY_1 || entry > GDT_TLS_ENTRY_2) {
        return -22;
    }
    
    gdt_set_tls(entry, u_info->base_addr, u_info->limit);
    
    if (current_task) {
        current_task->tls_base = u_info->base_addr;
        current_task->tls_limit = u_info->limit;
    }
    
    return 0;
}

static int linux_to_kernel_syscall(int linux_nr) {
    if (linux_nr & LEBIRUN_SYSCALL_FLAG) {
        return linux_nr & ~LEBIRUN_SYSCALL_FLAG;
    }
    
    switch (linux_nr) {
        case 1:   return SYSCALL_EXIT;
        case 3:   return SYSCALL_READ;
        case 4:   return SYSCALL_WRITE;
        case 5:   return SYSCALL_VFS_OPEN;
        case 6:   return SYSCALL_VFS_CLOSE;
        
        case 2:   return SYSCALL_FORK;
        case 7:   return SYSCALL_WAITPID;
        case 11:  return SYSCALL_EXECVE;
        case 20:  return SYSCALL_GETPID;
        case 37:  return SYSCALL_KILL;
        case 114: return SYSCALL_WAITPID;
        case 252: return SYSCALL_EXIT;
        
        case 45:  return SYSCALL_SBRK;
        case 90:  return SYSCALL_MMAP;
        case 192: return SYSCALL_MMAP2;
        case 91:  return SYSCALL_MUNMAP;
        case 125: return SYSCALL_MPROTECT;
        case 163: return SYSCALL_MREMAP;
        case 219: return SYSCALL_MADVISE;
        case 218: return SYSCALL_MINCORE;
        
        case 18:  return SYSCALL_STAT;
        case 106: return SYSCALL_STAT;
        case 108: return SYSCALL_FSTAT;
        case 195: return SYSCALL_STAT;
        case 197: return SYSCALL_FSTAT;
        
        case 54:  return SYSCALL_IOCTL;
        case 221: return SYSCALL_IOCTL;
        case 141: return SYSCALL_VFS_READDIR;
        case 220: return SYSCALL_GETDENTS64;
        case 19:  return SYSCALL_LSEEK;
        case 140: return SYSCALL_LSEEK;
        case 146: return SYSCALL_WRITEV;
        case 145: return SYSCALL_READV;
        case 180: return SYSCALL_PREAD64;
        case 181: return SYSCALL_PWRITE64;
        
        case 63:  return SYSCALL_DUP;
        case 41:  return SYSCALL_DUP2;
        case 330: return SYSCALL_DUP3;
        case 42:  return SYSCALL_PIPE;
        case 331: return SYSCALL_PIPE2;
        
        case 67:  return SYSCALL_SIGACTION;
        case 126: return SYSCALL_SIGPROCMASK;
        case 174: return SYSCALL_RT_SIGACTION;
        case 175: return SYSCALL_RT_SIGPROCMASK;
        case 176: return SYSCALL_RT_SIGPENDING;
        case 177: return SYSCALL_RT_SIGSUSPEND;
        case 178: return SYSCALL_RT_SIGTIMEDWAIT;
        case 179: return SYSCALL_RT_SIGQUEUEINFO;
        case 173: return SYSCALL_RT_SIGRETURN;
        case 238: return SYSCALL_TKILL;
        case 270: return SYSCALL_TGKILL;
        case 186: return SYSCALL_SIGALTSTACK;
        case 29:  return SYSCALL_PAUSE;
        case 27:  return SYSCALL_ALARM;
        
        case 13:  return SYSCALL_TIME;
        case 78:  return SYSCALL_GETTIMEOFDAY;
        case 265: return SYSCALL_CLOCK_GETTIME;
        case 403: return SYSCALL_CLOCK_GETTIME;
        case 162: return SYSCALL_SLEEP;
        
        case 12:  return SYSCALL_CHDIR;
        case 133: return SYSCALL_FCHDIR;
        case 183: return SYSCALL_GETCWD;
        case 33:  return SYSCALL_ACCESS;
        
        case 39:  return SYSCALL_VFS_MKDIR;
        case 10:  return SYSCALL_VFS_UNLINK;
        case 8:   return SYSCALL_VFS_CREATE;
        
        case 82:  return SYSCALL_SELECT;
        case 142: return SYSCALL_SELECT;
        case 168: return SYSCALL_POLL;
        case 309: return SYSCALL_PPOLL;
        
        case 359: return SYSCALL_SOCKET;
        case 360: return SYSCALL_SOCKETPAIR;
        case 361: return SYSCALL_BIND;
        case 362: return SYSCALL_CONNECT;
        case 363: return SYSCALL_LISTEN;
        case 364: return SYSCALL_ACCEPT;
        case 365: return SYSCALL_GETSOCKOPT;
        case 366: return SYSCALL_SETSOCKOPT;
        case 367: return SYSCALL_GETSOCKNAME;
        case 368: return SYSCALL_GETPEERNAME;
        case 369: return SYSCALL_SENDTO;
        case 370: return SYSCALL_SENDMSG;
        case 371: return SYSCALL_RECVFROM;
        case 372: return SYSCALL_RECVMSG;
        case 373: return SYSCALL_SHUTDOWN;
        case 364 + 300: return SYSCALL_ACCEPT4;
        
        case 295: return SYSCALL_OPENAT;
        case 296: return SYSCALL_MKDIRAT;
        case 297: return SYSCALL_MKNODAT;
        case 298: return SYSCALL_FCHOWNAT;
        case 301: return SYSCALL_UNLINKAT;
        case 302: return SYSCALL_RENAMEAT;
        case 303: return SYSCALL_LINKAT;
        case 304: return SYSCALL_SYMLINKAT;
        case 305: return SYSCALL_READLINKAT;
        case 306: return SYSCALL_FCHMODAT;
        case 307: return SYSCALL_FACCESSAT;
        case 300: return SYSCALL_FSTATAT;
        case 320: return SYSCALL_UTIMENSAT;
        case 353: return SYSCALL_RENAMEAT2;
        
        case 24:  return SYSCALL_GETUID;
        case 199: return SYSCALL_GETUID;
        case 47:  return SYSCALL_GETGID;
        case 200: return SYSCALL_GETGID;
        case 49:  return SYSCALL_GETEUID;
        case 201: return SYSCALL_GETEUID;
        case 50:  return SYSCALL_GETEGID;
        case 202: return SYSCALL_GETEGID;
        case 23:  return SYSCALL_SETUID;
        case 213: return SYSCALL_SETUID;
        case 46:  return SYSCALL_SETGID;
        case 214: return SYSCALL_SETGID;
        case 70:  return SYSCALL_SETREUID;
        case 203: return SYSCALL_SETREUID;
        case 71:  return SYSCALL_SETREGID;
        case 204: return SYSCALL_SETREGID;
        case 164: return SYSCALL_SETRESUID;
        case 208: return SYSCALL_SETRESUID;
        case 170: return SYSCALL_SETRESGID;
        case 210: return SYSCALL_SETRESGID;
        case 165: return SYSCALL_GETRESUID;
        case 209: return SYSCALL_GETRESUID;
        case 171: return SYSCALL_GETRESGID;
        case 211: return SYSCALL_GETRESGID;
        case 138: return SYSCALL_SETFSUID;
        case 215: return SYSCALL_SETFSUID;
        case 139: return SYSCALL_SETFSGID;
        case 216: return SYSCALL_SETFSGID;
        case 80:  return SYSCALL_GETGROUPS;
        case 205: return SYSCALL_GETGROUPS;
        case 81:  return SYSCALL_SETGROUPS;
        case 206: return SYSCALL_SETGROUPS;
        case 132: return SYSCALL_GETPGID;
        case 57:  return SYSCALL_SETPGID;
        case 65:  return SYSCALL_GETPGRP;
        case 66:  return SYSCALL_SETSID;
        case 147: return SYSCALL_GETSID;
        case 64:  return SYSCALL_GETPPID;
        case 224: return SYSCALL_GETTID;
        
        case 122: return SYSCALL_UNAME;
        case 116: return SYSCALL_SYSINFO;
        case 76:  return SYSCALL_GETRLIMIT;
        case 191: return SYSCALL_GETRLIMIT;
        case 75:  return SYSCALL_SETRLIMIT;
        case 77:  return SYSCALL_GETRUSAGE;
        case 340: return SYSCALL_PRLIMIT64;
        
        case 355: return SYSCALL_GETRANDOM;
        case 172: return SYSCALL_PRCTL;
        
        case 254: return SYSCALL_EPOLL_CREATE;
        case 329: return SYSCALL_EPOLL_CREATE1;
        case 255: return SYSCALL_EPOLL_CTL;
        case 256: return SYSCALL_EPOLL_WAIT;
        case 319: return SYSCALL_EPOLL_PWAIT;
        case 323: return SYSCALL_EVENTFD;
        case 328: return SYSCALL_EVENTFD2;
        
        case 258: return SYSCALL_SET_TID_ADDRESS;
        case 240: return SYSCALL_FUTEX;
        case 311: return SYSCALL_SET_ROBUST_LIST;
        case 312: return SYSCALL_GET_ROBUST_LIST;
        
        case 120: return SYSCALL_CLONE;
        case 190: return SYSCALL_VFORK;
        case 114 + 1: return SYSCALL_WAIT4;
        case 284: return SYSCALL_WAITID;
        
        case 92:  return SYSCALL_TRUNCATE;
        case 93:  return SYSCALL_FTRUNCATE;
        case 9:   return SYSCALL_LINK;
        case 83:  return SYSCALL_SYMLINK;
        case 85:  return SYSCALL_READLINK;
        case 60:  return SYSCALL_UMASK;
        case 15:  return SYSCALL_FCHMOD;
        case 94:  return SYSCALL_FCHOWN;
        case 118: return SYSCALL_FSYNC;
        case 148: return SYSCALL_FDATASYNC;
        case 143: return SYSCALL_FLOCK;
        case 38:  return SYSCALL_RENAME;
        case 55:  return SYSCALL_FCNTL;
        
        case 243: return -243;
        
        default:
            return -1;
    }
}

void do_syscall(registers_t *regs) {
    int linux_nr = regs->eax;
    int num;
    
    num = linux_to_kernel_syscall(linux_nr);
    
    if (num == -243) {
        struct user_desc *u_info = (struct user_desc *)regs->ebx;
        regs->eax = do_set_thread_area(u_info);
        return;
    }
    
    if (num < 0) {
        regs->eax = -ENOSYS;
        return;
    }

    set_syscall_frame(regs);
    fork_regs_ptr = regs;

    if (num >= NR_SYSCALLS || !syscall_table[num]) {
        clear_syscall_frame();
        fork_regs_ptr = NULL;
        regs->eax = -ENOSYS;
        return;
    }

    if (num == SYSCALL_VFS_READDIR) {
        regs->eax = sys_vfs_readdir(regs);
    } else {
        regs->eax = ((int (*)(int, const char *, int))syscall_table[num])(
            regs->ebx, (const char *)regs->ecx, regs->edx);
    }

    if ((int)regs->eax < 0) {
        int err = -(int)regs->eax;
        (void)err;
    }
    
    clear_syscall_frame();
    fork_regs_ptr = NULL;
}

void syscall_init(void) {
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
    syscalls_epoll_init();
    syscalls_pthread_init();
    syscalls_shm_init();
    syscalls_dl_init();
    syscalls_regex_init();
}

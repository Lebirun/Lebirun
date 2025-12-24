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
        case 295: return SYSCALL_VFS_OPEN;
        
        case 2:   return SYSCALL_FORK;
        case 7:   return SYSCALL_WAITPID;
        case 11:  return SYSCALL_EXECVE;
        case 20:  return SYSCALL_GETPID;
        case 37:  return SYSCALL_KILL;
        case 114: return SYSCALL_WAITPID;
        case 252: return SYSCALL_EXIT;
        
        case 45:  return SYSCALL_SBRK;
        case 90:  return SYSCALL_MMAP;
        case 192: return SYSCALL_MMAP;
        case 91:  return SYSCALL_MUNMAP;
        case 125: return SYSCALL_MPROTECT;
        
        case 18:  return SYSCALL_STAT;
        case 106: return SYSCALL_STAT;
        case 108: return SYSCALL_FSTAT;
        case 195: return SYSCALL_STAT;
        case 197: return SYSCALL_FSTAT;
        
        case 54:  return SYSCALL_IOCTL;
        case 221: return SYSCALL_IOCTL;
        case 141: return SYSCALL_VFS_READDIR;
        case 220: return SYSCALL_VFS_READDIR;
        case 19:  return SYSCALL_LSEEK;
        case 140: return SYSCALL_LSEEK;
        case 146: return SYSCALL_WRITEV;
        
        case 63:  return SYSCALL_DUP;
        case 41:  return SYSCALL_DUP2;
        case 42:  return SYSCALL_PIPE;
        
        case 67:  return SYSCALL_SIGACTION;
        case 126: return SYSCALL_SIGPROCMASK;
        case 174: return SYSCALL_SIGACTION;
        case 175: return SYSCALL_SIGPROCMASK;
        
        case 13:  return SYSCALL_TIME;
        case 78:  return SYSCALL_GETTIMEOFDAY;
        case 265: return SYSCALL_CLOCK_GETTIME;
        case 403: return SYSCALL_CLOCK_GETTIME;
        case 162: return SYSCALL_SLEEP;
        
        case 12:  return SYSCALL_CHDIR;
        case 183: return SYSCALL_GETCWD;
        case 33:  return SYSCALL_ACCESS;
        
        case 39:  return SYSCALL_VFS_MKDIR;
        case 296: return SYSCALL_VFS_MKDIR;
        case 10:  return SYSCALL_VFS_UNLINK;
        case 301: return SYSCALL_VFS_UNLINK;
        case 8:   return SYSCALL_VFS_CREATE;
        
        case 173: return -38;
        
        case 240: return -38;
        case 422: return -38;
        case 258: return SYSCALL_GETPID;
        case 243: return -38;
        
        case 122: return -38;
        case 191: return -38;
        case 224: return SYSCALL_GETPID;
        case 158: return SYSCALL_YIELD;
        case 172: return -38;
        
        case 24:  return -38;
        case 47:  return -38;
        case 49:  return -38;
        case 50:  return -38;
        case 199: return -38;
        case 200: return -38;
        case 201: return -38;
        case 202: return -38;
        
        case 55:  return SYSCALL_FCNTL;
        case 38:  return SYSCALL_RENAME;
        case 268: return -38;
        case 102: return -38;
        case 118: return -38;
        case 119: return -38;
        case 120: return -38;
        case 168: return -38;
        case 169: return -38;
        
        case 92:  return SYSCALL_TRUNCATE;
        case 93:  return SYSCALL_FTRUNCATE;
        case 9:   return SYSCALL_LINK;
        case 83:  return SYSCALL_SYMLINK;
        case 85:  return SYSCALL_READLINK;
        case 60:  return SYSCALL_UMASK;
        
        default:
            return -1;
    }
}

void do_syscall(registers_t *regs) {
    int linux_nr = regs->eax;
    int num;
    
    num = linux_to_kernel_syscall(linux_nr);
    
    if (num == -38) {
        switch (linux_nr) {
            case 174:
            case 175:
            case 258:
                regs->eax = (linux_nr == 258) ? 1 : 0;
                return;
            case 243: {
                struct user_desc *u_info = (struct user_desc *)regs->ebx;
                regs->eax = do_set_thread_area(u_info);
                return;
            }
            case 24: case 47: case 49: case 50:
            case 199: case 200: case 201: case 202:
                regs->eax = 0;
                return;
            case 122:
                regs->eax = -38;
                return;
            default:
                regs->eax = -38;
                return;
        }
    }
    
    if (num < 0) {
        printf("[SYSCALL] Unknown Linux syscall %d (EBX=%d ECX=%d EDX=%d)\n",
               linux_nr, regs->ebx, regs->ecx, regs->edx);
        regs->eax = -38;
        return;
    }

    set_syscall_frame(regs);
    fork_regs_ptr = regs;

    if (num < 0 || num >= NR_SYSCALLS || !syscall_table[num]) {
        clear_syscall_frame();
        fork_regs_ptr = NULL;
        regs->eax = -38;
        return;
    }

    if (num == SYSCALL_VFS_READDIR) {
        regs->eax = sys_vfs_readdir(regs);
    } else {
        regs->eax = ((int (*)(int, const char *, int))syscall_table[num])(
            regs->ebx, (const char *)regs->ecx, regs->edx);
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
}

#include "syscall_defs.h"

void *syscall_table[NR_SYSCALLS] = {0};
registers_t *fork_regs_ptr = NULL;

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
}

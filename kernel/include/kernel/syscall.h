#ifndef SYSCALL_H
#define SYSCALL_H

#include <kernel/registers.h>
#include <kernel/common.h>

#define SYSCALL_WRITE 1
#define USER_CS 0x1B
#define USER_DS 0x23
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

void syscall_init(void);
void do_syscall(registers_t *regs);

#endif
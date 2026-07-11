#ifndef SYSCALL_H
#define SYSCALL_H

#include <lebirun/registers.h>
#include <lebirun/common.h>

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
#define SYSCALL_FORK 13
#define SYSCALL_EXEC 14

#define USER_CS 0x1B
#define USER_DS 0x23
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

void syscall_init(void);
void do_syscall(registers_t *regs);

#endif
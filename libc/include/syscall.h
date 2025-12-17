#ifndef _SYSCALL_H
#define _SYSCALL_H 1

#include <sys/cdefs.h>

#define SYS_EXIT     0
#define SYS_WRITE    1
#define SYS_GETPID   2
#define SYS_READ     3
#define SYS_YIELD    4
#define SYS_SLEEP    5
#define SYS_WAITPID  6
#define SYS_SBRK     7
#define SYS_MMAP     8
#define SYS_KILL     9
#define SYS_GETTICKS 10
#define SYS_TIME     11
#define SYS_ISATTY   12
#define SYS_FORK     13
#define SYS_EXEC     14
#define SYS_INITRD_COUNT 15
#define SYS_INITRD_STAT 16
#define SYS_INITRD_READ 17
#define SYS_OPEN 18
#define SYS_CLOSE 19
#define SYS_FSTAT 20

#ifndef __is_libk

static inline int syscall0(int num) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline int syscall1(int num, int arg1) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1)
        : "memory"
    );
    return ret;
}

static inline int syscall2(int num, int arg1, int arg2) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2)
        : "memory"
    );
    return ret;
}

static inline int syscall3(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

#endif

#endif

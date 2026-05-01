#ifndef KERNEL_REGISTERS_H
#define KERNEL_REGISTERS_H

#include <stdint.h>

typedef struct registers {
    uint64_t return_cr3, entry_cr3;
    uint64_t es, ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, saved_entry_cr3, rcx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

#endif

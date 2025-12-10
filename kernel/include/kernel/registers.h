#ifndef KERNEL_REGISTERS_H
#define KERNEL_REGISTERS_H

#include <stdint.h>

typedef struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
    uint32_t cr3;
} registers_t;

#endif
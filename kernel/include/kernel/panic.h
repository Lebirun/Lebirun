#ifndef _KERNEL_PANIC_H
#define _KERNEL_PANIC_H

#include <kernel/registers.h>

void kernel_panic(const char *reason, registers_t *regs);

#endif

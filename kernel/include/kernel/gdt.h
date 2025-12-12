#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

void gdt_init(void);

void tss_set_esp0(uint32_t esp0);

#endif
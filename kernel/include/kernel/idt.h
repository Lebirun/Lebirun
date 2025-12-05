#ifndef ARCH_I386_IDT_H
#define ARCH_I386_IDT_H

#include <stdint.h>

extern volatile uint32_t tick_count;

void idt_init(void);
void pic_remap(void);
void keyboard_disable(void);

#endif
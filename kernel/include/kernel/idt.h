#ifndef ARCH_I386_IDT_H
#define ARCH_I386_IDT_H

#include <stdint.h>
#include <kernel/registers.h>

extern volatile uint32_t tick_count;

typedef void (*irq_handler_t)(registers_t *regs);

void idt_init(void);
void pic_remap(void);
void keyboard_disable(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unregister_handler(uint8_t irq);
void irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);

#endif
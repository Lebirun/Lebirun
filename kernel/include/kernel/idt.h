#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>
#include <kernel/registers.h>

extern volatile uint64_t tick_count;

typedef void (*irq_handler_t)(registers_t *regs);

void idt_init(void);
void idt_load(void);
void pic_remap(void);
void keyboard_disable(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unregister_handler(uint8_t irq);
void irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);

#endif
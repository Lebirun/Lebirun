#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

void gdt_init(void);
void tss_set_esp0(uint32_t esp0);
void gdt_set_tls(int entry, uint32_t base, uint32_t limit);
uint16_t gdt_get_tls_selector(int entry);

#define GDT_TLS_ENTRY_1 6
#define GDT_TLS_ENTRY_2 7

#endif
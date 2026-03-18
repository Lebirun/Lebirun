#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_init_ap(void *gdt_buf, void *tss_buf);
void tss_set_rsp0(uint64_t rsp0);
void gdt_set_tls(int entry, uint64_t base, uint64_t limit);
uint16_t gdt_get_tls_selector(int entry);

#define GDT_TLS_ENTRY_1 6
#define GDT_TLS_ENTRY_2 7

#endif
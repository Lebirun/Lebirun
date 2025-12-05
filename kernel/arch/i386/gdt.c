#include <kernel/gdt.h>
#include <kernel/tty.h>
#include <stdint.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

static gdt_entry_t gdt[3];
static gdt_ptr_t gdtp;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

extern void gdt_flush(uint32_t);

void gdt_init(void) {
    gdtp.limit = (sizeof(gdt_entry_t) * 3) - 1;
    gdtp.base  = (uint32_t)&gdt;

    for (int i = 0; i < 3; i++) {
        gdt[i].base_low    = 0;
        gdt[i].base_mid    = 0;
        gdt[i].base_high   = 0;
        gdt[i].limit_low   = 0;
        gdt[i].granularity = 0;
        gdt[i].access      = 0;
    }

    gdt_set_gate(0, 0, 0, 0, 0);

    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    asm volatile ("lgdt %0" : : "m"(gdtp) : "memory");

    gdt_flush(0x10);

    terminal_writestring("GDT table set up.\n");
    terminal_writestring("GDT loaded via LGDT.\n");
    terminal_writestring("GDT segments reloaded.\n");
}
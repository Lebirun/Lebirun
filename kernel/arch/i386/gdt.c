#include <kernel/gdt.h>
#include <kernel/tty.h>
#include <stdint.h>
#include <string.h>

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

static gdt_entry_t gdt[8] __attribute__((aligned(8)));
static gdt_ptr_t gdtp __attribute__((aligned(8)));

typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

static tss_entry_t tss;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

static void write_tss(int num, uint16_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t)&tss;
    uint32_t limit = sizeof(tss_entry_t);
    gdt_set_gate(num, base, limit, 0x89, 0x00);
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = ss0;
    tss.esp0 = esp0;
    tss.iomap_base = sizeof(tss_entry_t);
}

void tss_set_esp0(uint32_t esp0) {
    tss.esp0 = esp0;
}

void gdt_set_tls(int entry, uint32_t base, uint32_t limit) {
    if (entry < 6 || entry > 7) return;
    gdt_set_gate(entry, base, limit, 0xF2, 0xCF);
}

uint16_t gdt_get_tls_selector(int entry) {
    return (entry * 8) | 3;
}

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
    int i;
    uint32_t gdt_addr;
    uint16_t cs_selector;
    uint16_t ds_selector;
    uint16_t tss_selector;
    
    gdt_addr = (uint32_t)&gdt;
    
    gdtp.limit = (sizeof(gdt_entry_t) * 8) - 1;
    gdtp.base  = gdt_addr;

    for (i = 0; i < 8; i++) {
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

    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    write_tss(5, 0x10, 0);
    
    gdt_set_gate(6, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    gdt_set_gate(7, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    asm volatile ("lgdt %0" : : "m"(gdtp) : "memory");

    cs_selector = 0x08;
    ds_selector = 0x10;
    gdt_flush(ds_selector);

    tss_selector = 0x28;
    asm volatile ("ltr %w0" : : "r"(tss_selector) : "memory");
    
    uint16_t test_cs;
    uint16_t test_ds;
    asm volatile ("mov %%cs, %0" : "=r"(test_cs));
    asm volatile ("mov %%ds, %0" : "=r"(test_ds));
    if (test_cs != cs_selector || test_ds != ds_selector) {
        outb(0x3F8, 'G');
        outb(0x3F8, 'D');
        outb(0x3F8, 'T');
        outb(0x3F8, 'E');
        outb(0x3F8, 'R');
        outb(0x3F8, 'R');
        outb(0x3F8, '\n');
    }
}

static void ap_gdt_set_gate(gdt_entry_t *g, int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    g[num].base_low    = (base & 0xFFFF);
    g[num].base_mid    = (base >> 16) & 0xFF;
    g[num].base_high   = (base >> 24) & 0xFF;
    g[num].limit_low   = (limit & 0xFFFF);
    g[num].granularity = (limit >> 16) & 0x0F;
    g[num].granularity |= gran & 0xF0;
    g[num].access      = access;
}

void gdt_init_ap(void *gdt_buf, void *tss_buf) {
    gdt_entry_t *ap_gdt;
    tss_entry_t *ap_tss;
    gdt_ptr_t ap_gdtp;
    uint32_t tss_base;
    uint32_t tss_limit;
    uint16_t ds_selector;
    uint16_t tss_selector;
    int i;

    ap_gdt = (gdt_entry_t *)gdt_buf;
    ap_tss = (tss_entry_t *)tss_buf;

    for (i = 0; i < 8; i++) {
        ap_gdt[i].base_low    = 0;
        ap_gdt[i].base_mid    = 0;
        ap_gdt[i].base_high   = 0;
        ap_gdt[i].limit_low   = 0;
        ap_gdt[i].granularity = 0;
        ap_gdt[i].access      = 0;
    }

    ap_gdt_set_gate(ap_gdt, 0, 0, 0, 0, 0);
    ap_gdt_set_gate(ap_gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    ap_gdt_set_gate(ap_gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    ap_gdt_set_gate(ap_gdt, 3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    ap_gdt_set_gate(ap_gdt, 4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    memset(ap_tss, 0, sizeof(tss_entry_t));
    ap_tss->ss0 = 0x10;
    ap_tss->esp0 = 0;
    ap_tss->iomap_base = sizeof(tss_entry_t);

    tss_base = (uint32_t)ap_tss;
    tss_limit = sizeof(tss_entry_t);
    ap_gdt_set_gate(ap_gdt, 5, tss_base, tss_limit, 0x89, 0x00);

    ap_gdt_set_gate(ap_gdt, 6, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    ap_gdt_set_gate(ap_gdt, 7, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    ap_gdtp.limit = (sizeof(gdt_entry_t) * 8) - 1;
    ap_gdtp.base  = (uint32_t)ap_gdt;

    asm volatile ("lgdt %0" : : "m"(ap_gdtp) : "memory");

    ds_selector = 0x10;
    gdt_flush(ds_selector);

    tss_selector = 0x28;
    asm volatile ("ltr %w0" : : "r"(tss_selector) : "memory");
}
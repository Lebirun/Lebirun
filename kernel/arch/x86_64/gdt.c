#include <lebirun/gdt.h>
#include <lebirun/tty.h>
#include <lebirun/kstack.h>
#include <lebirun/mem_map.h>
#include <lebirun/smp.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    gdt_entry_t low;
    uint64_t base_upper;
    uint64_t reserved;
} __attribute__((packed)) gdt_system_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

#define GDT_ENTRIES 10
_Static_assert(sizeof(gdt_entry_t) * GDT_ENTRIES == GDT_CPU_BYTES, "gdt size");
_Static_assert(sizeof(tss64_t) == TSS_CPU_BYTES, "tss size");
static gdt_entry_t gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static gdt_ptr_t gdtp __attribute__((aligned(16)));
static tss64_t tss __attribute__((aligned(16)));

#define IST_STACK_SIZE 4096
static uint8_t ist1_stack[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist2_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

static void gdt_set_tss(int num, uint64_t base, uint64_t limit) {
    gdt_system_entry_t *sys;

    gdt_set_gate(num, (uint64_t)(base & 0xFFFFFFFF), limit, 0x89, 0x00);
    sys = (gdt_system_entry_t *)&gdt[num];
    sys->base_upper = (uint64_t)(base >> 32);
    sys->reserved = 0;
}

void tss_set_rsp0(uint64_t rsp0) {
    cpu_info_t *cpu;

    if (rsp0 != 0 && !kstack_is_in_region(rsp0) && !kstack_is_in_region(rsp0 - 1)) {
        if (rsp0 < KERNEL_VMA) {
            printf("TSS: BAD rsp0=0x%016lX (below KERNEL_VMA)\n", rsp0);
            __asm__ volatile ("cli; hlt");
        }
    }
    cpu = smp_this_cpu();
    if (cpu && !cpu->bsp && cpu->tss) {
        ((tss64_t *)cpu->tss)->rsp0 = rsp0;
        return;
    }
    tss.rsp0 = rsp0;
}

uint64_t tss_get_rsp0(void) {
    cpu_info_t *cpu;

    cpu = smp_this_cpu();
    if (cpu && !cpu->bsp && cpu->tss)
        return ((tss64_t *)cpu->tss)->rsp0;
    return tss.rsp0;
}

void gdt_set_tls(int entry, uint64_t base, uint64_t limit) {
    if (entry < 6 || entry > 7) return;
    gdt_set_gate(entry, (uint64_t)(base & 0xFFFFFFFF), limit, 0xF2, 0xCF);
}

uint16_t gdt_get_tls_selector(int entry) {
    return (entry * 8) | 3;
}

extern void gdt_flush(uint16_t);

void KERNEL_EARLY_INIT gdt_init(void) {
    int i;
    uint64_t tss_base;
    uint16_t ds_selector;
    uint16_t tss_selector;

    gdtp.limit = (sizeof(gdt_entry_t) * GDT_ENTRIES) - 1;
    gdtp.base  = (uint64_t)&gdt;

    for (i = 0; i < GDT_ENTRIES; i++) {
        gdt[i].base_low    = 0;
        gdt[i].base_mid    = 0;
        gdt[i].base_high   = 0;
        gdt[i].limit_low   = 0;
        gdt[i].granularity = 0;
        gdt[i].access      = 0;
    }

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xAF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    memset(&tss, 0, sizeof(tss));
    tss.iomap_base = sizeof(tss64_t);
    tss.ist1 = (uint64_t)(ist1_stack + IST_STACK_SIZE);
    tss.ist2 = (uint64_t)(ist2_stack + IST_STACK_SIZE);
    tss_base = (uint64_t)&tss;
    gdt_set_tss(5, tss_base, sizeof(tss64_t) - 1);

    gdt_set_gate(7, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    gdt_set_gate(8, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    asm volatile ("lgdt %0" : : "m"(gdtp) : "memory");

    ds_selector = 0x10;
    gdt_flush(ds_selector);

    tss_selector = 0x28;
    asm volatile ("ltr %w0" : : "r"(tss_selector) : "memory");
}

void KERNEL_INIT gdt_init_ap(void *gdt_buf, void *tss_buf, void *kernel_stack) {
    gdt_entry_t *ap_gdt;
    tss64_t *ap_tss;
    gdt_ptr_t ap_gdtp;
    uint64_t tss_base;
    uint16_t ds_selector;
    uint16_t tss_selector;
    int i;

    ap_gdt = (gdt_entry_t *)gdt_buf;
    ap_tss = (tss64_t *)tss_buf;

    for (i = 0; i < GDT_ENTRIES; i++) {
        ap_gdt[i].base_low    = 0;
        ap_gdt[i].base_mid    = 0;
        ap_gdt[i].base_high   = 0;
        ap_gdt[i].limit_low   = 0;
        ap_gdt[i].granularity = 0;
        ap_gdt[i].access      = 0;
    }

    ap_gdt[0] = (gdt_entry_t){0};
    ap_gdt[1] = gdt[1];
    ap_gdt[2] = gdt[2];
    ap_gdt[3] = gdt[3];
    ap_gdt[4] = gdt[4];

    memset(ap_tss, 0, sizeof(tss64_t));
    ap_tss->iomap_base = sizeof(tss64_t);
    ap_tss->rsp0 = (uint64_t)kernel_stack + KSTACK_RUNTIME_SIZE;
    ap_tss->ist1 = (uint64_t)kernel_stack + KSTACK_RUNTIME_SIZE;
    ap_tss->ist2 = (uint64_t)kernel_stack + KSTACK_RUNTIME_SIZE;
    ap_tss->ist3 = (uint64_t)kernel_stack + KSTACK_RUNTIME_SIZE;

    tss_base = (uint64_t)ap_tss;
    {
        gdt_system_entry_t *sys;
        uint64_t limit;

        limit = sizeof(tss64_t) - 1;
        ap_gdt[5].base_low    = (uint16_t)(tss_base & 0xFFFF);
        ap_gdt[5].base_mid    = (uint8_t)((tss_base >> 16) & 0xFF);
        ap_gdt[5].base_high   = (uint8_t)((tss_base >> 24) & 0xFF);
        ap_gdt[5].limit_low   = (uint16_t)(limit & 0xFFFF);
        ap_gdt[5].granularity = (uint8_t)((limit >> 16) & 0x0F);
        ap_gdt[5].access      = 0x89;
        sys = (gdt_system_entry_t *)&ap_gdt[5];
        sys->base_upper = (uint64_t)(tss_base >> 32);
        sys->reserved = 0;
    }

    ap_gdtp.limit = (sizeof(gdt_entry_t) * GDT_ENTRIES) - 1;
    ap_gdtp.base  = (uint64_t)ap_gdt;

    asm volatile ("lgdt %0" : : "m"(ap_gdtp) : "memory");

    ds_selector = 0x10;
    gdt_flush(ds_selector);

    tss_selector = 0x28;
    asm volatile ("ltr %w0" : : "r"(tss_selector) : "memory");
}

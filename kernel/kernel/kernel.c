#include <stdio.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/pit.h>
#include <kernel/keyboard.h>
#include <kernel/common.h>
#include "../arch/i386/io.h"

static inline unsigned long read_cr3(void) {
    unsigned long val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline unsigned long read_cr0(void) {
    unsigned long val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

void kernel_main(void) {
    terminal_initialize();
    gdt_init();
    idt_init();
    printf("Hello!\n");
	unsigned long cr3 = read_cr3();
	unsigned long cr0 = read_cr0();
	printf("CR3="); print_hex(cr3);
	printf(" CR0="); print_hex(cr0);
	printf("\n");
	unsigned long expected = (unsigned long)(&boot_page_directory) - 0xC0000000UL;
	printf("EXP="); print_hex(expected);
	printf("\n");
	if (cr3 == expected) {
		printf("PAGING OK\n");
	} else {
		printf("PAGING MISMATCH\n");
	}
	unsigned long *pd = (unsigned long*)0xFFFFF000UL;
	printf("PDE0="); print_hex(pd[0]);
	printf("\n");
	pic_remap();
    //keyboard_disable();
    calibrate_pit();
    pit_init(100);
    keyboard_init();

    terminal_writestring("PIC master mask: 0x");
    uint8_t master_mask = inb(0x21);
    print_hex(master_mask);
    terminal_writestring("\nPIC slave mask: 0x");
    uint8_t slave_mask = inb(0xA1);
    print_hex(slave_mask);
    terminal_writestring("\n");

	terminal_writestring("About to execute STI...\n");
    asm volatile ("sti");
	terminal_writestring("STI completed! Interrupts enabled.\n");

    for (;;) {
        char c = getchar();
        terminal_putchar(c);
    }
}

#include <stdio.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/pit.h>
#include "../arch/i386/io.h"


/* static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %b0, %w1" : : "a"(val), "d"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %w1, %b0" : "=a"(ret) : "d"(port));
    return ret;
} */

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

static void print_hex(unsigned long v) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 0; i < 8; ++i) {
        unsigned int nib = (v >> ((7 - i) * 4)) & 0xF;
        buf[i] = (nib < 10) ? ('0' + nib) : ('A' + (nib - 10));
    }
    printf("0x");
    printf(buf);
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
    keyboard_disable();
    pit_init(100);

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

    terminal_writestring("Starting PIT delay test...\n");
    uint32_t start_ticks = tick_count;  
    delay(5000);
    terminal_writestring("Waiting 5 seconds for ticks...\n");  
    uint32_t elapsed = tick_count - start_ticks;
    terminal_writestring("Elapsed ticks: ");
    char buf[10]; uint32_t temp = elapsed; int i=9; buf[9]='\0';
    do { buf[--i] = '0' + (temp % 10); temp /= 10; } while (temp);
    terminal_writestring(&buf[i] ? &buf[i] : "0"); 
    terminal_writestring("\n");
    if (elapsed >= 450) {  
        terminal_writestring("PIT WORKS! ~");
        terminal_writestring(&buf[i] ? &buf[i] : "0");
        terminal_writestring(" ticks counted.\n");
    } else {
        terminal_writestring("PIT SILENT - check masks!\n");
    }

	/*terminal_writestring("Triggering INT 0...\n");

	asm volatile ("int $0");

	terminal_writestring("This NEVER prints if IDT works\n");

	terminal_writestring("Kernel idle loop started. HLTing...\n");*/

    for (;;) {
        asm volatile ("hlt");
    }
}

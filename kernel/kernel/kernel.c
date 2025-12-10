#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/pit.h>
#include <kernel/keyboard.h>
#include <kernel/common.h>
#include <kernel/mem_map.h>
#include <kernel/debug.h>
#include "../arch/i386/io.h"

bool debugMode = false;

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

extern uint32_t multiboot_magic;
extern uint32_t multiboot_ptr;

void kernel_main(void) {
    terminal_initialize();

    init_mem_map(multiboot_magic, multiboot_ptr);

    pfa_init();

    uint32_t test_frame = pfa_alloc();
    if (test_frame) {
        printf("PFA Test alloc: Frame at 0x%08X\n", test_frame);
        
        uint32_t second_frame = pfa_alloc();
        printf("PFA Second alloc: 0x%08X\n", second_frame);
        pfa_free(test_frame);
        printf("Freed first frame.\n");
        
        uint32_t third_frame = pfa_alloc();
        printf("PFA Third (reuse?): 0x%08X\n", third_frame);
        
    } else {
        printf("PFA alloc failed - check map!\n");
    }

    heap_init();

    char *heap_buf = kmalloc(1024);
    if (heap_buf) {
        strcpy(heap_buf, "Hello heap!");
        printf("Heap test: %s\n", heap_buf);
        kfree(heap_buf);
    } else {
        printf("Heap alloc failed.\n");
    }

    heap_dump();

    serial_puts("Lebirun is about to start up!\n");

    gdt_init();
    idt_init();
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
    pit_init(100);    
    calibrate_pit();  
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
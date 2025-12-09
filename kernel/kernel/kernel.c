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
#include "../arch/i386/io.h"

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

extern uint32_t multiboot_magic;
extern uint32_t multiboot_ptr;

void kernel_main(void) {
    terminal_initialize();

    init_mem_map(multiboot_magic, multiboot_ptr);

    void *test_page = pmm_alloc_page();
    if (test_page) {
        printf("Test alloc: Page at 0x"); print_hex((unsigned long)test_page); printf("\n");
    } else {
        printf("Test alloc: Failed (map too small?)\n");
    }

    if (test_page) {
        void *kernel_pt = pmm_alloc_low_page();
        if (kernel_pt) {
            printf("Alloc'd PT at 0x"); print_hex((unsigned long)kernel_pt); printf("\n");

            uint32_t *boot_pde = (uint32_t *)0xFFFFF000;
            boot_pde[769] = ((uint32_t)kernel_pt & ~0xFFF) | 3;

            uint32_t *pt = (uint32_t *)kernel_pt;
            uint32_t pt_index = 2; 
            pt[pt_index] = ((uint32_t)test_page & ~0xFFF) | 3;

            uint32_t cr3_phys = (uint32_t)(read_cr3() & ~0xFFF);
            __asm__ volatile ("movl %0, %%cr3\n\t"
                              "movl %0, %%cr3" : : "r" (cr3_phys));

            char *virt_buf = (char *)0xC0402000;
            *(uint32_t *)virt_buf = 0xDEADBEEF;
            if (*(uint32_t *)virt_buf == 0xDEADBEEF) {
                printf("Aligned + zero + write OK (virt: 0x"); print_hex(*(uint32_t *)virt_buf); printf(")\n");
            }
            strcpy(virt_buf, "Hello safe virt RAM!");
            printf("Virt test: %s\n", virt_buf);
        } else {
            printf("PT alloc failed - no high mapping.\n");
        }
    }

    char *heap_buf = malloc_virt(1024);
    if (heap_buf) {
        strcpy(heap_buf, "Hello heap!");
        printf("Heap test: %s\n", heap_buf); 
        free_virt(heap_buf);
    } else {
        printf("Heap alloc failed.\n");
    }

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
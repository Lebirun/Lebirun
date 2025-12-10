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
#include <kernel/task.h>
#include "../arch/i386/io.h"

bool debugMode = true;

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

void idle_task(void) {
    while (1) {
        printf("Idle from Task 2! Tick: %d\n", tick_count);
        asm volatile ("hlt");
    }
}

void blinker_task(void) {
    uint16_t ds_val;
    asm volatile ("mov %%ds, %0" : "=r" (ds_val));
    terminal_writestring("Blink start! DS=0x");
    print_hex(ds_val);
    terminal_writestring(" Tick start\n");

    while (1) {
        terminal_writestring("B");
        for (volatile uint32_t i = 0; i < 10000; i++);
        schedule();
    }
}

extern uint32_t multiboot_magic;
extern uint32_t multiboot_ptr;

extern task_t* current_task;
extern task_t* ready_queue_head;

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
    pit_init(100);    
    calibrate_pit();
    init_tasks();  
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

    printf("About to create blinker task...\n");
    task_t* blink = create_task(blinker_task, TASK_READY);
    if (blink) {
        printf("Blink task created successfully (ptr=0x%08X)\n", (uint32_t)blink);
    } else {
        printf("Failed to create blinker - check heap!\n");
    }

    printf("Creating idle task...\n");
    task_t* idle = create_task(idle_task, TASK_READY);
    if (idle) {
        printf("Idle task created successfully (ptr=0x%08X)\n", (uint32_t)idle);
    } else {
        printf("Failed to create idle - check heap!\n");
    }

    printf("Starting manual scheduler test...\n");
    while (1) {
        printf("Main from Task %d! Tick: %d\n", current_task->id, tick_count);
        for (volatile uint32_t i = 0; i < 1000000; i++);
        schedule();
    }

    // for (;;) {
    //     char c = getchar();
    //     terminal_putchar(c);
    // }
}
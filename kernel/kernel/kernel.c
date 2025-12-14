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
#include <kernel/mutex.h>
#include <kernel/syscall.h>
#include <kernel/io.h>
#include "launch_user.h"

bool debugMode = true; 
int debugLevel = 3; 

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

mutex_t print_lock;

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

    mutex_init(&print_lock);

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
    init_tasks();
    pit_init(100);    
    calibrate_pit();
    keyboard_init();
    syscall_init();

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

    extern uint8_t user_shell_bin_start[], user_shell_bin_end[];
    printf("heap: verify before launching user\n");
    heap_verify();
    task_t* shell = launch_user_binary(user_shell_bin_start, user_shell_bin_end);
    printf("heap: verify after launch attempt\n");
    heap_verify();
    if (!shell) {
        printf("Failed to launch user shell\n");
    } else {
        printf("User shell launched (PID %u)!\n", shell->pid);
    }

    while (1) {
        asm volatile ("hlt");
    }
}
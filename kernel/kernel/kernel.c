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
#include "../arch/i386/io.h"

bool debugMode = true; 

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

static mutex_t print_lock;
static task_t* blink_task_ptr = NULL;

void idle_task(void) {
    static uint32_t last_print = 0;
    while (1) {
        reap_dead_tasks();
        if (tick_count - last_print >= 1000) {
            mutex_lock(&print_lock);
            printf("Idle from Task %u (pid=%u)! Tick: %d\n", current_task ? current_task->id : 0, getpid(), tick_count);
            mutex_unlock(&print_lock);
            last_print = tick_count;
        }
        yield();
        asm volatile ("hlt");
    }
}

void blinker_task(void) {
    uint16_t ds_val;
    asm volatile ("mov %%ds, %0" : "=r" (ds_val));
    terminal_writestring("Blink start! DS=0x");
    print_hex(ds_val);
    terminal_writestring(" Tick start\n");

    for (int i = 0; i < 50; i++) {
        mutex_lock(&print_lock);
        terminal_writestring("B");
        mutex_unlock(&print_lock);
        sleep_ms(100);
    }
    mutex_lock(&print_lock);
    terminal_writestring("blinker exiting\n");
    mutex_unlock(&print_lock);
    task_exit(0);
}

void joiner_task(void) {
    while (!blink_task_ptr) {
        sleep_ms(10);
    }
    uint32_t code = 0;
    int rc = task_join(blink_task_ptr, &code);
    mutex_lock(&print_lock);
    printf("joiner: task_join rc=%d exit_code=%u\n", rc, code);
    mutex_unlock(&print_lock);
    task_exit(0);
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

    DPRINTF("About to create blinker task...\n");
    blink_task_ptr = create_task(blinker_task, TASK_READY);
    if (blink_task_ptr) {
        DPRINTF("Blink task created successfully: id=%u pid=%u ptr=0x%08X\n", blink_task_ptr->id, blink_task_ptr->pid, (uint32_t)blink_task_ptr);
    } else {
        printf("Failed to create blinker - check heap!\n");
    }

    DPRINTF("Creating joiner task...\n");
    task_t* joiner = create_task(joiner_task, TASK_READY);
    if (joiner) {
        DPRINTF("Joiner task created successfully: id=%u pid=%u ptr=0x%08X\n", joiner->id, joiner->pid, (uint32_t)joiner);
    } else {
        printf("Failed to create joiner - check heap!\n");
    }

    DPRINTF("Creating idle task...\n");
    task_t* idle = create_task(idle_task, TASK_READY);
    if (idle) {
        DPRINTF("Idle task created successfully: id=%u pid=%u ptr=0x%08X\n", idle->id, idle->pid, (uint32_t)idle);
    } else {
        printf("Failed to create idle - check heap!\n");
    }

    printf("Starting manual scheduler test with keyboard...\n");
    while (1) {
        char c = getchar();
        if (c) terminal_putchar(c); 
        asm volatile ("hlt");  
        schedule();  
    }
}
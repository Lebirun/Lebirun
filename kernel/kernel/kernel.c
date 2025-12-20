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
#include <kernel/initrd.h>
#include <kernel/ramfs.h>
#include <kernel/framebuffer.h>
#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/drivers/sata/ahci.h>
#include "launch_user.h"

bool debugMode = false; 
int debugLevel = 3; 

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

mutex_t print_lock;

extern uint32_t multiboot_magic;
extern uint32_t multiboot_ptr;

extern task_t* current_task;
extern task_t* ready_queue_head;

void kernel_main(void) {
    terminal_initialize();
    console_init();

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

    uint32_t mb_page = multiboot_ptr & ~0xFFF;
    vmm_map_page(mb_page + 0xC0000000, mb_page, 0x003);
    multiboot_t *mb = (multiboot_t *)(multiboot_ptr + 0xC0000000);

    if (mb->flags & (1 << 12)) {
        terminal_init_fb(mb->framebuffer_addr, mb->framebuffer_width, 
                        mb->framebuffer_height, mb->framebuffer_pitch,
                        mb->framebuffer_bpp, mb->framebuffer_type);

        extern uint8_t unifont_psf_start[] __attribute__((weak));
        extern uint8_t unifont_psf_end[] __attribute__((weak));
        uintptr_t u_start = (uintptr_t)unifont_psf_start;
        uintptr_t u_end = (uintptr_t)unifont_psf_end;
        size_t unifont_size = 0;
        if (u_end > u_start) unifont_size = (size_t)(u_end - u_start);
        if (unifont_size > 0) {
            terminal_load_psf_font(unifont_psf_start, unifont_size);
        }

        console_init();

        printf("FB: addr=0x%llX %ux%u pitch=%u bpp=%u type=%u\n",
               (unsigned long long)mb->framebuffer_addr, 
               mb->framebuffer_width, mb->framebuffer_height,
               mb->framebuffer_pitch, mb->framebuffer_bpp, mb->framebuffer_type);
    }

    printf("MB info: flags=0x%08X mods_count=%u mods_addr=0x%08X\n", mb->flags, mb->mods_count, mb->mods_addr);

    printf("MB: first 32 bytes: ");
    uint8_t *mbb = (uint8_t *)mb;
    for (uint32_t i = 0; i < 32; i++) {
        printf("%02X", mbb[i]);
        if (i % 4 == 3) printf(" ");
    }
    printf("\n");

    if (mb->mods_count > 0 && mb->mods_addr) {
        uint32_t mods_start_page = mb->mods_addr & ~0xFFF;
        uint32_t mods_end_page = (mb->mods_addr + mb->mods_count * sizeof(multiboot_module_t) + 0xFFF) & ~0xFFF;
        printf("MB: Mapping modules array phys 0x%08X - 0x%08X\n", mods_start_page, mods_end_page);
        for (uint32_t phys = mods_start_page; phys < mods_end_page; phys += 0x1000) {
            vmm_map_page(phys + 0xC0000000, phys, 0x003);
        }

        multiboot_module_t *modarr = (multiboot_module_t *)(mb->mods_addr + 0xC0000000);
        for (uint32_t i = 0; i < mb->mods_count; i++) {
            uint32_t ms = modarr[i].mod_start;
            uint32_t me = modarr[i].mod_end;
            uint32_t msize = me - ms;
            printf("MB: module[%u]: phys 0x%08X-0x%08X (%u bytes) cmdline=0x%08X\n", i, ms, me, msize, modarr[i].cmdline);

            uint32_t pstart = ms & ~0xFFF;
            vmm_map_page(pstart + 0xC0000000, pstart, 0x003);
            uint8_t *mstart = (uint8_t *)(ms + 0xC0000000);
            printf("MB: first 16 bytes of module[%u]: ", i);
            for (uint32_t b = 0; b < 16 && b < msize; b++) printf("%02X", mstart[b]);
            printf("\n");
        }

        initrd_init(mb->mods_count, mb->mods_addr);
        initrd_list_files();
        vfs_init();
        initrd_vfs_register();
        ramfs_vfs_register();
    } else {
        printf("No multiboot modules present (mods_count=%u)\n", mb->mods_count);
    }

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
    pit_init(1000);
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

    if (ahci_init() == 0) {
        printf("AHCI SATA driver initialized successfully\n");
        
        ahci_port_t *port = ahci_get_port(0);
        if (port) {
            uint8_t test_sector[512];
            if (ahci_read_sectors(port, 0, 1, test_sector) == 0) {
                printf("AHCI: Successfully read sector 0 from drive\n");
                printf("AHCI: First 16 bytes: ");
                for (int i = 0; i < 16; i++) {
                    printf("%02X ", test_sector[i]);
                }
                printf("\n");
            }
        }
    } else {
        printf("AHCI SATA driver not available (no controller found)\n");
    }

	terminal_writestring("About to execute STI...\n");
    asm volatile ("sti");
	terminal_writestring("STI completed! Interrupts enabled.\n");

    extern uint8_t user_shell_bin_start[], user_shell_bin_end[];
    printf("heap: verify before launching user\n");
    heap_verify();

    task_t* shell = launch_user_binary(user_shell_bin_start, user_shell_bin_end, 1);
    printf("heap: verify after launch attempt\n");
    heap_verify();
    if (!shell) {
        printf("Failed to launch user shell\n");
    }

    while (1) {
        asm volatile ("hlt");
    }
}
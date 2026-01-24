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
#include <kernel/fs/ext4/ext4.h>
#include <kernel/drivers/net/net.h>
#include <kernel/vring.h>
#include <kernel/about.h>
#include "launch_user.h"

#ifndef CONFIG_DEBUG_MODE
#define CONFIG_DEBUG_MODE 0
#endif
#ifndef CONFIG_DEBUG_VERBOSITY
#define CONFIG_DEBUG_VERBOSITY 3
#endif

bool debugMode = CONFIG_DEBUG_MODE ? true : false;
int debugLevel = CONFIG_DEBUG_VERBOSITY;

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

extern uint32_t multiboot_magic;
extern uint32_t multiboot_ptr;

mutex_t print_lock;

multiboot_t *g_multiboot = NULL;

extern task_t* current_task;

extern task_t* ready_queue_head;

void kernel_main(void) {
    uint32_t test_frame;
    uint32_t second_frame;
    uint32_t third_frame;
    char *heap_buf;
    uint32_t mb_page;
    multiboot_t *mb;
    extern uint8_t unifont_psf_start[] __attribute__((weak));
    extern uint8_t unifont_psf_end[] __attribute__((weak));
    uintptr_t u_start;
    uintptr_t u_end;
    size_t unifont_size;
    extern void terminal_replay_early_boot(void);
    uint32_t saved_mods_count;
    uint32_t saved_mods_addr;
    uint8_t *mbb;
    uint32_t i;
    uint32_t mods_start_page;
    uint32_t mods_end_page;
    uint32_t phys;
    multiboot_module_t *modarr;
    uint32_t ms;
    uint32_t me;
    uint32_t msize;
    uint32_t pstart;
    uint8_t *mstart;
    uint32_t b;
    extern void procfs_init(void);
    extern void devfs_init(void);
    extern void ramfs_debug_check_root(const char *location);
    unsigned long cr3;
    unsigned long cr0;
    unsigned long expected;
    unsigned long *pd;
    int sl;
    uint8_t master_mask;
    uint8_t slave_mask;
    ahci_port_t *port;
    uint8_t test_sector[512];
    int j;
    task_t *shell;
    task_t *t;
    task_t *start;

    gdt_init();
    idt_init();

    terminal_initialize();
    console_init();

    init_mem_map(multiboot_magic, multiboot_ptr);

    pfa_init();

    test_frame = pfa_alloc();
    if (test_frame) {
        printf("PFA Test alloc: Frame at 0x%08X\n", test_frame);
        
        second_frame = pfa_alloc();
        printf("PFA Second alloc: 0x%08X\n", second_frame);
        pfa_free(test_frame);
        printf("Freed first frame.\n");
        
        third_frame = pfa_alloc();
        printf("PFA Third (reuse?): 0x%08X\n", third_frame);
        
    } else {
        printf("PFA alloc failed - check map!\n");
    }

    heap_init();

    heap_buf = kmalloc(1024);
    if (heap_buf) {
        strcpy(heap_buf, "Hello heap!");
        printf("Heap test: %s\n", heap_buf);
        kfree(heap_buf);
    } else {
        printf("Heap alloc failed.\n");
    }

    heap_dump();

    mb_page = multiboot_ptr & ~0xFFF;
    vmm_map_page(mb_page + 0xC0000000, mb_page, 0x003);
    
    mb = (multiboot_t *)(multiboot_ptr + 0xC0000000);
    g_multiboot = mb;

    if (mb->flags & (1 << 12)) {
        terminal_init_fb(mb->framebuffer_addr, mb->framebuffer_width, 
                        mb->framebuffer_height, mb->framebuffer_pitch,
                        mb->framebuffer_bpp, mb->framebuffer_type);

        u_start = (uintptr_t)unifont_psf_start;
        u_end = (uintptr_t)unifont_psf_end;
        unifont_size = 0;
        if (u_end > u_start) unifont_size = (size_t)(u_end - u_start);
        if (unifont_size > 0) {
            terminal_load_psf_font(unifont_psf_start, unifont_size);
        }

        console_init();

        terminal_replay_early_boot();

        printf("FB: addr=0x%llX %ux%u pitch=%u bpp=%u type=%u\n",
               (unsigned long long)mb->framebuffer_addr, 
               mb->framebuffer_width, mb->framebuffer_height,
               mb->framebuffer_pitch, mb->framebuffer_bpp, mb->framebuffer_type);
    }

    printf("MB info: flags=0x%08X mods_count=%u mods_addr=0x%08X\n", mb->flags, mb->mods_count, mb->mods_addr);

    saved_mods_count = mb->mods_count;
    saved_mods_addr = mb->mods_addr;

    printf("MB: first 32 bytes: ");
    mbb = (uint8_t *)mb;
    for (i = 0; i < 32; i++) {
        printf("%02X", mbb[i]);
        if (i % 4 == 3) printf(" ");
    }
    printf("\n");

    if (mb->mods_count > 0 && mb->mods_addr) {
        mods_start_page = mb->mods_addr & ~0xFFF;
        mods_end_page = (mb->mods_addr + mb->mods_count * sizeof(multiboot_module_t) + 0xFFF) & ~0xFFF;
        printf("MB: Mapping modules array phys 0x%08X - 0x%08X\n", mods_start_page, mods_end_page);
        for (phys = mods_start_page; phys < mods_end_page; phys += 0x1000) {
            vmm_map_page(phys + 0xC0000000, phys, 0x003);
        }

        modarr = (multiboot_module_t *)(mb->mods_addr + 0xC0000000);
        for (i = 0; i < mb->mods_count; i++) {
            ms = modarr[i].mod_start;
            me = modarr[i].mod_end;
            msize = me - ms;
            printf("MB: module[%u]: phys 0x%08X-0x%08X (%u bytes) cmdline=0x%08X\n", i, ms, me, msize, modarr[i].cmdline);

            pstart = ms & ~0xFFF;
            vmm_map_page(pstart + 0xC0000000, pstart, 0x003);
            mstart = (uint8_t *)(ms + 0xC0000000);
            printf("MB: first 16 bytes of module[%u]: ", i);
            for (b = 0; b < 16 && b < msize; b++) printf("%02X", mstart[b]);
            printf("\n");
        }

        initrd_init(mb->mods_count, mb->mods_addr);
        initrd_list_files();
        vfs_init();
        printf("[KERNEL] After vfs_init\n");
        initrd_vfs_register();
        printf("[KERNEL] After initrd_vfs_register\n");
        ramfs_vfs_register();
        printf("[KERNEL] After ramfs_vfs_register\n");
        ramfs_debug_check_root("kernel: after ramfs_vfs_register");
        ext4_init();
        printf("[KERNEL] After ext4_init\n");
        ramfs_debug_check_root("kernel: after ext4_init");
        ext4_vfs_register();
        printf("[KERNEL] After ext4_vfs_register\n");
        ramfs_debug_check_root("kernel: after ext4_vfs_register");
        
        procfs_init();
        printf("[KERNEL] After procfs_init\n");
        ramfs_debug_check_root("kernel: after procfs_init");
        devfs_init();
        printf("[KERNEL] After devfs_init\n");
        ramfs_debug_check_root("kernel: after devfs_init");
        
        printf("[KERNEL] About to mount /dev...\n");
        vfs_mount(NULL, "/dev", "devfs");
        printf("[KERNEL] After vfs_mount /dev\n");
        vfs_mount(NULL, "/proc", "procfs");
    } else {
        printf("No multiboot modules present (mods_count=%u)\n", mb->mods_count);
    }

    mutex_init(&print_lock);

    cr3 = read_cr3();
    cr0 = read_cr0();
    printf("CR3="); print_hex(cr3);
    printf(" CR0="); print_hex(cr0);
    printf("\n");
    if (!pae_enabled) {
        expected = (unsigned long)(&boot_page_directory) - 0xC0000000UL;
        printf("EXP="); print_hex(expected);
        printf("\n");
        if (cr3 == expected) {
            printf("PAGING OK\n");
        } else {
            printf("PAGING MISMATCH\n");
        }
        pd = (unsigned long*)0xFFFFF000UL;
        printf("PDE0="); print_hex(pd[0]);
        printf("\n");
    } else {
        printf("PAE mode: skipping legacy paging check\n");
    }
    pic_remap();
    init_tasks();
    
    vring_init();
    kproc_init();
    kproc_print_init();
    printf("VRING: Virtual rings initialized (ring 0.1 = kprint, PID -1)\n");
    
    console_writer_init();
    printf("CONSOLE: Async writer thread started\n");
    
    pit_init(1000);
    calibrate_pit();
    keyboard_init();
    syscall_init();

    if (saved_mods_count >= 2) {
        rootfs_init(saved_mods_count, saved_mods_addr);
    }

    (void)ramfs_create_dir("/bin", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
    (void)ramfs_unlink("/bin/sh");
    sl = ramfs_create_symlink("/bin/sh", "/bin/lsh", VFS_PERM_READ | VFS_PERM_EXEC);
    if (sl != 0 && sl != RAMFS_ERR_EXIST) {
        printf("BOOT: failed to create /bin/sh symlink (%d)\n", sl);
    }

    terminal_writestring("PIC master mask: 0x");
    master_mask = inb(0x21);
    print_hex(master_mask);
    terminal_writestring("\nPIC slave mask: 0x");
    slave_mask = inb(0xA1);
    print_hex(slave_mask);
    terminal_writestring("\n");

    if (ahci_init() == 0) {
        printf("AHCI SATA driver initialized successfully\n");
        
        port = ahci_get_port(0);
        if (port) {
            if (ahci_read_sectors(port, 0, 1, test_sector) == 0) {
                printf("AHCI: Successfully read sector 0 from drive\n");
                printf("AHCI: First 16 bytes: ");
                for (j = 0; j < 16; j++) {
                    printf("%02X ", test_sector[j]);
                }
                printf("\n");
            }

            if (vfs_mount(NULL, "/disk", "ext4") == 0) {
                printf("EXT4 filesystem mounted on /disk\n");
            } else {
                printf("Failed to mount ext4 filesystem (disk may not be formatted)\n");
            }
        }
    } else {
        printf("AHCI SATA driver not available (no controller found)\n");
    }

    net_init();

    terminal_writestring("About to execute STI...\n");
    asm volatile ("sti");
    terminal_writestring("STI completed! Interrupts enabled.\n");

    printf("heap: verify before launching user\n");
    heap_verify();

    shell = launch_user_path("/bin/lsh", 1);
    printf("heap: verify after launch attempt\n");
    heap_verify();
    if (!shell) {
        printf("Failed to launch user shell\n");
    } else {
        printf("Shell launched: task_id=%u is_user=%d state=%d on console 1\n", 
               shell->id, shell->is_user, shell->state);
        
        t = ready_queue_head;
        printf("Run queue: ");
        if (t) {
            start = t;
            do {
                printf("[%u s=%d u=%d] ", t->id, t->state, t->is_user);
                t = t->next;
            } while (t && t != start);
        }
        printf("\n");
    }

    while (1) {
        asm volatile ("hlt");
    }
}
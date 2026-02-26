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
#include <kernel/squashfs.h>
#include <kernel/overlayfs.h>
#include <kernel/framebuffer.h>
#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/fs/ext4/ext4.h>
#include <kernel/drivers/net/net.h>
#include <kernel/vring.h>
#include <kernel/about.h>
#include <kernel/panic.h>
#include <kernel/kstack.h>
#include <kernel/smp.h>
#include <kernel/power.h>
#include "launch_user.h"

bool debug_memory = CONFIG_DEBUG_MEMORY ? true : false;
bool debug_task = CONFIG_DEBUG_TASK ? true : false;
bool debug_vfs = CONFIG_DEBUG_VFS ? true : false;
bool debug_ramfs = CONFIG_DEBUG_RAMFS ? true : false;
bool debug_initrd = CONFIG_DEBUG_INITRD ? true : false;
bool debug_elf = CONFIG_DEBUG_ELF ? true : false;
bool debug_syscall = CONFIG_DEBUG_SYSCALL ? true : false;
bool debug_idt = CONFIG_DEBUG_IDT ? true : false;
bool debug_driver = CONFIG_DEBUG_DRIVER ? true : false;
bool debug_fs_ext4 = CONFIG_DEBUG_FS_EXT4 ? true : false;
bool debug_fs_other = CONFIG_DEBUG_FS_OTHER ? true : false;

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
    extern uint32_t boot_page_table1[];
    extern uint32_t boot_page_table2[];
    extern uint32_t boot_page_table3[];
    extern uint32_t boot_page_table4[];
    extern uint32_t boot_pd_low[];
    extern uint32_t boot_pd_high[];
    extern uint32_t boot_pt_0[];
    extern uint32_t boot_pt_1[];
    extern uint32_t boot_pt_2[];
    extern uint32_t boot_pt_3[];
    uintptr_t u_start;
    uintptr_t u_end;
    size_t unifont_size;
    extern void terminal_replay_early_boot(void);
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
    extern void sysfs_init(void);
    extern void ramfs_debug_check_root(const char *location);
    unsigned long cr3;
    unsigned long cr0;
    unsigned long expected;
    unsigned long *pd;
    int mount_ret;
    int use_squashfs;
    multiboot_module_t mod1;
    overlay_context_t *overlay_ctx;
    vfs_node_t *squashfs_root;
    vfs_node_t *ramfs_upper;
    uint8_t master_mask;
    uint8_t slave_mask;
    ahci_port_t *port;
    uint8_t test_sector[512];
    int j;
    task_t *init_task;
    task_t *t;
    task_t *start;

    gdt_init();
    idt_init();

    terminal_initialize();
    console_init();


    init_mem_map(multiboot_magic, multiboot_ptr);


    pfa_init();


    if (pae_enabled) {
        pfa_reclaim_kernel_range((uint32_t)boot_page_directory - 0xC0000000,
                                 (uint32_t)boot_page_directory - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_page_table1 - 0xC0000000,
                                 (uint32_t)boot_page_table1 - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_page_table2 - 0xC0000000,
                                 (uint32_t)boot_page_table2 - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_page_table3 - 0xC0000000,
                                 (uint32_t)boot_page_table3 - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_page_table4 - 0xC0000000,
                                 (uint32_t)boot_page_table4 - 0xC0000000 + PAGE_SIZE);
    } else {
        pfa_reclaim_kernel_range((uint32_t)boot_pd_low - 0xC0000000,
                                 (uint32_t)boot_pd_low - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_pd_high - 0xC0000000,
                                 (uint32_t)boot_pd_high - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_pt_0 - 0xC0000000,
                                 (uint32_t)boot_pt_0 - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_pt_1 - 0xC0000000,
                                 (uint32_t)boot_pt_1 - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_pt_2 - 0xC0000000,
                                 (uint32_t)boot_pt_2 - 0xC0000000 + PAGE_SIZE);
        pfa_reclaim_kernel_range((uint32_t)boot_pt_3 - 0xC0000000,
                                 (uint32_t)boot_pt_3 - 0xC0000000 + PAGE_SIZE);
    }


    test_frame = pfa_alloc();
    if (test_frame) {
        printf("PFA Test alloc: Frame at 0x%08X\n", test_frame);
        
        second_frame = pfa_alloc();
        printf("PFA Second alloc: 0x%08X\n", second_frame);
        pfa_free(test_frame);
        printf("Freed first frame.\n");
        
        third_frame = pfa_alloc();
        printf("PFA Third (reuse?): 0x%08X\n", third_frame);
        if (second_frame) {
            pfa_free(second_frame);
        }
        if (third_frame) {
            pfa_free(third_frame);
        }
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
            terminal_compact_font(256);
            {
                uint32_t font_phys_start;
                uint32_t font_phys_end;
                font_phys_start = (uint32_t)((uintptr_t)unifont_psf_start) - 0xC0000000;
                font_phys_end = (uint32_t)((uintptr_t)unifont_psf_end) - 0xC0000000;
                pfa_reclaim_kernel_range(font_phys_start, font_phys_end);
            }
        }

        console_init();

        terminal_replay_early_boot();

        printf("FB: addr=0x%llX %ux%u pitch=%u bpp=%u type=%u\n",
               (unsigned long long)mb->framebuffer_addr, 
               mb->framebuffer_width, mb->framebuffer_height,
               mb->framebuffer_pitch, mb->framebuffer_bpp, mb->framebuffer_type);
    }

    printf("MB info: flags=0x%08X mods_count=%u mods_addr=0x%08X\n", mb->flags, mb->mods_count, mb->mods_addr);

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

        initrd_init(1, mb->mods_addr);
        initrd_list_files();
        vfs_init();
        printf("[KERNEL] After vfs_init\n");
        initrd_vfs_register();
        printf("[KERNEL] After initrd_vfs_register\n");
        ramfs_vfs_register();
        printf("[KERNEL] After ramfs_vfs_register\n");
        squashfs_vfs_register();
        printf("[KERNEL] After squashfs_vfs_register\n");
        overlayfs_vfs_register();
        printf("[KERNEL] After overlayfs_vfs_register\n");

        use_squashfs = 0;
        squashfs_root = NULL;
        ramfs_upper = NULL;
        overlay_ctx = NULL;

        mount_ret = vfs_mount(NULL, "/", "ramfs");
        if (mount_ret == 0) {
            printf("[KERNEL] Mounted ramfs as root\n");
        } else {
            printf("[KERNEL] Failed to mount ramfs as root\n");
        }

        if (mb->mods_count >= 2) {
            mod1 = modarr[1];
            squashfs_init(mod1.mod_start, mod1.mod_end);
            mount_ret = vfs_mount(NULL, "/squashfs", "squashfs");
            if (mount_ret == 0) {
                squashfs_root = vfs_namei("/squashfs");
                use_squashfs = 1;
            } else {
                printf("[KERNEL] SquashFS mount failed\n");
            }
        }

        ramfs_create_dir("/var", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
        ramfs_create_dir("/tmp", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
        ramfs_create_dir("/home", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
        ramfs_create_dir("/root", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
        ramfs_create_dir("/root/.config", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
        ramfs_create_dir("/root/.config/htop", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
        ramfs_create_dir("/run", VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);

        if (use_squashfs && squashfs_root) {
            ramfs_upper = vfs_get_root();
            overlay_ctx = overlayfs_create(squashfs_root, ramfs_upper);
            if (overlay_ctx && overlay_ctx->merged_root) {
                vfs_replace_mount_root("/", overlay_ctx->merged_root);
                printf("[KERNEL] OverlayFS active: squashfs (lower) + ramfs (upper) on /\n");
            }
        } else {
            ramfs_create_symlink("/bin", "/initrd/bin", VFS_PERM_READ | VFS_PERM_EXEC);
            ramfs_create_symlink("/sbin", "/initrd/sbin", VFS_PERM_READ | VFS_PERM_EXEC);
            ramfs_create_symlink("/usr", "/initrd/usr", VFS_PERM_READ | VFS_PERM_EXEC);
            ramfs_create_symlink("/lib", "/initrd/lib", VFS_PERM_READ | VFS_PERM_EXEC);
            ramfs_create_symlink("/etc", "/initrd/etc", VFS_PERM_READ | VFS_PERM_EXEC);
        }

        printf("[BOOT] overlay/symlink done, mounting initrd...\n");
        mount_ret = vfs_mount(NULL, "/initrd", "initrd");
        printf("[BOOT] vfs_mount initrd returned %d\n", mount_ret);
        if (mount_ret == 0) {
            printf("[KERNEL] Mounted initrd on /initrd\n");
        }

        printf("[BOOT] procfs_init...\n");
        procfs_init();
        printf("[BOOT] devfs_init...\n");
        devfs_init();
        printf("[BOOT] sysfs_init...\n");
        sysfs_init();

        printf("[BOOT] mounting /dev /proc /sys...\n");
        vfs_mount(NULL, "/dev", "devfs");
        vfs_mount(NULL, "/proc", "procfs");
        vfs_mount(NULL, "/sys", "sysfs");

        if (use_squashfs) {
            printf("[BOOT] vfs_block_squashfs_access...\n");
            vfs_block_squashfs_access();
        }

        printf("[BOOT] ramfs_internalize_all...\n");
        ramfs_internalize_all();
        printf("[BOOT] initrd_free_pages...\n");
        initrd_free_pages();
        printf("[BOOT] initrd_free_pages done\n");
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
    printf("[BOOT] pic_remap...\n");
    pic_remap();
    printf("[BOOT] kstack_init...\n");
    kstack_init();
    printf("[BOOT] init_tasks...\n");
    init_tasks();
    printf("[BOOT] smp_init...\n");
    smp_init();
    printf("[BOOT] smp_init done\n");
    
    power_init();
    
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

            ext4_init();
            ext4_vfs_register();
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

    kprint_enable();

    printf("heap: verify before launching init\n");
    heap_verify();
    slab_gc();

    init_task = launch_user_path("/bin/init", 0);
    if (!init_task) {
        printf("init not found, retrying in 5 seconds...\n");
        sleep_ticks(5000);
        init_task = launch_user_path("/bin/init", 0);
    }
    printf("heap: verify after launch attempt\n");
    heap_verify();
    if (!init_task) {
        printf("FATAL: /bin/init not found. System halted.\n");
        for (;;)
            asm volatile ("hlt");
    }

    printf("Init launched: task_id=%u is_user=%d state=%d on console 0\n",
           init_task->id, init_task->is_user, init_task->state);

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

    yield();

    while (1) {
        task_deferred_work();
        asm volatile ("hlt");
    }
}
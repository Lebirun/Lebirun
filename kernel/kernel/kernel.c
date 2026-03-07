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
#include <kernel/partition.h>
#include <kernel/drivers/net/net.h>
#include <kernel/drivers/net/tls.h>
#include <kernel/vring.h>
#include <kernel/about.h>
#include <kernel/panic.h>
#include <kernel/kstack.h>
#include <kernel/smp.h>
#include <kernel/power.h>
#include <kernel/cmdline.h>
#include <kernel/rng.h>
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
bool debug_boot_vfs = CONFIG_DEBUG_BOOT_VFS ? true : false;
bool debug_boot_modules = CONFIG_DEBUG_BOOT_MODULES ? true : false;
bool debug_boot_hw = CONFIG_DEBUG_BOOT_HW ? true : false;

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
    extern int devfs_register_blockdev(const char *name, uint32_t port_index);
    extern int devfs_register_partition(const char *name, uint32_t port_index,
                                        uint64_t start_lba, uint64_t sector_count);
    extern void devfs_register_initrd(void);
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
    vfs_node_t *initrd_root;
    uint8_t master_mask;
    uint8_t slave_mask;
    ahci_port_t *port;
    int j;
    uint32_t pi;
    char devname[8];
    char partname[16];
    partition_table_t ptable;
    int pk;
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
        if (debug_memory) printf("PFA Test alloc: Frame at 0x%08X\n", test_frame);
        
        second_frame = pfa_alloc();
        if (debug_memory) printf("PFA Second alloc: 0x%08X\n", second_frame);
        pfa_free(test_frame);
        if (debug_memory) printf("Freed first frame.\n");
        
        third_frame = pfa_alloc();
        if (debug_memory) printf("PFA Third (reuse?): 0x%08X\n", third_frame);
        if (second_frame) {
            pfa_free(second_frame);
        }
        if (third_frame) {
            pfa_free(third_frame);
        }
    } else {
        if (debug_memory) printf("PFA alloc failed - check map!\n");
    }

    heap_init();


    heap_buf = kmalloc(1024);
    if (heap_buf) {
        strcpy(heap_buf, "Hello heap!");
        if (debug_memory) printf("Heap test: %s\n", heap_buf);
        kfree(heap_buf);
    } else {
        if (debug_memory) printf("Heap alloc failed.\n");
    }

    if (debug_memory) heap_dump();


    mb_page = multiboot_ptr & ~0xFFF;
    vmm_map_page(mb_page + 0xC0000000, mb_page, 0x003);
    
    mb = (multiboot_t *)(multiboot_ptr + 0xC0000000);
    g_multiboot = mb;

    cmdline_parse(mb->flags, mb->cmdline);

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

    if (debug_boot_modules) printf("MB info: flags=0x%08X mods_count=%u mods_addr=0x%08X\n", mb->flags, mb->mods_count, mb->mods_addr);

    if (debug_boot_modules) {
        printf("MB: first 32 bytes: ");
        mbb = (uint8_t *)mb;
        for (i = 0; i < 32; i++) {
            printf("%02X", mbb[i]);
            if (i % 4 == 3) printf(" ");
        }
        printf("\n");
    }

    if (mb->mods_count > 0 && mb->mods_addr) {
        mods_start_page = mb->mods_addr & ~0xFFF;
        mods_end_page = (mb->mods_addr + mb->mods_count * sizeof(multiboot_module_t) + 0xFFF) & ~0xFFF;
        if (debug_boot_modules) printf("MB: Mapping modules array phys 0x%08X - 0x%08X\n", mods_start_page, mods_end_page);
        for (phys = mods_start_page; phys < mods_end_page; phys += 0x1000) {
            vmm_map_page(phys + 0xC0000000, phys, 0x003);
        }

        modarr = (multiboot_module_t *)(mb->mods_addr + 0xC0000000);
        for (i = 0; i < mb->mods_count; i++) {
            ms = modarr[i].mod_start;
            me = modarr[i].mod_end;
            msize = me - ms;
            if (debug_boot_modules) printf("MB: module[%u]: phys 0x%08X-0x%08X (%u bytes) cmdline=0x%08X\n", i, ms, me, msize, modarr[i].cmdline);

            pstart = ms & ~0xFFF;
            vmm_map_page(pstart + 0xC0000000, pstart, 0x003);
            if (debug_boot_modules) {
                mstart = (uint8_t *)(ms + 0xC0000000);
                printf("MB: first 16 bytes of module[%u]: ", i);
                for (b = 0; b < 16 && b < msize; b++) printf("%02X", mstart[b]);
                printf("\n");
            }
        }

        initrd_init(1, mb->mods_addr);
        initrd_list_files();
        vfs_init();
        if (debug_boot_vfs) printf("[KERNEL] After vfs_init\n");
        initrd_vfs_register();
        if (debug_boot_vfs) printf("[KERNEL] After initrd_vfs_register\n");
        ramfs_vfs_register();
        if (debug_boot_vfs) printf("[KERNEL] After ramfs_vfs_register\n");
        squashfs_vfs_register();
        if (debug_boot_vfs) printf("[KERNEL] After squashfs_vfs_register\n");
        overlayfs_vfs_register();
        if (debug_boot_vfs) printf("[KERNEL] After overlayfs_vfs_register\n");

        use_squashfs = 0;
        squashfs_root = NULL;
        ramfs_upper = NULL;
        overlay_ctx = NULL;

        mount_ret = vfs_mount(NULL, "/", "ramfs");
        if (mount_ret == 0) {
            if (debug_boot_vfs) printf("[KERNEL] Mounted ramfs as root\n");
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
            mount_ret = vfs_mount(NULL, "/ro", "initrd");
            if (mount_ret == 0) {
                initrd_root = vfs_namei("/ro");
                if (initrd_root) {
                    ramfs_upper = vfs_get_root();
                    overlay_ctx = overlayfs_create(initrd_root, ramfs_upper);
                    if (overlay_ctx && overlay_ctx->merged_root) {
                        vfs_replace_mount_root("/", overlay_ctx->merged_root);
                        printf("[KERNEL] OverlayFS active: initrd (lower) + ramfs (upper) on /\n");
                    }
                }
            }
        }

        if (debug_boot_vfs) printf("[BOOT] procfs_init...\n");
        procfs_init();
        if (debug_boot_vfs) printf("[BOOT] devfs_init...\n");
        devfs_init();
        devfs_register_initrd();
        if (debug_boot_vfs) printf("[BOOT] sysfs_init...\n");
        sysfs_init();

        if (debug_boot_vfs) printf("[BOOT] mounting /dev /proc /sys...\n");
        vfs_mount(NULL, "/dev", "devfs");
        vfs_mount(NULL, "/proc", "procfs");
        vfs_mount(NULL, "/sys", "sysfs");

        if (use_squashfs) {
            if (debug_boot_vfs) printf("[BOOT] vfs_block_squashfs_access...\n");
            vfs_block_squashfs_access();
        }

        if (debug_boot_vfs) printf("[BOOT] ramfs_internalize_all...\n");
        ramfs_internalize_all();
        if (debug_boot_vfs) printf("[BOOT] initrd_free_pages...\n");
        initrd_free_pages();
        if (debug_boot_vfs) printf("[BOOT] initrd_free_pages done\n");
    } else {
        printf("No multiboot modules present (mods_count=%u)\n", mb->mods_count);
    }

    mutex_init(&print_lock);

    cr3 = read_cr3();
    cr0 = read_cr0();
    if (debug_boot_hw) {
        printf("CR3="); print_hex(cr3);
        printf(" CR0="); print_hex(cr0);
        printf("\n");
    }
    if (!pae_enabled) {
        expected = (unsigned long)(&boot_page_directory) - 0xC0000000UL;
        if (debug_boot_hw) {
            printf("EXP="); print_hex(expected);
            printf("\n");
        }
        if (cr3 == expected) {
            if (debug_boot_hw) printf("PAGING OK\n");
        } else {
            printf("PAGING MISMATCH\n");
        }
        pd = (unsigned long*)0xFFFFF000UL;
        if (debug_boot_hw) {
            printf("PDE0="); print_hex(pd[0]);
            printf("\n");
        }
    } else {
        if (debug_boot_hw) printf("PAE mode: skipping legacy paging check\n");
    }
    if (debug_boot_hw) printf("[BOOT] pic_remap...\n");
    pic_remap();
    if (debug_boot_hw) printf("[BOOT] kstack_init...\n");
    kstack_init();
    if (debug_boot_hw) printf("[BOOT] init_tasks...\n");
    init_tasks();
    if (debug_boot_hw) printf("[BOOT] smp_init...\n");
    smp_init();
    if (debug_boot_hw) printf("[BOOT] smp_init done\n");
    
    power_init();
    
    vring_init();
    kproc_init();
    kproc_print_init();
    if (debug_boot_hw) printf("VRING: Virtual rings initialized (ring 0.1 = kprint, PID -1)\n");
    
    console_writer_init();
    if (debug_boot_hw) printf("CONSOLE: Async writer thread started\n");
    
    pit_init(1000);
    calibrate_pit();

    if (lapic_base) {
        lapic_timer_init(1000);
        ioapic_mask_irq(0);
        printf("TIMER: LAPIC timer active at 1000 Hz (PIT masked)\n");
    } else {
        printf("TIMER: PIT active at 1000 Hz (no LAPIC)\n");
    }

    rng_init();
    keyboard_init();
    syscall_init();

    if (debug_boot_hw) {
        terminal_writestring("PIC master mask: 0x");
        master_mask = inb(0x21);
        print_hex(master_mask);
        terminal_writestring("\nPIC slave mask: 0x");
        slave_mask = inb(0xA1);
        print_hex(slave_mask);
        terminal_writestring("\n");
    }

    if (ahci_init() == 0) {
        printf("AHCI SATA driver initialized successfully\n");
        
        ext4_init();
        ext4_vfs_register();

        j = 0;
        for (pi = 0; pi < AHCI_MAX_PORTS; pi++) {
            port = ahci_get_port(pi);
            if (port) {
                devname[0] = 's';
                devname[1] = 'd';
                devname[2] = (char)('a' + j);
                devname[3] = '\0';
                devfs_register_blockdev(devname, pi);
                printf("AHCI: Registered /dev/%s (port %u)\n", devname, pi);

                if (partition_scan(pi, &ptable) == 0 && ptable.count > 0) {
                    printf("PART: Found %d partition(s) on /dev/%s (%s)\n",
                           ptable.count, devname,
                           ptable.is_gpt ? "GPT" : "MBR");
                    for (pk = 0; pk < ptable.count; pk++) {
                        partname[0] = 's';
                        partname[1] = 'd';
                        partname[2] = (char)('a' + j);
                        if (ptable.parts[pk].part_number >= 10) {
                            partname[3] = '0' + (ptable.parts[pk].part_number / 10);
                            partname[4] = '0' + (ptable.parts[pk].part_number % 10);
                            partname[5] = '\0';
                        } else {
                            partname[3] = '0' + ptable.parts[pk].part_number;
                            partname[4] = '\0';
                        }
                        devfs_register_partition(partname, pi,
                                                 ptable.parts[pk].start_lba,
                                                 ptable.parts[pk].sector_count);
                        printf("PART: Registered /dev/%s (start=%llu, sectors=%llu)\n",
                               partname,
                               (unsigned long long)ptable.parts[pk].start_lba,
                               (unsigned long long)ptable.parts[pk].sector_count);
                    }
                }
                j++;
            }
        }
    } else {
        printf("AHCI SATA driver not available (no controller found)\n");
    }

    net_init();

    tls_init();
    {
        vfs_node_t *ca_node;
        ca_node = vfs_namei("/etc/ssl/certs/ca-certificates.crt");
        if (ca_node && ca_node->length > 0) {
            uint8_t *ca_buf;
            ca_buf = (uint8_t *)kmalloc(ca_node->length);
            if (ca_buf) {
                uint32_t rd;
                rd = vfs_read(ca_node, 0, ca_node->length, ca_buf);
                if (rd > 0) {
                    tls_load_ca_certs(ca_buf, rd);
                }
                kfree(ca_buf);
            }
        }
    }

    if (debug_boot_hw) terminal_writestring("About to execute STI...\n");
    asm volatile ("sti");
    if (debug_boot_hw) terminal_writestring("STI completed! Interrupts enabled.\n");

    kprint_enable();

    extern void watchdog_init(void);
    watchdog_init();

    if (debug_memory) printf("heap: verify before launching init\n");
    if (debug_memory) heap_verify();
    slab_gc();

    init_task = launch_user_path(cmdline_get_init(), 0);
    if (!init_task) {
        printf("init not found, retrying in 5 seconds...\n");
        sleep_ticks(5000);
        init_task = launch_user_path(cmdline_get_init(), 0);
    }
    if (debug_memory) printf("heap: verify after launch attempt\n");
    if (debug_memory) heap_verify();
    if (!init_task) {
        printf("FATAL: /sbin/init not found. System halted.\n");
        for (;;)
            asm volatile ("hlt");
    }

    if (debug_boot_hw) {
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

    sleep_ticks(50);

    yield();

    {
        extern void watchdog_kick(void);
        while (1) {
            watchdog_kick();
            task_deferred_work();
            asm volatile ("hlt");
        }
    }
}
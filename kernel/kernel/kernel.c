#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <lebirun/tty.h>
#include <lebirun/gdt.h>
#include <lebirun/idt.h>
#include <lebirun/pit.h>
#include <lebirun/keyboard.h>
#include <lebirun/mouse.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/debug.h>
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/syscall.h>
#include <lebirun/io.h>
#include <lebirun/initrd.h>
#include <lebirun/ramfs.h>
#include <lebirun/squashfs.h>
#include <lebirun/iso9660.h>
#include <lebirun/overlayfs.h>
#include <lebirun/framebuffer.h>
#include <lebirun/console.h>
#include <lebirun/vfs.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/partition.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/tls.h>
#include <lebirun/vring.h>
#include <lebirun/about.h>
#include <lebirun/panic.h>
#include <lebirun/kstack.h>
#include <lebirun/smp.h>
#include <lebirun/power.h>
#include <lebirun/cmdline.h>
#include <lebirun/rng.h>
#include <lebirun/multiboot2.h>
#include "launch_user.h"

#ifndef CONFIG_DEBUG_BOOT_VFS
#define CONFIG_DEBUG_BOOT_VFS 0
#endif
#ifndef CONFIG_DEBUG_BOOT_MODULES
#define CONFIG_DEBUG_BOOT_MODULES 0
#endif
#ifndef CONFIG_DEBUG_BOOT_HW
#define CONFIG_DEBUG_BOOT_HW 0
#endif

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
#if defined(CONFIG_DEBUG_BOOT_VFS)
bool debug_boot_vfs = CONFIG_DEBUG_BOOT_VFS ? true : false;
#else
bool debug_boot_vfs = false;
#endif
#if defined(CONFIG_DEBUG_BOOT_MODULES)
bool debug_boot_modules = CONFIG_DEBUG_BOOT_MODULES ? true : false;
#else
bool debug_boot_modules = false;
#endif
#if defined(CONFIG_DEBUG_BOOT_HW)
bool debug_boot_hw = CONFIG_DEBUG_BOOT_HW ? true : false;
#else
bool debug_boot_hw = false;
#endif

extern uint64_t boot_pml4[512] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt_low[512] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt_high[512] __attribute__((aligned(4096)));
extern uint64_t boot_pd_0[512] __attribute__((aligned(4096)));
extern uint64_t boot_pd_1[512] __attribute__((aligned(4096)));

extern uint32_t multiboot_magic;
extern uint64_t multiboot_ptr;

mutex_t print_lock;

extern task_t* current_task;

extern task_t* ready_queue_head;

void kernel_main(void) {
    uint64_t mb_phys;
    uint64_t mb_page;
    struct multiboot2_tag *tag;
    struct multiboot2_tag_module *tag_mod;
    uint32_t mb2_total_size;
    uint64_t mb_end_page;
    uint64_t pg;
    extern uint8_t unifont_psf_start[] __attribute__((weak));
    extern uint8_t unifont_psf_end[] __attribute__((weak));
    uintptr_t u_start;
    uintptr_t u_end;
    size_t unifont_size;
    extern int early_fb_valid;
    extern uint64_t early_fb_addr;
    extern uint32_t early_fb_width;
    extern uint32_t early_fb_height;
    extern uint32_t early_fb_pitch;
    extern uint8_t early_fb_bpp;
    extern uint8_t early_fb_type;
    extern char early_cmdline[];
    extern uint32_t early_mod_count;
    uint8_t *mbb;
    uint32_t i;
    uint64_t mod_start;
    uint64_t mod_end;
    uint64_t mod_size;
    uint64_t pstart;
    int vring_boot_enabled;
    uint8_t *mstart;
    uint32_t b;
    uint32_t mod_count;
    extern void procfs_init(void);
    extern void devfs_init(void);
    extern int devfs_register_blockdev(const char *name, uint32_t port_index);
    extern int devfs_register_cdrom(const char *name, uint32_t port_index);
    extern int devfs_register_partition(const char *name, uint32_t port_index,
                                        uint64_t start_lba, uint64_t sector_count);
    extern void devfs_register_initrd(void);
    extern void sysfs_init(void);
    extern void lke_init(void);
    extern void lke_autoload(void);
#if CONFIG_VIRT_VFL
    extern void vfl_init(void);
    extern void vfl_register_devfs(void);
#endif
    extern void ramfs_debug_check_root(const char *location);
    uint64_t cr3;
    uint64_t cr0;
    int mount_ret;
    int use_squashfs;
    overlay_context_t *overlay_ctx;
    vfs_node_t *squashfs_root;
    vfs_node_t *ramfs_upper;
    vfs_node_t *initrd_root;
    uint8_t master_mask;
    uint8_t slave_mask;
    ahci_port_t *port;
    int j;
    int sr_idx;
    uint32_t pi;
    char devname[8];
    char partname[16];
    partition_table_t ptable;
    int pk;
    task_t *init_task;
    squashfs_context_t *sqctx;
    task_t *t;
    task_t *start;
    const char *root_dev;
    vfs_node_t *ext4_root;
    int ahci_done;
    int devs_registered;
    struct multiboot2_tag_module *tag_mod_initrd;

    gdt_init();
    idt_init();

    terminal_initialize();


    init_mem_map(multiboot_magic, multiboot_ptr);

    pfa_init();

    heap_init();

    cmdline_parse(early_cmdline[0] ? early_cmdline : NULL);

    if (cmdline_get_text_mode()) {
        fb_init_textmode(fb_get_default_font_data(), 128, 16);
        console_reinit();
        terminal_replay_early_boot();
    } else if (early_fb_valid) {
        terminal_init_fb(early_fb_addr, early_fb_width,
                        early_fb_height, early_fb_pitch,
                        early_fb_bpp, early_fb_type);

        u_start = (uintptr_t)unifont_psf_start;
        u_end = (uintptr_t)unifont_psf_end;
        unifont_size = 0;
        if (u_end > u_start) unifont_size = (size_t)(u_end - u_start);
        if (unifont_size > 0) {
            terminal_load_psf_font(unifont_psf_start, unifont_size);
            terminal_compact_font(256);
            {
                uint64_t font_phys_start;
                uint64_t font_phys_end;
                font_phys_start = (uintptr_t)unifont_psf_start - KERNEL_VMA;
                font_phys_end = (uintptr_t)unifont_psf_end - KERNEL_VMA;
                pfa_reclaim_kernel_range(font_phys_start, font_phys_end);
            }
        }

        console_init();
        terminal_replay_early_boot();

        printf("FB: addr=0x%llX %ux%u pitch=%u bpp=%u type=%u\n",
               (unsigned long long)early_fb_addr,
               early_fb_width, early_fb_height,
               early_fb_pitch, early_fb_bpp, early_fb_type);
    } else {
        fb_init_textmode(fb_get_default_font_data(), 128, 16);
        console_reinit();
        terminal_replay_early_boot();
    }

    mod_count = early_mod_count;

    mb_phys = multiboot_ptr;
    mb_page = mb_phys & ~0xFFFULL;
    vmm_map_page(mb_page + KERNEL_VMA, mb_page, 0x003);
    if (((mb_phys + 8) & ~0xFFFULL) != mb_page)
        vmm_map_page(((mb_phys + 8) & ~0xFFFULL) + KERNEL_VMA, (mb_phys + 8) & ~0xFFFULL, 0x003);

    mb2_total_size = multiboot2_total_size((void *)(mb_phys + KERNEL_VMA));
    mb_end_page = ((mb_phys + mb2_total_size + 0xFFF) & ~0xFFFULL);
    for (pg = mb_page; pg < mb_end_page; pg += 0x1000)
        vmm_map_page(pg + KERNEL_VMA, pg, 0x003);

    if (debug_boot_modules) printf("MB2 info: total_size=%u mod_count=%u\n", mb2_total_size, mod_count);

    if (debug_boot_modules) {
        printf("MB2: first 32 bytes: ");
        mbb = (uint8_t *)(mb_phys + KERNEL_VMA);
        for (i = 0; i < 32; i++) {
            printf("%02X", mbb[i]);
            if (i % 4 == 3) printf(" ");
        }
        printf("\n");
    }

    ahci_done = 0;
    devs_registered = 0;
    if (mod_count > 0) {
        i = 0;
        for (tag = multiboot2_first_tag((void *)(mb_phys + KERNEL_VMA));
             tag->type != MULTIBOOT2_TAG_END;
             tag = multiboot2_next_tag(tag)) {
            if (tag->type != MULTIBOOT2_TAG_MODULE)
                continue;
            tag_mod = (struct multiboot2_tag_module *)tag;
            mod_start = tag_mod->mod_start;
            mod_end = tag_mod->mod_end;
            mod_size = mod_end - mod_start;
            if (debug_boot_modules)
                printf("MB2: module[%u]: phys 0x%016lX-0x%016lX (%lu bytes) cmdline=\"%s\"\n",
                       i, mod_start, mod_end, mod_size, tag_mod->cmdline);

            pstart = mod_start & ~0xFFFULL;
            for (pg = pstart; pg < mod_end; pg += 0x1000)
                vmm_map_page(pg + KERNEL_VMA, pg, 0x003);
            if (debug_boot_modules) {
                mstart = (uint8_t *)(mod_start + KERNEL_VMA);
                printf("MB2: first 16 bytes of module[%u]: ", i);
                for (b = 0; b < 16 && b < mod_size; b++) printf("%02X", mstart[b]);
                printf("\n");
            }
            i++;
        }

        vfs_init();
        if (debug_boot_vfs) printf("KERNEL: After vfs_init\n");
        initrd_vfs_register();
        if (debug_boot_vfs) printf("KERNEL: After initrd_vfs_register\n");
        ramfs_vfs_register();
        if (debug_boot_vfs) printf("KERNEL: After ramfs_vfs_register\n");
        squashfs_vfs_register();
        if (debug_boot_vfs) printf("KERNEL: After squashfs_vfs_register\n");
        iso9660_vfs_register();
        if (debug_boot_vfs) printf("KERNEL: After iso9660_vfs_register\n");
        overlayfs_vfs_register();
        if (debug_boot_vfs) printf("KERNEL: After overlayfs_vfs_register\n");
        tmpfs_vfs_register();

        use_squashfs = 0;
        squashfs_root = NULL;
        ramfs_upper = NULL;
        overlay_ctx = NULL;

        mount_ret = vfs_mount(NULL, "/", "ramfs");
        if (mount_ret == 0) {
            if (debug_boot_vfs) printf("KERNEL: Mounted ramfs as root\n");
        } else {
            printf("KERNEL: Failed to mount ramfs as root\n");
        }

        tag_mod = NULL;
        tag_mod_initrd = NULL;
        i = 0;
        for (tag = multiboot2_first_tag((void *)(mb_phys + KERNEL_VMA));
             tag->type != MULTIBOOT2_TAG_END;
             tag = multiboot2_next_tag(tag)) {
            if (tag->type == MULTIBOOT2_TAG_MODULE) {
                if (i == 0)
                    tag_mod = (struct multiboot2_tag_module *)tag;
                else if (i == 1)
                    tag_mod_initrd = (struct multiboot2_tag_module *)tag;
                i++;
            }
        }
        if (tag_mod) {
            squashfs_init(tag_mod->mod_start, tag_mod->mod_end);
            mount_ret = vfs_mount(NULL, "/squashfs", "squashfs");
            if (mount_ret == 0) {
                squashfs_root = vfs_namei("/squashfs");
                use_squashfs = 1;
            } else {
                printf("KERNEL: SquashFS mount failed\n");
            }
        }
        ramfs_create_dir("/var", 0755);
        ramfs_create_dir("/tmp", 0777);
        ramfs_create_dir("/home", 0755);
        ramfs_create_dir("/root", 0700);
        ramfs_create_dir("/root/.config", 0700);
        ramfs_create_dir("/root/.config/htop", 0700);
        ramfs_create_dir("/run", 0755);

        if (use_squashfs && squashfs_root) {
            ramfs_upper = vfs_get_root();
            overlay_ctx = overlayfs_create(squashfs_root, ramfs_upper);
            if (overlay_ctx && overlay_ctx->merged_root) {
                vfs_replace_mount_root("/", overlay_ctx->merged_root, NULL, NULL);
                printf("KERNEL: OverlayFS active: squashfs (lower) + ramfs (upper) on /\n");
            }
        } else {
            mount_ret = vfs_mount(NULL, "/ro", "initrd");
            if (mount_ret == 0) {
                initrd_root = vfs_namei("/ro");
                if (initrd_root) {
                    ramfs_upper = vfs_get_root();
                    overlay_ctx = overlayfs_create(initrd_root, ramfs_upper);
                    if (overlay_ctx && overlay_ctx->merged_root) {
                        vfs_replace_mount_root("/", overlay_ctx->merged_root, NULL, NULL);
                        printf("KERNEL: OverlayFS active: initrd (lower) + ramfs (upper) on /\n");
                    }
                }
            }
        }

        if (debug_boot_vfs) printf("BOOT: procfs_init...\n");
        procfs_init();
        if (debug_boot_vfs) printf("BOOT: devfs_init...\n");
        devfs_init();
        if (tag_mod_initrd) {
            multiboot_module_t tmp_mod;
            tmp_mod.mod_start = tag_mod_initrd->mod_start;
            tmp_mod.mod_end = tag_mod_initrd->mod_end;
            tmp_mod.cmdline = 0;
            tmp_mod.reserved = 0;
            initrd_init(1, (uint64_t)&tmp_mod - KERNEL_VMA);
        }
        devfs_register_initrd();
#if CONFIG_VIRT_VFL
        vfl_init();
        vfl_register_devfs();
#endif
        if (debug_boot_vfs) printf("BOOT: sysfs_init...\n");
        sysfs_init();
        if (cmdline_get_lke())
            lke_init();

        if (debug_boot_vfs) printf("BOOT: mounting /dev /proc /sys...\n");
        vfs_mount(NULL, "/dev", "devfs");
        vfs_mount(NULL, "/proc", "procfs");
        vfs_mount(NULL, "/sys", "sysfs");
        vfs_mount(NULL, "/tmp", "tmpfs");

        if (use_squashfs) {
            if (debug_boot_vfs) printf("BOOT: vfs_block_squashfs_access...\n");
            vfs_block_squashfs_access();
        }

        if (!use_squashfs) {
            if (debug_boot_vfs) printf("BOOT: ramfs_internalize_all...\n");
            ramfs_internalize_all();
        }

        sqctx = squashfs_get_context();
        if (sqctx && sqctx->base && sqctx->size > 0) {
            ramfs_create_dir("/boot", 0755);
            ramfs_create_file("/boot/rootfs.squashfs", 0444);
            ramfs_set_backing("/boot/rootfs.squashfs", sqctx->base, sqctx->size);
            printf("BOOT: Exported /boot/rootfs.squashfs (%u bytes, zero-copy)\n", sqctx->size);
        }

        if (debug_boot_vfs) printf("BOOT: boot file export done\n");

#if CONFIG_DRIVER_AHCI
        if (ahci_init() == 0)
            ahci_done = 1;
#endif

        if (cmdline_get_lke())
            lke_autoload();
    } else {
        root_dev = cmdline_get_root();
        if (root_dev) {
            printf("BOOT: Installed mode: root=%s\n", root_dev);

            vfs_init();
            initrd_vfs_register();
            ramfs_vfs_register();
            squashfs_vfs_register();
            overlayfs_vfs_register();
            tmpfs_vfs_register();

            mount_ret = vfs_mount(NULL, "/", "ramfs");
            if (mount_ret != 0) {
                printf("BOOT: Failed to mount ramfs as root\n");
            }

            procfs_init();
            devfs_init();
#if CONFIG_VIRT_VFL
            vfl_init();
            vfl_register_devfs();
#endif
            sysfs_init();
            if (cmdline_get_lke())
                lke_init();

            vfs_mount(NULL, "/dev", "devfs");

            ext4_init();
            ext4_vfs_register();

            ahci_done = 0;
#if CONFIG_DRIVER_AHCI
            if (ahci_init() == 0) {
                ahci_done = 1;
                printf("AHCI SATA driver initialized successfully\n");

                j = 0;
                sr_idx = 0;
                for (pi = 0; pi < AHCI_MAX_PORTS; pi++) {
                    port = ahci_get_port(pi);
                    if (port && port->type == AHCI_DEV_SATA) {
                        devname[0] = 's';
                        devname[1] = 'd';
                        devname[2] = (char)('a' + j);
                        devname[3] = '\0';
                        devfs_register_blockdev(devname, pi);
                        printf("AHCI: Registered /dev/%s (port %u)\n", devname, pi);

                        printf("AHCI: Calling partition_scan for port %u...\n", pi);
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
                    } else if (port && port->type == AHCI_DEV_SATAPI) {
                        devname[0] = 's';
                        devname[1] = 'r';
                        devname[2] = (char)('0' + sr_idx);
                        devname[3] = '\0';
                        devfs_register_cdrom(devname, pi);
                        printf("AHCI: Registered /dev/%s (port %u, SATAPI)\n", devname, pi);
                        sr_idx++;
                    }
                }
                devs_registered = 1;
            } else {
                printf("AHCI SATA driver not available\n");
            }
#else
            printf("AHCI SATA driver disabled\n");
#endif

            iso9660_vfs_register();

            ext4_root = NULL;
            if (ahci_done) {
                mount_ret = vfs_mount(root_dev, "/mnt", "ext4");
                if (mount_ret == 0) {
                    ext4_root = vfs_namei("/mnt");
                }
            }

            if (ext4_root) {
                vfs_replace_mount_root("/", ext4_root, root_dev, "ext4");
                vfs_remove_mount("/mnt");
                ext4_root->name[0] = '\0';
                printf("BOOT: ext4 root mounted from %s\n", root_dev);
            } else {
                printf("BOOT: FATAL: failed to mount ext4 root %s\n", root_dev);
            }

            vfs_mount(NULL, "/proc", "procfs");
            vfs_mount(NULL, "/sys", "sysfs");
            vfs_mount(NULL, "/tmp", "tmpfs");
            if (cmdline_get_lke())
                lke_autoload();
        } else {
            printf("No multiboot modules present (mod_count=%u)\n", mod_count);
        }
    }

    mutex_init(&print_lock);

    cr3 = read_cr3();
    cr0 = read_cr0();
    if (debug_boot_hw) {
        printf("CR3="); print_hex(cr3);
        printf(" CR0="); print_hex(cr0);
        printf("\n");
    }
    if (debug_boot_hw) printf("BOOT: pic_remap...\n");
    pic_remap();
    if (debug_boot_hw) printf("BOOT: kstack_init...\n");
    kstack_init();
    if (debug_boot_hw) printf("BOOT: init_tasks...\n");
    init_tasks();
    if (debug_boot_hw) printf("BOOT: smp_init...\n");
    smp_init();

    power_init();
    vring_init();
    kproc_init();
    kproc_print_init();
    vring_boot_enabled = 1;
    
    pit_init(1000);
    calibrate_pit();

    if (lapic_base) {
        lapic_timer_init(1000);
        ioapic_mask_irq(0);
    }

    rng_init();
#if CONFIG_DRIVER_PS2_KEYBOARD
    keyboard_init();
#endif
#if CONFIG_DRIVER_PS2_MOUSE
    mouse_init();
#endif
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

#if CONFIG_DRIVER_AHCI
    if (!ahci_done && ahci_init() == 0) {
        ahci_done = 1;
    } else if (!ahci_done) {
        printf("AHCI SATA driver not available (no controller found)\n");
    }
#endif

    if (ahci_done && !devs_registered) {
        ext4_init();
        ext4_vfs_register();

        j = 0;
        sr_idx = 0;
        for (pi = 0; pi < AHCI_MAX_PORTS; pi++) {
            port = ahci_get_port(pi);
            if (port && port->type == AHCI_DEV_SATA) {
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
            } else if (port && port->type == AHCI_DEV_SATAPI) {
                devname[0] = 's';
                devname[1] = 'r';
                devname[2] = (char)('0' + sr_idx);
                devname[3] = '\0';
                devfs_register_cdrom(devname, pi);
                printf("AHCI: Registered /dev/%s (port %u, SATAPI)\n", devname, pi);
                sr_idx++;
            }
        }
    }

#if CONFIG_DRIVER_NET
    net_init();
#else
    printf("NET: Network stack disabled\n");
#endif

    if (debug_boot_hw) terminal_writestring("BOOT: About to execute STI...\n");
    asm volatile ("sti");
    if (debug_boot_hw) terminal_writestring("BOOT: STI completed! Interrupts enabled.\n");
    if (vring_boot_enabled) {
        kprint_enable();
        extern void watchdog_init(void);
        watchdog_init();
    } else {
        printf("BOOT: kprint/watchdog skipped (bring-up fallback)\n");
    }

    if (debug_memory) printf("BOOT: heap: verify before launching init\n");
    if (debug_memory) heap_verify();
    slab_gc();
    console_writer_flush();
    kprint_flush();

    {
        const char *init_candidates[4];
        int ci;

        init_candidates[0] = cmdline_get_init();
        init_candidates[1] = "/init";
        init_candidates[2] = "/sbin/init";
        init_candidates[3] = "/bin/init";

        init_task = NULL;
        for (ci = 0; ci < 4; ci++) {
            if (!init_candidates[ci] || !init_candidates[ci][0]) continue;
            init_task = launch_user_path(init_candidates[ci], 0);
            if (init_task) {
                break;
            }
        }
    }
    if (!init_task) {
        printf("BOOT: init not found, retrying in 5 seconds...\n");
        sleep_ticks(5000);
        init_task = launch_user_path("/init", 0);
        if (!init_task) init_task = launch_user_path("/sbin/init", 0);
        if (!init_task) init_task = launch_user_path("/bin/init", 0);
    }
    if (debug_memory) printf("BOOT: heap: verify after launch attempt\n");
    if (debug_memory) heap_verify();
    if (!init_task) {
        kernel_panic("FATAL: no init executable found (/init, /sbin/init, /bin/init).", NULL);
    }

    if (debug_boot_hw) {
        t = ready_queue_head;
        printf("BOOT: Run queue: ");
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

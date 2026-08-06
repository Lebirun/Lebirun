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
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/kernel_syscall.h>
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
#if CONFIG_DRIVER_VIRTIO_VGA || CONFIG_DRIVER_VIRTIO_GPU_PCI
#include <lebirun/drivers/fb/virtio_gpu.h>
#endif
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
#include <lebirun/watchdog.h>
#include <lebirun/multiboot2.h>
#include <lebirun/security.h>
#include "launch_user.h"

extern uint64_t boot_pml4[512] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt_low[512] __attribute__((aligned(4096)));
extern uint64_t boot_pdpt_high[512] __attribute__((aligned(4096)));
extern uint64_t boot_pd_0[512] __attribute__((aligned(4096)));
extern uint8_t _kernel_phys_start[];

extern uint32_t multiboot_magic;
extern uint64_t multiboot_ptr;

mutex_t print_lock;

extern task_t* current_task;

static void kernel_reclaim_memory(uint8_t *section_start, uint8_t *section_end,
                                  int flush_cpus, int report) {
    uint64_t start;
    uint64_t end;
    uint64_t address;

    start = (uint64_t)(uintptr_t)section_start - KERNEL_VMA;
    end = (uint64_t)(uintptr_t)section_end - KERNEL_VMA;
    for (address = (uint64_t)(uintptr_t)section_start;
         address < (uint64_t)(uintptr_t)section_end;
         address += PAGE_SIZE) {
        vmm_unmap_page(address);
    }
    if (flush_cpus && smp_tlb_flush_all_sync() < 0) return;
    if (report) {
        pfa_reclaim_kernel_range(start, end);
    } else {
        pfa_reclaim_kernel_range_quiet(start, end);
    }
}

static void kernel_reclaim_early_init_memory(void) {
    extern uint8_t _kernel_early_init_start[];
    extern uint8_t _kernel_early_init_end[];

    kernel_reclaim_memory(_kernel_early_init_start,
                          _kernel_early_init_end, 0, 0);
}

static void kernel_reclaim_init_memory(void) {
    extern uint8_t _kernel_init_start[];
    extern uint8_t _kernel_init_end[];

    kernel_reclaim_memory(_kernel_init_start, _kernel_init_end, 1, 1);
}

static void kernel_reclaim_optional_init_memory(void) {
    extern uint8_t _kernel_init_optional_start[];
    extern uint8_t _kernel_init_optional_end[];
    extern int syscall_core_fallback_reclaimable(void);

    if (!klog_early_storage_reclaimable()) return;
    if (!syscall_core_fallback_reclaimable()) return;
    if (!console_fallback_reclaimable()) return;
    kernel_reclaim_memory(_kernel_init_optional_start,
                          _kernel_init_optional_end, 1, 1);
}

static void kernel_reclaim_boot_stack_memory(void) {
    extern uint8_t _kernel_boot_stack_start[];
    extern uint8_t _kernel_boot_stack_end[];

    kernel_reclaim_memory(_kernel_boot_stack_start,
                          _kernel_boot_stack_end, 1, 0);
}

static void kernel_reclaim_multiboot_memory(void) {
    extern uint8_t _kernel_multiboot_start[];
    extern uint8_t _kernel_multiboot_end[];
    uint64_t start;
    uint64_t end;
    uint64_t address;

    start = (uint64_t)(uintptr_t)_kernel_multiboot_start;
    end = (uint64_t)(uintptr_t)_kernel_multiboot_end;
    for (address = start; address < end; address += PAGE_SIZE) {
        vmm_unmap_page(address + KERNEL_VMA);
    }
    if (smp_tlb_flush_all_sync() < 0) return;
    pfa_reclaim_kernel_range_quiet(start, end);
}

static void KERNEL_INIT kernel_boot(void) {
    uint64_t mb_phys;
    uint64_t mb_page;
    struct multiboot2_tag *tag;
    struct multiboot2_tag_module *tag_mod;
    uint32_t mb2_total_size;
    uint64_t mb_end_page;
    uint64_t pg;
    extern uint8_t unifont_glyphs_start[] __attribute__((weak));
    extern uint8_t unifont_glyphs_end[] __attribute__((weak));
    uintptr_t u_start;
    uintptr_t u_end;
    size_t unifont_size;
    int font_loaded;
    extern int early_fb_valid;
    extern uint64_t early_fb_addr;
    extern uint32_t early_fb_width;
    extern uint32_t early_fb_height;
    extern uint32_t early_fb_pitch;
    extern uint8_t early_fb_bpp;
    extern uint8_t early_fb_type;
    extern char early_cmdline[];
    extern uint32_t early_mod_count;
    uint32_t i;
    uint64_t mod_start;
    uint64_t mod_end;
    uint64_t pstart;
    int vring_boot_enabled;
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
    extern void vfl_register_devfs(void);
#endif
    extern void ramfs_debug_check_root(const char *location);
    int mount_ret;
    int use_squashfs;
    overlay_context_t *overlay_ctx;
    vfs_node_t *squashfs_root;
    vfs_node_t *ramfs_upper;
    vfs_node_t *initrd_root;
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
    const char *root_dev;
    vfs_node_t *ext4_root;
    int ahci_done;
    int devs_registered;
    struct multiboot2_tag_module *tag_mod_initrd;

    gdt_init();
    idt_init();
    x86_security_enable();

    terminal_initialize();


    init_mem_map(multiboot_magic, multiboot_ptr);

    pfa_init();

    heap_init();

    cmdline_parse(early_cmdline[0] ? early_cmdline : NULL);

    if (cmdline_get_text_mode()) {
        fb_init_textmode(fb_get_default_font_data(), 128, 16);
        console_reinit();
    } else if (early_fb_valid && CONFIG_DRIVER_VGA) {
        terminal_init_fb(early_fb_addr, early_fb_width,
                        early_fb_height, early_fb_pitch,
                        early_fb_bpp, early_fb_type);

        console_init();

        KERNEL_INIT_LOG("FB: addr=0x%llX %ux%u pitch=%u bpp=%u type=%u\n",
               (unsigned long long)early_fb_addr,
               early_fb_width, early_fb_height,
               early_fb_pitch, early_fb_bpp, early_fb_type);
    } else {
        fb_init_textmode(fb_get_default_font_data(), 128, 16);
        console_reinit();
    }

#if CONFIG_DRIVER_VIRTIO_VGA || CONFIG_DRIVER_VIRTIO_GPU_PCI
    if (!cmdline_get_text_mode()) {
        virtio_gpu_init(early_fb_valid ? early_fb_width : 0,
                        early_fb_valid ? early_fb_height : 0);
    }
#endif

    u_start = (uintptr_t)unifont_glyphs_start;
    u_end = (uintptr_t)unifont_glyphs_end;
    unifont_size = 0;
    font_loaded = 0;
    if (u_end > u_start) unifont_size = (size_t)(u_end - u_start);
    if (!cmdline_get_text_mode() && fb_get()->addr && unifont_size > 0) {
        font_loaded = terminal_load_embedded_font(unifont_glyphs_start,
                                                  unifont_size) == 0;
        if (font_loaded) {
#if CONFIG_DRIVER_VIRTIO_VGA || CONFIG_DRIVER_VIRTIO_GPU_PCI
            if (virtio_gpu_is_available()) virtio_gpu_flush();
#endif
        }
    }

    kernel_reclaim_early_init_memory();

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

    ahci_done = 0;
    devs_registered = 0;
    if (mod_count > 0) {
        for (tag = multiboot2_first_tag((void *)(mb_phys + KERNEL_VMA));
             tag->type != MULTIBOOT2_TAG_END;
             tag = multiboot2_next_tag(tag)) {
            if (tag->type != MULTIBOOT2_TAG_MODULE)
                continue;
            tag_mod = (struct multiboot2_tag_module *)tag;
            mod_start = tag_mod->mod_start;
            mod_end = tag_mod->mod_end;

            pstart = mod_start & ~0xFFFULL;
            for (pg = pstart; pg < mod_end; pg += 0x1000)
                vmm_map_page(pg + KERNEL_VMA, pg, 0x003);
        }

        vfs_init();
        initrd_vfs_register();
        ramfs_vfs_register();
        squashfs_vfs_register();
        iso9660_vfs_register();
        overlayfs_vfs_register();
        tmpfs_vfs_register();

        use_squashfs = 0;
        squashfs_root = NULL;
        ramfs_upper = NULL;
        overlay_ctx = NULL;

        mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/"),
                              KERNEL_INIT_STRING("ramfs"));
        if (mount_ret == 0) {
            KERNEL_INIT_LOG("KERNEL: Mounted ramfs as root\n");
        } else {
            KERNEL_INIT_LOG("KERNEL: Failed to mount ramfs as root\n");
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
            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/squashfs"),
                                  KERNEL_INIT_STRING("squashfs"));
            if (mount_ret == 0) {
                squashfs_root = vfs_namei(KERNEL_INIT_STRING("/squashfs"));
                use_squashfs = 1;
            } else {
                KERNEL_INIT_LOG("KERNEL: SquashFS mount failed\n");
            }
        }
        ramfs_create_dir(KERNEL_INIT_STRING("/var"), 0755);
        ramfs_create_dir(KERNEL_INIT_STRING("/tmp"), 0777);
        ramfs_create_dir(KERNEL_INIT_STRING("/home"), 0755);
        ramfs_create_dir(KERNEL_INIT_STRING("/root"), 0700);
        ramfs_create_dir(KERNEL_INIT_STRING("/root/.config"), 0700);
        ramfs_create_dir(KERNEL_INIT_STRING("/root/.config/htop"), 0700);
        ramfs_create_dir(KERNEL_INIT_STRING("/run"), 0755);

        if (use_squashfs && squashfs_root) {
            ramfs_upper = vfs_get_root();
            overlay_ctx = overlayfs_create(squashfs_root, ramfs_upper);
            if (overlay_ctx && overlay_ctx->merged_root) {
                vfs_replace_mount_root(KERNEL_INIT_STRING("/"),
                                       overlay_ctx->merged_root, NULL, NULL);
                KERNEL_INIT_LOG("KERNEL: OverlayFS active: squashfs (lower) + ramfs (upper) on /\n");
            }
        } else {
            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/ro"),
                                  KERNEL_INIT_STRING("initrd"));
            if (mount_ret == 0) {
                initrd_root = vfs_namei(KERNEL_INIT_STRING("/ro"));
                if (initrd_root) {
                    ramfs_upper = vfs_get_root();
                    overlay_ctx = overlayfs_create(initrd_root, ramfs_upper);
                    if (overlay_ctx && overlay_ctx->merged_root) {
                        vfs_replace_mount_root(KERNEL_INIT_STRING("/"),
                                               overlay_ctx->merged_root,
                                               NULL, NULL);
                        KERNEL_INIT_LOG("KERNEL: OverlayFS active: initrd (lower) + ramfs (upper) on /\n");
                    }
                }
            }
        }

        procfs_init();
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
        vfl_register_devfs();
#endif
        sysfs_init();
        if (cmdline_get_lke())
            lke_init();

        vfs_mount(NULL, KERNEL_INIT_STRING("/dev"),
                  KERNEL_INIT_STRING("devfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/proc"),
                  KERNEL_INIT_STRING("procfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/sys"),
                  KERNEL_INIT_STRING("sysfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/tmp"),
                  KERNEL_INIT_STRING("tmpfs"));

        if (use_squashfs) {
            vfs_block_squashfs_access();
        }

        if (!use_squashfs) {
            ramfs_internalize_all();
        }

        sqctx = squashfs_get_context();
        if (sqctx && sqctx->base && sqctx->size > 0) {
            ramfs_create_dir(KERNEL_INIT_STRING("/boot"), 0755);
            ramfs_create_file(KERNEL_INIT_STRING("/boot/rootfs.squashfs"),
                              0444);
            ramfs_set_backing(KERNEL_INIT_STRING("/boot/rootfs.squashfs"),
                              sqctx->base, sqctx->size);
            KERNEL_INIT_LOG("BOOT: Exported /boot/rootfs.squashfs (%u bytes, zero-copy)\n", sqctx->size);
        }

#if CONFIG_DRIVER_AHCI
        if (ahci_init() == 0)
            ahci_done = 1;
#endif

        if (cmdline_get_lke())
            lke_autoload();
    } else {
        root_dev = cmdline_get_root();
        if (root_dev) {
            KERNEL_INIT_LOG("BOOT: Installed mode: root=%s\n", root_dev);

            vfs_init();
            initrd_vfs_register();
            ramfs_vfs_register();
            squashfs_vfs_register();
            overlayfs_vfs_register();
            tmpfs_vfs_register();

            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/"),
                                  KERNEL_INIT_STRING("ramfs"));
            if (mount_ret != 0) {
                KERNEL_INIT_LOG("BOOT: Failed to mount ramfs as root\n");
            }

            procfs_init();
            devfs_init();
#if CONFIG_VIRT_VFL
            vfl_register_devfs();
#endif
            sysfs_init();
            if (cmdline_get_lke())
                lke_init();

            vfs_mount(NULL, KERNEL_INIT_STRING("/dev"),
                      KERNEL_INIT_STRING("devfs"));

            ext4_init();
            ext4_vfs_register();

            ahci_done = 0;
#if CONFIG_DRIVER_AHCI
            if (ahci_init() == 0) {
                ahci_done = 1;
                KERNEL_INIT_LOG("AHCI SATA driver initialized successfully\n");

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
                        KERNEL_INIT_LOG("AHCI: Registered /dev/%s (port %u)\n", devname, pi);

                        if (partition_scan(pi, &ptable) == 0 && ptable.count > 0) {
                            KERNEL_INIT_LOG("PART: Found %d partition(s) on /dev/%s (%s)\n",
                                   ptable.count, devname,
                                   ptable.is_gpt ? KERNEL_INIT_STRING("GPT") :
                                                   KERNEL_INIT_STRING("MBR"));
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
                                KERNEL_INIT_LOG("PART: Registered /dev/%s (start=%llu, sectors=%llu)\n",
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
                        KERNEL_INIT_LOG("AHCI: Registered /dev/%s (port %u, SATAPI)\n", devname, pi);
                        sr_idx++;
                    }
                }
                devs_registered = 1;
            } else {
                KERNEL_INIT_LOG("AHCI SATA driver not available\n");
            }
#else
            KERNEL_INIT_LOG("AHCI SATA driver disabled\n");
#endif

            iso9660_vfs_register();

            ext4_root = NULL;
            if (ahci_done) {
                mount_ret = vfs_mount(root_dev, KERNEL_INIT_STRING("/mnt"),
                                      KERNEL_INIT_STRING("ext4"));
                if (mount_ret == 0) {
                    ext4_root = vfs_namei(KERNEL_INIT_STRING("/mnt"));
                }
            }

            if (ext4_root) {
                mount_ret = vfs_replace_mount_root(
                    KERNEL_INIT_STRING("/"), ext4_root, root_dev,
                    KERNEL_INIT_STRING("ext4"));
                if (mount_ret == 0) {
                    ext4_prepare_root_node(ext4_root);
                    vfs_remove_mount(KERNEL_INIT_STRING("/mnt"));
                    KERNEL_INIT_LOG("BOOT: ext4 root mounted from %s\n", root_dev);
                } else {
                    KERNEL_INIT_LOG("BOOT: FATAL: failed to replace root mount with %s\n", root_dev);
                }
            } else {
                KERNEL_INIT_LOG("BOOT: FATAL: failed to mount ext4 root %s\n", root_dev);
            }

            vfs_mount(NULL, KERNEL_INIT_STRING("/proc"),
                      KERNEL_INIT_STRING("procfs"));
            vfs_mount(NULL, KERNEL_INIT_STRING("/sys"),
                      KERNEL_INIT_STRING("sysfs"));
            vfs_mount(NULL, KERNEL_INIT_STRING("/tmp"),
                      KERNEL_INIT_STRING("tmpfs"));
            if (cmdline_get_lke())
                lke_autoload();
        } else {
            KERNEL_INIT_LOG("No multiboot modules present (mod_count=%u)\n", mod_count);
        }
    }

    pfa_release_multiboot_range(mb_page, mb_end_page);

    mutex_init(&print_lock);

    pic_remap();
    kstack_init();
    init_tasks();
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

#if CONFIG_DRIVER_AHCI
    if (!ahci_done && ahci_init() == 0) {
        ahci_done = 1;
    } else if (!ahci_done) {
        KERNEL_INIT_LOG("AHCI SATA driver not available (no controller found)\n");
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
                KERNEL_INIT_LOG("AHCI: Registered /dev/%s (port %u)\n", devname, pi);

                if (partition_scan(pi, &ptable) == 0 && ptable.count > 0) {
                    KERNEL_INIT_LOG("PART: Found %d partition(s) on /dev/%s (%s)\n",
                           ptable.count, devname,
                           ptable.is_gpt ? KERNEL_INIT_STRING("GPT") :
                                           KERNEL_INIT_STRING("MBR"));
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
                        KERNEL_INIT_LOG("PART: Registered /dev/%s (start=%llu, sectors=%llu)\n",
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
                KERNEL_INIT_LOG("AHCI: Registered /dev/%s (port %u, SATAPI)\n", devname, pi);
                sr_idx++;
            }
        }
    }

#if CONFIG_DRIVER_NET
    net_init();
#else
    KERNEL_INIT_LOG("NET: Network stack disabled\n");
#endif

    smp_enable_scheduling();
    asm volatile ("sti");
    if (pt_reclaim_low_identity() == 0)
        pfa_release_cold_low_memory(
            (uint64_t)(uintptr_t)_kernel_phys_start);
    if (vring_boot_enabled) {
        kprint_enable();
        watchdog_init();
    } else {
        KERNEL_INIT_LOG("BOOT: kprint/watchdog skipped (bring-up fallback)\n");
    }

    console_reclaim_unused();
    fb_reclaim_unused();
    slab_reclaim_empty();
    kstack_reclaim_unused();
    heap_reclaim_unused();
    console_writer_flush();
    kprint_flush();
    klog_reclaim_unused();
    {
        const char *init_candidates[4];
        int ci;

        init_candidates[0] = cmdline_get_init();
        init_candidates[1] = KERNEL_INIT_STRING("/init");
        init_candidates[2] = KERNEL_INIT_STRING("/sbin/init");
        init_candidates[3] = KERNEL_INIT_STRING("/bin/init");

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
        KERNEL_INIT_LOG("BOOT: init not found, retrying in 5 seconds...\n");
        sleep_ms(5000);
        init_task = launch_user_path(KERNEL_INIT_STRING("/init"), 0);
        if (!init_task)
            init_task = launch_user_path(KERNEL_INIT_STRING("/sbin/init"), 0);
        if (!init_task)
            init_task = launch_user_path(KERNEL_INIT_STRING("/bin/init"), 0);
    }
    if (!init_task) {
        kernel_panic(KERNEL_INIT_STRING(
            "FATAL: no init executable found (/init, /sbin/init, /bin/init)."),
            NULL);
    }
    watchdog_set_init_pid((int)init_task->pid);

}

static void kernel_idle_loop(void) {
    extern void watchdog_kick(void);

    while (1) {
        watchdog_kick();
        task_deferred_work();
        console_process_pending();
        asm volatile ("hlt");
    }
}

static void kernel_runtime(void) {
    asm volatile ("sti");
    kernel_reclaim_init_memory();
    kernel_reclaim_optional_init_memory();
    kernel_reclaim_boot_stack_memory();
    kernel_reclaim_multiboot_memory();
    heap_reclaim_unused();

    sleep_ticks(50);

    yield();

    kernel_idle_loop();
}

void KERNEL_INIT kernel_main(void) {
    uint8_t *runtime_stack;
    extern void kernel_switch_stack(uint64_t stack_top, void (*entry)(void));

    kernel_boot();
    runtime_stack = kstack_alloc();
    if (!runtime_stack) {
        kernel_reclaim_init_memory();
        heap_reclaim_unused();
        sleep_ticks(50);
        yield();
        kernel_idle_loop();
    }
    current_task->kernel_stack_base = runtime_stack;
    current_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    current_task->regs.rsp = (uint64_t)runtime_stack + KSTACK_RUNTIME_SIZE;
    tss_set_rsp0((uint64_t)runtime_stack + KSTACK_RUNTIME_SIZE);
    kernel_switch_stack((uint64_t)runtime_stack + KSTACK_RUNTIME_SIZE,
                        kernel_runtime);

    kernel_idle_loop();
}

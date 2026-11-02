#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <lebirun/tty.h>
#include <lebirun/gdt.h>
#include <lebirun/idt.h>
#include <lebirun/pit.h>
#include <lebirun/timekeeping.h>
#include <lebirun/keyboard.h>
#include <lebirun/mouse.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/kernel_syscall.h>
#include <lebirun/initrd.h>
#include <lebirun/ramfs.h>
#include <lebirun/squashfs.h>
#include <lebirun/iso9660.h>
#include <lebirun/overlayfs.h>
#include <lebirun/devfs.h>
#include <lebirun/framebuffer.h>
#include <lebirun/console.h>
#include <lebirun/vfs.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/drivers/nvme/nvme.h>
#include <lebirun/drivers/usb/xhci.h>
#include <lebirun/blkdev.h>
#include <lebirun/drivers/i2c/i2c-hid.h>
#if CONFIG_DRIVER_VIRTIO_VGA || CONFIG_DRIVER_VIRTIO_GPU_PCI
#include <lebirun/drivers/fb/virtio_gpu.h>
#endif
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/fs/fat.h>
#include <lebirun/fs/exfat.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/tls.h>
#include <lebirun/vring.h>
#include <lebirun/about.h>
#include <lebirun/panic.h>
#include <lebirun/kstack.h>
#include <lebirun/smp.h>
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
static int kernel_live_fs_used;

extern task_t* current_task;

static void KERNEL_INIT kernel_storage_init(void) {
#if CONFIG_DRIVER_AHCI
    ahci_init();
#endif
#if CONFIG_DRIVER_NVME
    nvme_init();
#endif
}

static void KERNEL_INIT kernel_add_disks(int *fs_ready, int force) {
    if (!blkdev_count() && !force) return;
    if (!*fs_ready) {
#if CONFIG_FS_EXT4
        ext4_init();
        ext4_vfs_register();
#endif
#if CONFIG_FS_FAT
        fat_vfs_register();
#endif
#if CONFIG_FS_EXFAT
        exfat_vfs_register();
#endif
        *fs_ready = 1;
    }
    devfs_add_blkdevs();
}

static void KERNEL_INIT kernel_reclaim_memory(
    uint8_t *section_start, uint8_t *section_end, int flush_cpus,
    int report) {
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

static void KERNEL_INIT kernel_reclaim_early_init_memory(void) {
    extern uint8_t _kernel_early_init_start[];
    extern uint8_t _kernel_early_init_end[];

    kernel_reclaim_memory(_kernel_early_init_start,
                          _kernel_early_init_end, 0, 0);
}

static void kernel_unmap_virtual_memory(uint8_t *section_start,
                                        uint8_t *section_end) {
    uint64_t address;

    for (address = (uint64_t)(uintptr_t)section_start;
         address < (uint64_t)(uintptr_t)section_end;
         address += PAGE_SIZE) {
        vmm_unmap_page(address);
    }
}

static void kernel_release_virtual_memory(uint8_t *section_start,
                                          uint8_t *section_end,
                                          int report) {
    uint64_t start;
    uint64_t end;

    start = (uint64_t)(uintptr_t)section_start - KERNEL_VMA;
    end = (uint64_t)(uintptr_t)section_end - KERNEL_VMA;
    if (report)
        pfa_reclaim_kernel_range(start, end);
    else
        pfa_reclaim_kernel_range_quiet(start, end);
}

static void kernel_reclaim_runtime_memory(void) {
    extern uint8_t _kernel_init_start[];
    extern uint8_t _kernel_init_end[];
    extern uint8_t _kernel_init_optional_start[];
    extern uint8_t _kernel_init_optional_end[];
    extern uint8_t _kernel_boot_stack_start[];
    extern uint8_t _kernel_boot_stack_end[];
    extern uint8_t _kernel_multiboot_start[];
    extern uint8_t _kernel_multiboot_end[];
    extern uint8_t _kernel_live_text_start[];
    extern uint8_t _kernel_live_text_end[];
    extern int syscall_core_fallback_reclaimable(void);
    uint64_t multiboot_start;
    uint64_t multiboot_end;
    uint64_t address;
    int reclaim_optional;

    reclaim_optional = klog_early_storage_reclaimable() &&
                       syscall_core_fallback_reclaimable() &&
                       console_fallback_reclaimable();
    multiboot_start = (uint64_t)(uintptr_t)_kernel_multiboot_start;
    multiboot_end = (uint64_t)(uintptr_t)_kernel_multiboot_end;

    kernel_unmap_virtual_memory(_kernel_init_start, _kernel_init_end);
    if (reclaim_optional)
        kernel_unmap_virtual_memory(_kernel_init_optional_start,
                                    _kernel_init_optional_end);
    if (!kernel_live_fs_used)
        kernel_unmap_virtual_memory(_kernel_live_text_start,
                                    _kernel_live_text_end);
    kernel_unmap_virtual_memory(_kernel_boot_stack_start,
                                _kernel_boot_stack_end);
    for (address = multiboot_start; address < multiboot_end;
         address += PAGE_SIZE) {
        vmm_unmap_page(address + KERNEL_VMA);
    }
    if (smp_tlb_flush_all_sync() < 0) return;

    kernel_release_virtual_memory(_kernel_init_start, _kernel_init_end, 1);
    if (reclaim_optional)
        kernel_release_virtual_memory(_kernel_init_optional_start,
                                      _kernel_init_optional_end, 1);
    if (!kernel_live_fs_used)
        kernel_release_virtual_memory(_kernel_live_text_start,
                                      _kernel_live_text_end, 1);
    kernel_release_virtual_memory(_kernel_boot_stack_start,
                                  _kernel_boot_stack_end, 0);
    pfa_reclaim_kernel_range_quiet(multiboot_start, multiboot_end);
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
    extern const char *early_cmdline;
    extern uint32_t early_mod_count;
    uint32_t i;
    uint64_t mod_start;
    uint64_t mod_end;
    uint64_t pstart;
    int vring_boot_enabled;
    uint32_t mod_count;
    extern void procfs_init(void);
    extern void sysfs_init(void);
#if CONFIG_KERNEL_LKE
    extern void lke_init(void);
    extern void lke_autoload(void);
#endif
#if CONFIG_VIRT_VFL
    extern void vfl_register_devfs(void);
#endif
    int mount_ret;
    int use_squashfs;
    overlay_context_t *overlay_ctx;
    vfs_node_t *squashfs_root;
    vfs_node_t *ramfs_upper;
    vfs_node_t *initrd_root;
    task_t *init_task;
    squashfs_context_t *sqctx;
    const char *root_dev;
#if CONFIG_FS_EXT4
    vfs_node_t *ext4_root;
#endif
    int fs_ready;
    int mem_map_relocated;
    struct multiboot2_tag_module *tag_mod_initrd;

    gdt_init();
    idt_init();
    x86_security_enable();

    terminal_initialize();


    init_mem_map(multiboot_magic, multiboot_ptr);

    pfa_init();

    mem_map_relocated = heap_init();

    cmdline_parse(early_cmdline && early_cmdline[0] ? early_cmdline : NULL);

    if (cmdline_get_text_mode()) {
        fb_init_textmode(fb_get_default_font_data(), 128, 16);
        console_reinit();
    } else if (early_fb_valid && CONFIG_DRIVER_VGA &&
               early_fb_type != MULTIBOOT2_FRAMEBUFFER_TEXT) {
        fb_remodeset_grub(&early_fb_width, &early_fb_height,
                          &early_fb_pitch, &early_fb_bpp,
                          cmdline_get_screen_width(),
                          cmdline_get_screen_height());
        terminal_init_fb(early_fb_addr, early_fb_width,
                        early_fb_height, early_fb_pitch,
                        early_fb_bpp, early_fb_type);

        console_init();
    } else if (CONFIG_DRIVER_VGA &&
               fb_boot_graphical(cmdline_get_screen_width(),
                                 cmdline_get_screen_height()) == 0) {
        console_init();
    } else {
        fb_init_textmode(fb_get_default_font_data(), 128, 16);
        console_reinit();
    }

#if CONFIG_DRIVER_VIRTIO_VGA || CONFIG_DRIVER_VIRTIO_GPU_PCI
    if (!cmdline_get_text_mode()) {
        if (early_fb_valid &&
            early_fb_type != MULTIBOOT2_FRAMEBUFFER_TEXT)
            virtio_gpu_init(early_fb_width, early_fb_height);
        else
            virtio_gpu_init(cmdline_get_screen_width(),
                            cmdline_get_screen_height());
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

    fs_ready = 0;
    if (mod_count > 0) {
        kernel_live_fs_used = 1;
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
#if CONFIG_FS_ISO9660
        iso9660_vfs_register();
#endif
        overlayfs_vfs_register();
        tmpfs_vfs_register();
        devpts_vfs_register();

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
#if CONFIG_KERNEL_LKE
        if (cmdline_get_lke())
            lke_init();
#endif

        vfs_mount(NULL, KERNEL_INIT_STRING("/dev"),
                  KERNEL_INIT_STRING("devfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/proc"),
                  KERNEL_INIT_STRING("procfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/sys"),
                  KERNEL_INIT_STRING("sysfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/tmp"),
                  KERNEL_INIT_STRING("tmpfs"));
        vfs_mount(NULL, KERNEL_INIT_STRING("/dev/shm"),
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

        kernel_storage_init();

#if CONFIG_KERNEL_LKE
        if (cmdline_get_lke())
            lke_autoload();
#endif
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
            devpts_vfs_register();

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
#if CONFIG_KERNEL_LKE
            if (cmdline_get_lke())
                lke_init();
#endif

            vfs_mount(NULL, KERNEL_INIT_STRING("/dev"),
                      KERNEL_INIT_STRING("devfs"));

#if CONFIG_FS_EXT4
            ext4_init();
            ext4_vfs_register();
#else
            KERNEL_INIT_LOG("ext4 filesystem disabled\n");
#endif
#if CONFIG_FS_FAT
            fat_vfs_register();
#endif
#if CONFIG_FS_EXFAT
            exfat_vfs_register();
#endif

            fs_ready = 1;
            kernel_storage_init();
            devfs_add_blkdevs();

#if CONFIG_FS_ISO9660
            iso9660_vfs_register();
#endif

#if CONFIG_FS_EXT4
            ext4_root = NULL;
            if (blkdev_count()) {
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
                    {
                        static const char *need_dirs[] = {
                            "dev", "proc", "sys", "tmp", "mnt", "run", NULL
                        };
                        vfs_node_t *new_root;
                        char abs_path[16];
                        int di;

                        new_root = vfs_namei(KERNEL_INIT_STRING("/"));
                        if (new_root) {
                            for (di = 0; need_dirs[di]; di++) {
                                snprintf(abs_path, sizeof(abs_path), "/%s",
                                         need_dirs[di]);
                                if (!vfs_namei(abs_path))
                                    vfs_mkdir(new_root, need_dirs[di],
                                              strcmp(need_dirs[di], "tmp") == 0 ? 01777 : 0755);
                            }
                        }
                    }
                    vfs_remove_mount(KERNEL_INIT_STRING("/dev"));
                    mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/dev"),
                                          KERNEL_INIT_STRING("devfs"));
                    if (mount_ret != 0)
                        KERNEL_INIT_LOG("BOOT: failed to mount devfs on /dev\n");
                } else {
                    KERNEL_INIT_LOG("BOOT: FATAL: failed to replace root mount with %s\n", root_dev);
                }
            } else {
                KERNEL_INIT_LOG("BOOT: FATAL: failed to mount ext4 root %s\n", root_dev);
            }
#else
            KERNEL_INIT_LOG("ext4 filesystem disabled\n");
#endif

            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/proc"),
                                      KERNEL_INIT_STRING("procfs"));
            if (mount_ret != 0)
                KERNEL_INIT_LOG("BOOT: failed to mount procfs on /proc\n");
            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/sys"),
                                      KERNEL_INIT_STRING("sysfs"));
            if (mount_ret != 0)
                KERNEL_INIT_LOG("BOOT: failed to mount sysfs on /sys\n");
            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/tmp"),
                                      KERNEL_INIT_STRING("tmpfs"));
            if (mount_ret != 0)
                KERNEL_INIT_LOG("BOOT: failed to mount tmpfs on /tmp\n");
            mount_ret = vfs_mount(NULL, KERNEL_INIT_STRING("/dev/shm"),
                                      KERNEL_INIT_STRING("tmpfs"));
            if (mount_ret != 0)
                KERNEL_INIT_LOG("BOOT: failed to mount tmpfs on /dev/shm\n");
#if CONFIG_KERNEL_LKE
            if (cmdline_get_lke())
                lke_autoload();
#endif
        } else {
            KERNEL_INIT_LOG("No multiboot modules present (mod_count=%u)\n", mod_count);
        }
    }

    if (mem_map_relocated)
        pfa_release_multiboot_range(mb_page, mb_end_page);

    mutex_init(&print_lock);

    pic_remap();
    kstack_init();
    init_tasks();
    smp_init();

    vring_init();
    kproc_init();
    kproc_print_init();
    vring_boot_enabled = 1;
    
    pit_init(1000);
    tsc_early_init();

    if (lapic_base) {
        lapic_timer_init(1000);
        ioapic_mask_irq(0);
        pit_stop();
    }

    rng_init();
#if CONFIG_DRIVER_PS2_KEYBOARD
    keyboard_init();
#endif
#if CONFIG_DRIVER_PS2_MOUSE
    mouse_init();
#endif
    syscall_init();

    kernel_storage_init();
    kernel_add_disks(&fs_ready, 0);

#if CONFIG_DRIVER_XHCI
    kernel_add_disks(&fs_ready, xhci_init() == 0 &&
                     CONFIG_DRIVER_USB_HOTPLUG && CONFIG_DRIVER_USB_STORAGE);
#endif

#if CONFIG_DRIVER_I2C_HID
    i2c_hid_init();
#endif

#if CONFIG_DRIVER_NET
    net_init();
#else
    KERNEL_INIT_LOG("NET: Network stack disabled\n");
#endif

    smp_enable_scheduling();
    asm volatile ("sti");
    tsc_init();
    if (pt_reclaim_low_identity() == 0)
        pfa_release_cold_low_memory(
            (uint64_t)(uintptr_t)_kernel_phys_start);
    if (vring_boot_enabled) {
        kprint_enable();
        watchdog_init();
    } else {
        KERNEL_INIT_LOG("BOOT: kprint/watchdog skipped (bring-up fallback)\n");
    }
    kprint_flush();
    console_reclaim_unused();
    fb_reclaim_unused();
    klog_reclaim_unused();
    kstack_reclaim_unused();
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
            init_task = launch_user_path_state(init_candidates[ci], 0,
                                               TASK_BLOCKED);
            if (init_task) {
                break;
            }
        }
    }
    if (!init_task) {
        KERNEL_INIT_LOG("BOOT: init not found, retrying in 5 seconds...\n");
        sleep_ms(5000);
        init_task = launch_user_path_state(KERNEL_INIT_STRING("/init"), 0,
                                           TASK_BLOCKED);
        if (!init_task)
            init_task = launch_user_path_state(
                KERNEL_INIT_STRING("/sbin/init"), 0, TASK_BLOCKED);
        if (!init_task)
            init_task = launch_user_path_state(
                KERNEL_INIT_STRING("/bin/init"), 0, TASK_BLOCKED);
    }
    if (!init_task) {
        kernel_panic(KERNEL_INIT_STRING(
            "FATAL: no init executable found (/init, /sbin/init, /bin/init)."),
            NULL);
    }
    watchdog_set_init_pid((int)init_task->pid);
    cmdline_reclaim_boot_values();
    kprint_flush();
    klog_reclaim_unused();

}

static void kernel_idle_loop(void) {
    extern void watchdog_kick(void);

    while (1) {
        watchdog_kick();
        task_deferred_work();
        console_process_pending();
        asm volatile ("cli");
        smp_tick_idle();
        asm volatile ("sti; hlt");
    }
}

static void kernel_start_init(void) {
    task_t *init_task;

    init_task = task_find(1);
    if (!init_task)
        kernel_panic("FATAL: init task disappeared.", NULL);
    wake_task(init_task);
    yield();
}

static void kernel_runtime(void) {
    asm volatile ("sti");
    kernel_reclaim_runtime_memory();
    heap_reclaim_unused();

    kernel_start_init();

    kernel_idle_loop();
}

void KERNEL_INIT kernel_main(void) {
    uint8_t *runtime_stack;
    extern void kernel_switch_stack(uint64_t stack_top, void (*entry)(void));

    kernel_boot();
    runtime_stack = kstack_alloc();
    if (!runtime_stack) {
        heap_reclaim_unused();
        kernel_start_init();
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

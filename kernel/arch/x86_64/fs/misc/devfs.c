#include <lebirun/vfs.h>
#include <lebirun/devfs.h>
#include <lebirun/pty.h>
#if CONFIG_VIRT_VFL
#include <lebirun/vfl.h>
#endif
#include <lebirun/mem_map.h>
#include <lebirun/console.h>
#include <lebirun/cmdline.h>
#include <lebirun/initrd.h>
#include <lebirun/blkdev.h>
#include <lebirun/partition.h>
#include <lebirun/mouse.h>
#include <lebirun/framebuffer.h>
#include <lebirun/task.h>
#include <lebirun/evdev.h>
#include <lebirun/rng.h>
#include <lebirun/vring.h>
#include <lebirun/mutex.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    vfs_node_t node;
    int in_use;
    uint64_t port_index;
    uint64_t start_lba;
    uint64_t sector_count;
    int is_partition;
    int is_cdrom;
} devfs_blockdev_t;

static devfs_blockdev_t **devfs_blockdevs;
static int devfs_blockdev_count;
static int devfs_blockdev_capacity;
static mutex_t devfs_blk_lock;

static int dev_initrd_registered;

static vfs_node_t devfs_root;
static vfs_node_t **dev_ttys;
static int dev_tty_count;

enum {
    DEVFS_NODE_NULL,
    DEVFS_NODE_ZERO,
    DEVFS_NODE_URANDOM,
    DEVFS_NODE_RANDOM,
    DEVFS_NODE_TTY,
    DEVFS_NODE_CONSOLE,
    DEVFS_NODE_STDIN,
    DEVFS_NODE_STDOUT,
    DEVFS_NODE_STDERR,
    DEVFS_NODE_FD,
    DEVFS_NODE_PTMX,
    DEVFS_NODE_PTS,
    DEVFS_NODE_FULL,
    DEVFS_NODE_MEM,
    DEVFS_NODE_KMEM,
    DEVFS_NODE_PORT,
    DEVFS_NODE_MICE,
    DEVFS_NODE_FB0,
    DEVFS_NODE_SHM,
    DEVFS_NODE_KMSG,
    DEVFS_NODE_HWRNG,
    DEVFS_NODE_COUNT
};

enum {
    DEVFS_OP_NULL,
    DEVFS_OP_ZERO,
    DEVFS_OP_RANDOM,
    DEVFS_OP_TTY,
    DEVFS_OP_LINK,
    DEVFS_OP_NONE,
    DEVFS_OP_OPEN,
    DEVFS_OP_FULL,
    DEVFS_OP_MICE,
    DEVFS_OP_FB,
    DEVFS_OP_KMSG,
    DEVFS_OP_HWRNG
};

typedef struct {
    const char *name;
    uint16_t flags;
    uint16_t mask;
    uint8_t operations;
} devfs_static_node_desc_t;

static vfs_node_t *devfs_nodes[DEVFS_NODE_COUNT];

static void devfs_open(vfs_node_t *node, uint64_t flags);
static void devfs_close(vfs_node_t *node);

static dirent_t devfs_boot_dirent;

static dirent_t *devfs_alloc_dirent(void) {
    vfs_dirent_release_name(&devfs_boot_dirent);
    memset(&devfs_boot_dirent, 0, sizeof(devfs_boot_dirent));
    return &devfs_boot_dirent;
}

static uint64_t dev_null_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static uint64_t dev_null_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

static uint64_t dev_zero_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    memset(buffer, 0, size);
    return size;
}

static uint64_t dev_zero_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

static uint64_t dev_full_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    memset(buffer, 0, size);
    return size;
}

static uint64_t dev_full_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer; (void)size;
    return (uint64_t)-28;
}

static uint64_t dev_urandom_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    rng_fill(buffer, size);
    return size;
}

static uint64_t dev_urandom_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    rng_add_entropy(buffer, size);
    return size;
}

static uint64_t dev_tty_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static uint64_t dev_tty_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    extern void console_write_to(int id, const char *buf, size_t len);
    extern int console_get_current(void);
    int con = console_get_current();
    console_write_to(con, (const char *)buffer, size);
    return size;
}

static uint64_t dev_ttyN_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    int tty_num;

    (void)offset; (void)size; (void)buffer;
    tty_num = node->inode;
    (void)tty_num;
    return 0;
}

static uint64_t dev_ttyN_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    int tty_num;
    extern void console_write_to(int id, const char *buf, size_t len);

    (void)offset;
    tty_num = node->inode;
    console_write_to(tty_num, (const char *)buffer, size);
    return size;
}

static uint64_t dev_mice_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint64_t generation;
    int nread;

    (void)node;
    (void)offset;
    while (!mouse_has_data()) {
        generation = descriptor_ready_generation();
        if (mouse_has_data()) break;
        descriptor_ready_wait(generation, UINT64_MAX);
    }
    nread = mouse_read(buffer, (uint32_t)size);
    return (uint64_t)nread;
}

static uint64_t dev_mice_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    (void)buffer;
    (void)size;
    return 0;
}

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

static uint64_t dev_fb0_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    framebuffer_t *fb;
    uint64_t fb_size;
    uint64_t avail;

    (void)node;
    fb = fb_get();
    if (!fb || !fb->addr)
        return 0;
    fb_size = fb->pitch * fb->height;
    if (offset >= fb_size)
        return 0;
    avail = fb_size - offset;
    if (size > avail)
        size = avail;
    memcpy(buffer, (uint8_t *)fb->addr + offset, size);
    return size;
}

static uint64_t dev_fb0_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    framebuffer_t *fb;
    uint64_t fb_size;
    uint64_t avail;

    (void)node;
    fb = fb_get();
    if (!fb || !fb->addr)
        return 0;
    fb_size = fb->pitch * fb->height;
    if (offset >= fb_size)
        return 0;
    avail = fb_size - offset;
    if (size > avail)
        size = avail;
    memcpy((uint8_t *)fb->addr + offset, buffer, size);
    return size;
}

int devfs_is_fb(vfs_node_t *node) {
    return node && node->read == dev_fb0_read;
}

static int dev_fb0_ioctl(vfs_node_t *node, unsigned long request, void *arg) {
    framebuffer_t *fb;
    struct fb_var_screeninfo *var;
    struct fb_fix_screeninfo *fix;
    uint32_t expected_bpp;

    (void)node;
    fb = fb_get();
    if (!fb)
        return -22;

    switch (request) {
    case FBIOGET_VSCREENINFO:
        var = (struct fb_var_screeninfo *)arg;
        memset(var, 0, sizeof(*var));
        var->xres = (uint32_t)fb->width;
        var->yres = (uint32_t)fb->height;
        var->xres_virtual = (uint32_t)fb->width;
        var->yres_virtual = (uint32_t)fb->height;
        var->bits_per_pixel = (uint32_t)fb->bpp;
        var->red.offset = 16;
        var->red.length = 8;
        var->green.offset = 8;
        var->green.length = 8;
        var->blue.offset = 0;
        var->blue.length = 8;
        var->transp.offset = 24;
        var->transp.length = 8;
        return 0;
    case FBIOGET_FSCREENINFO:
        fix = (struct fb_fix_screeninfo *)arg;
        memset(fix, 0, sizeof(*fix));
        strcpy(fix->id, "lebfb");
        fix->smem_start = fb->phys_addr;
        fix->smem_len = (uint32_t)(fb->pitch * fb->height);
        fix->type = FB_TYPE_PACKED_PIXELS;
        fix->visual = FB_VISUAL_TRUECOLOR;
        fix->line_length = (uint32_t)fb->pitch;
        return 0;
    case FBIOPUT_VSCREENINFO:
        var = (struct fb_var_screeninfo *)arg;
        if (!var) return -14;
        expected_bpp = (uint32_t)fb->bpp;
        if (var->xres != (uint32_t)fb->width ||
            var->yres != (uint32_t)fb->height ||
            var->xres_virtual != (uint32_t)fb->width ||
            var->yres_virtual != (uint32_t)fb->height ||
            var->xoffset != 0 || var->yoffset != 0 ||
            var->bits_per_pixel != expected_bpp)
            return -22;
        return 0;
    case FBIOGETCMAP:
    case FBIOPUTCMAP:
        return -25;
    default:
        return -25;
    }
}

static uint64_t dev_blockdev_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    devfs_blockdev_t *bdev;
    blkdev_t *dev;
    uint64_t lba;
    uint64_t abs_lba;
    uint64_t sector_count;
    uint8_t *tmp;
    uint64_t skip;
    uint64_t copy_len;

    bdev = (devfs_blockdev_t *)node->private_data;
    if (!bdev)
        return 0;

    dev = blkdev_get(bdev->port_index);
    if (!dev)
        return 0;

    lba = offset / 512;
    skip = offset % 512;
    if (size > 1024 * 1024 || skip > 511) return 0;
    if (skip > UINT64_MAX - size) return 0;
    if (skip + size > UINT64_MAX - 511) return 0;
    sector_count = (skip + size + 511) / 512;
    if (sector_count == 0 || sector_count > 2048) return 0;

    if (bdev->is_partition) {
        if (sector_count > bdev->sector_count) return 0;
        if (lba > bdev->sector_count - sector_count)
            return 0;
        abs_lba = bdev->start_lba + lba;
    } else {
        abs_lba = lba;
    }

    tmp = (uint8_t *)kmalloc(sector_count * 512);
    if (!tmp)
        return 0;

    if (blkdev_read(dev, abs_lba, sector_count, tmp) != 0) {
        kfree(tmp);
        return 0;
    }

    copy_len = size;
    memcpy(buffer, tmp + skip, copy_len);
    kfree(tmp);
    return copy_len;
}

static uint64_t dev_blockdev_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    devfs_blockdev_t *bdev;
    blkdev_t *dev;
    uint64_t lba;
    uint64_t abs_lba;
    uint64_t sector_count;
    uint8_t *tmp;
    uint64_t skip;
    int wr_ret;

    bdev = (devfs_blockdev_t *)node->private_data;
    if (!bdev)
        return 0;

    dev = blkdev_get(bdev->port_index);
    if (!dev)
        return 0;

    lba = offset / 512;
    skip = offset % 512;
    if (size > 1024 * 1024 || skip > 511) return 0;
    if (skip > UINT64_MAX - size) return 0;
    if (skip + size > UINT64_MAX - 511) return 0;
    sector_count = (skip + size + 511) / 512;
    if (sector_count == 0 || sector_count > 2048) return 0;

    if (bdev->is_partition) {
        if (sector_count > bdev->sector_count) return 0;
        if (lba > bdev->sector_count - sector_count)
            return 0;
        abs_lba = bdev->start_lba + lba;
    } else {
        abs_lba = lba;
    }

    if (skip == 0 && (size % 512) == 0 && sector_count <= 128) {
        wr_ret = blkdev_write(dev, abs_lba, sector_count, buffer);
        if (wr_ret != 0)
            return 0;
        return size;
    }

    tmp = (uint8_t *)kmalloc(sector_count * 512);
    if (!tmp)
        return 0;

    if (skip != 0 || (size % 512) != 0) {
        if (blkdev_read(dev, abs_lba, sector_count, tmp) != 0) {
            kfree(tmp);
            return 0;
        }
    }

    memcpy(tmp + skip, buffer, size);

    wr_ret = blkdev_write(dev, abs_lba, sector_count, tmp);
    if (wr_ret != 0) {
        kfree(tmp);
        return 0;
    }

    kfree(tmp);
    return size;
}

static uint64_t dev_cdrom_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    devfs_blockdev_t *bdev;
    blkdev_t *dev;
    uint64_t lba;
    uint64_t sector_count;
    uint8_t *tmp;
    uint64_t skip;
    uint64_t copy_len;

    bdev = (devfs_blockdev_t *)node->private_data;
    if (!bdev)
        return 0;

    dev = blkdev_get(bdev->port_index);
    if (!dev)
        return 0;

    lba = offset / dev->block_size;
    skip = offset % dev->block_size;
    if (size > 1024 * 1024) return 0;
    if (skip + size > UINT64_MAX - (dev->block_size - 1)) return 0;
    sector_count = (skip + size + dev->block_size - 1) / dev->block_size;
    if (sector_count == 0 || sector_count > 512) return 0;

    tmp = (uint8_t *)kmalloc(sector_count * dev->block_size);
    if (!tmp)
        return 0;

    if (dev->ops->read(dev, lba, sector_count, tmp) != 0) {
        kfree(tmp);
        return 0;
    }

    copy_len = size;
    memcpy(buffer, tmp + skip, copy_len);
    kfree(tmp);
    return copy_len;
}

static uint64_t dev_cdrom_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    return 0;
}

static uint64_t dev_stdio_link_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    const char *target;
    uint64_t len;
    uint64_t copy_len;

    (void)node;
    target = "/dev/tty";
    len = 8;
    if (offset >= len) return 0;
    copy_len = len - offset;
    if (copy_len > size) copy_len = size;
    memcpy(buffer, target + offset, copy_len);
    return copy_len;
}

static uint64_t dev_kmsg_read(vfs_node_t *node, uint64_t offset,
                              uint64_t size, uint8_t *buffer) {
    int len;

    (void)node;
    len = klog_snapshot(NULL, 0);
    if (len <= 0) return 0;
    if (offset >= (uint64_t)len) return 0;
    if (size > (uint64_t)len - offset) size = (uint64_t)len - offset;
    return (uint64_t)klog_snapshot_range((char *)buffer, (int)offset,
                                         (int)size);
}

static uint64_t dev_kmsg_write(vfs_node_t *node, uint64_t offset,
                               uint64_t size, uint8_t *buffer) {
    int written;

    (void)node;
    (void)offset;
    if (size == 0) return 0;
    if (size > 4096) size = 4096;
    written = klog_enqueue_raw((const char *)buffer, (size_t)size);
    if (written < 0) return (uint64_t)-5;
    return (uint64_t)written;
}

static uint64_t dev_hwrng_read(vfs_node_t *node, uint64_t offset,
                               uint64_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    rng_fill(buffer, size);
    return size;
}

static uint64_t dev_hwrng_write(vfs_node_t *node, uint64_t offset,
                                uint64_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    rng_add_entropy(buffer, size);
    return size;
}

static const devfs_static_node_desc_t devfs_node_descs[DEVFS_NODE_COUNT] = {
    { "null", VFS_CHARDEVICE, 0666, DEVFS_OP_NULL },
    { "zero", VFS_CHARDEVICE, 0666, DEVFS_OP_ZERO },
    { "urandom", VFS_CHARDEVICE, 0666, DEVFS_OP_RANDOM },
    { "random", VFS_CHARDEVICE, 0666, DEVFS_OP_RANDOM },
    { "tty", VFS_CHARDEVICE, 0666, DEVFS_OP_TTY },
    { "console", VFS_CHARDEVICE, 0600, DEVFS_OP_TTY },
    { "stdin", VFS_SYMLINK, 0777, DEVFS_OP_LINK },
    { "stdout", VFS_SYMLINK, 0777, DEVFS_OP_LINK },
    { "stderr", VFS_SYMLINK, 0777, DEVFS_OP_LINK },
    { "fd", VFS_DIRECTORY, 0755, DEVFS_OP_NONE },
    { "ptmx", VFS_CHARDEVICE, 0666, DEVFS_OP_OPEN },
    { "pts", VFS_DIRECTORY, 0755, DEVFS_OP_NONE },
    { "full", VFS_CHARDEVICE, 0666, DEVFS_OP_FULL },
    { "mem", VFS_CHARDEVICE, 0640, DEVFS_OP_OPEN },
    { "kmem", VFS_CHARDEVICE, 0640, DEVFS_OP_OPEN },
    { "port", VFS_CHARDEVICE, 0640, DEVFS_OP_OPEN },
    { "mice", VFS_CHARDEVICE, 0666, DEVFS_OP_MICE },
    { "fb0", VFS_CHARDEVICE, 0666, DEVFS_OP_FB },
    { "shm", VFS_DIRECTORY, 01777, DEVFS_OP_NONE },
    { "kmsg", VFS_CHARDEVICE, 0666, DEVFS_OP_KMSG },
    { "hwrng", VFS_CHARDEVICE, 0666, DEVFS_OP_HWRNG }
};

static void devfs_init_base_node(vfs_node_t *node, const devfs_static_node_desc_t *desc, int idx) {
    memset(node, 0, sizeof(vfs_node_t));
    node->flags = desc->flags;
    vfs_node_set_name(node, desc->name);
    node->mask = desc->mask;
    node->uid = 0;
    node->gid = 0;
    node->inode = idx + 1;
    switch (desc->operations) {
        case DEVFS_OP_NULL:
            node->read = dev_null_read;
            node->write = dev_null_write;
            break;
        case DEVFS_OP_ZERO:
            node->read = dev_zero_read;
            node->write = dev_zero_write;
            break;
        case DEVFS_OP_RANDOM:
            node->read = dev_urandom_read;
            node->write = dev_urandom_write;
            break;
        case DEVFS_OP_TTY:
            node->read = dev_tty_read;
            node->write = dev_tty_write;
            break;
        case DEVFS_OP_LINK:
            node->read = dev_stdio_link_read;
            break;
        case DEVFS_OP_FULL:
            node->read = dev_full_read;
            node->write = dev_full_write;
            break;
        case DEVFS_OP_MICE:
            node->read = dev_mice_read;
            node->write = dev_mice_write;
            break;
        case DEVFS_OP_FB:
            node->read = dev_fb0_read;
            node->write = dev_fb0_write;
            node->ioctl = dev_fb0_ioctl;
            break;
        case DEVFS_OP_KMSG:
            node->read = dev_kmsg_read;
            node->write = dev_kmsg_write;
            break;
        case DEVFS_OP_HWRNG:
            node->read = dev_hwrng_read;
            node->write = dev_hwrng_write;
            break;
        default:
            break;
    }
    if (desc->operations != DEVFS_OP_LINK &&
        desc->operations != DEVFS_OP_NONE) {
        node->open = devfs_open;
        node->close = devfs_close;
    }
    node->parent = &devfs_root;
    node->ref_count = 1;
    node->ptr = NULL;
    node->private_data = NULL;
}

static vfs_node_t *devfs_get_base_node_by_index(int idx) {
    vfs_node_t *node;

    if (idx < 0 || idx >= DEVFS_NODE_COUNT) return NULL;
    if (!devfs_nodes[idx]) {
        node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
        if (!node) return NULL;
        devfs_init_base_node(node, &devfs_node_descs[idx], idx);
        devfs_nodes[idx] = node;
    }
    return devfs_nodes[idx];
}

static vfs_node_t *devfs_get_base_node_by_name(const char *name) {
    int i;

    for (i = 0; i < DEVFS_NODE_COUNT; i++) {
        if (strcmp(name, devfs_node_descs[i].name) == 0) {
            return devfs_get_base_node_by_index(i);
        }
    }
    return NULL;
}

static void devfs_init_tty_node(vfs_node_t *tty, int idx) {
    char name[16];

    memset(tty, 0, sizeof(vfs_node_t));
    snprintf(name, sizeof(name), "tty%d", idx);
    tty->flags = VFS_CHARDEVICE;
    vfs_node_set_name(tty, name);
    tty->mask = 0620;
    tty->uid = 0;
    tty->gid = 0;
    tty->inode = idx;
    tty->read = dev_ttyN_read;
    tty->write = dev_ttyN_write;
    tty->open = devfs_open;
    tty->close = devfs_close;
    tty->parent = &devfs_root;
    tty->ref_count = 1;
    tty->ptr = NULL;
    tty->private_data = NULL;
}

static vfs_node_t *devfs_get_tty_node(int idx) {
    vfs_node_t *tty;

    if (idx < 0 || idx >= dev_tty_count || !dev_ttys) return NULL;
    if (!dev_ttys[idx]) {
        tty = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
        if (!tty) return NULL;
        devfs_init_tty_node(tty, idx);
        dev_ttys[idx] = tty;
    }
    return dev_ttys[idx];
}

static dirent_t *devfs_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;
    const char *entry_name;
    uint64_t base_count;
    int count;
    int i;

    (void)node;

    base_count = DEVFS_NODE_COUNT + 1;
#if CONFIG_VIRT_VFL
    base_count++;
#endif
    if (index < base_count) {
        d = devfs_alloc_dirent();
        if (!d) return NULL;
        if (index < DEVFS_NODE_COUNT) {
            entry_name = devfs_node_descs[index].name;
            d->type = devfs_node_descs[index].flags;
        } else if (index == DEVFS_NODE_COUNT) {
            entry_name = "input";
            d->type = VFS_DIRECTORY;
        } else {
            entry_name = "vfl";
            d->type = VFS_CHARDEVICE;
        }
        strcpy(d->name, entry_name);
        d->inode = index + 1;
        return d;
    }

    index -= base_count;
    if (index < (uint64_t)dev_tty_count) {
        d = devfs_alloc_dirent();
        if (!d) return NULL;
        snprintf(d->name, sizeof(d->name), "tty%lu", index);
        d->inode = 16 + index + 1;
        d->type = VFS_CHARDEVICE;
        return d;
    }
    
    index -= (uint64_t)dev_tty_count;
    if (index < (uint64_t)devfs_blockdev_count) {
        count = 0;
        d = NULL;
        mutex_lock(&devfs_blk_lock);
        for (i = 0; i < devfs_blockdev_capacity; i++) {
            if (!devfs_blockdevs[i]->in_use || count++ != (int)index) continue;
            d = devfs_alloc_dirent();
            if (d && vfs_dirent_set_name(
                    d, vfs_node_name(&devfs_blockdevs[i]->node)) != 0)
                d = NULL;
            if (d) {
                d->inode = 100 + i;
                d->type = VFS_BLOCKDEVICE;
            }
            break;
        }
        mutex_unlock(&devfs_blk_lock);
        if (i < devfs_blockdev_capacity) return d;
    }

    index -= (uint64_t)devfs_blockdev_count;
    if (dev_initrd_registered && index == 0) {
        d = devfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, "initrd");
        d->inode = 200;
        d->type = VFS_DIRECTORY;
        return d;
    }

    return NULL;
}

static vfs_node_t *devfs_finddir(vfs_node_t *node, const char *name) {
    int i;
    int idx;
    int digit;
    vfs_node_t *root;
    vfs_node_t *base_node;

    (void)node;
    
    base_node = devfs_get_base_node_by_name(name);
    if (base_node) return base_node;
    if (strcmp(name, "input") == 0) return evdev_get_input_dir();
#if CONFIG_VIRT_VFL
    if (strcmp(name, "vfl") == 0) return vfl_get_devfs_node();
#endif
    
    if (name[0] == 't' && name[1] == 't' && name[2] == 'y' &&
        name[3] >= '0' && name[3] <= '9') {
        idx = 0;
        for (i = 3; name[i] != '\0'; i++) {
            if (name[i] < '0' || name[i] > '9') return NULL;
            digit = name[i] - '0';
            if (idx > (INT32_MAX - digit) / 10) return NULL;
            idx = idx * 10 + digit;
        }
        return devfs_get_tty_node(idx);
    }
    
    mutex_lock(&devfs_blk_lock);
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (devfs_blockdevs[i]->in_use &&
            strcmp(vfs_node_name(&devfs_blockdevs[i]->node), name) == 0)
            break;
    }
    mutex_unlock(&devfs_blk_lock);
    if (i < devfs_blockdev_capacity) return &devfs_blockdevs[i]->node;

    if (dev_initrd_registered && strcmp(name, "initrd") == 0) {
        root = initrd_get_vfs_root();
        if (!root)
            return NULL;
        vfs_node_set_name(root, "initrd");
        root->parent = &devfs_root;
        return root;
    }

    return NULL;
}



static void devfs_open(vfs_node_t *node, uint64_t flags) {
    (void)node;
    (void)flags;
}

static void devfs_close(vfs_node_t *node) {
    (void)node;
}

static int devfs_unmount_impl(vfs_node_t *node) {
    (void)node;
    return 0;
}

static vfs_node_t *devfs_mount_impl(const char *device, const char *mountpoint) {
    (void)device;
    (void)mountpoint;
    return &devfs_root;
}

void KERNEL_INIT devfs_init(void) {
    int i;
    int tty_count_local;
    vfs_fs_type_t *fs_type;

    devfs_blockdev_capacity = 0;
    devfs_blockdevs = NULL;
    mutex_init(&devfs_blk_lock);
    for (i = 0; i < DEVFS_NODE_COUNT; i++) {
        devfs_nodes[i] = NULL;
    }

    memset(&devfs_root, 0, sizeof(vfs_node_t));
    devfs_root.flags = VFS_DIRECTORY;
    vfs_node_set_name(&devfs_root, "dev");
    devfs_root.mask = VFS_PERM_READ | VFS_PERM_EXEC;
    devfs_root.uid = 0;
    devfs_root.gid = 0;
    devfs_root.readdir = devfs_readdir;
    devfs_root.finddir = devfs_finddir;
    devfs_root.open = devfs_open;
    devfs_root.close = devfs_close;
    devfs_root.ref_count = 1;
    devfs_root.parent = NULL;
    devfs_root.ptr = NULL;
    devfs_root.private_data = NULL;

    evdev_get_input_dir()->parent = &devfs_root;
    evdev_get_event_node(0)->parent = evdev_get_input_dir();
    evdev_get_event_node(1)->parent = evdev_get_input_dir();

    tty_count_local = console_get_count();
    if (tty_count_local <= 0) tty_count_local = 1;
    dev_ttys = (vfs_node_t **)kmalloc(tty_count_local * sizeof(vfs_node_t *));
    if (!dev_ttys) {
        tty_count_local = 0;
    }
    dev_tty_count = tty_count_local;
    for (i = 0; i < dev_tty_count; i++) {
        dev_ttys[i] = NULL;
    }

    evdev_init();
    evdev_get_input_dir()->parent = &devfs_root;

    pty_init();

    fs_type = (vfs_fs_type_t *)kmalloc(sizeof(vfs_fs_type_t));

    if (!fs_type) {
        return;
    }

    memset(fs_type, 0, sizeof(vfs_fs_type_t));

    fs_type->name = "devfs";
    fs_type->mount = devfs_mount_impl;
    fs_type->unmount = devfs_unmount_impl;
    fs_type->next = NULL;

    vfs_register_fs(fs_type);
}

static devfs_blockdev_t *devfs_blockdev_add(const char *name, uint64_t index,
                                            uint64_t start, uint64_t sectors,
                                            int kind) {
    devfs_blockdev_t **list;
    devfs_blockdev_t *b;
    int i;

    b = NULL;
    mutex_lock(&devfs_blk_lock);
    for (i = 0; i < devfs_blockdev_capacity && !b; i++) {
        if (!devfs_blockdevs[i]->in_use &&
            devfs_blockdevs[i]->node.ref_count <= 1)
            b = devfs_blockdevs[i];
    }
    if (!b) {
        list = (devfs_blockdev_t **)krealloc(devfs_blockdevs,
            (uint64_t)(devfs_blockdev_capacity + 1) * sizeof(*list));
        if (list) devfs_blockdevs = list;
        b = list ? (devfs_blockdev_t *)kcalloc(1, sizeof(*b)) : NULL;
        if (b) devfs_blockdevs[devfs_blockdev_capacity++] = b;
    }
    if (b) {
        vfs_node_release_name(&b->node);
        memset(b, 0, sizeof(*b));
        b->port_index = index;
        b->start_lba = start;
        b->sector_count = sectors;
        b->is_partition = kind == 2;
        b->is_cdrom = kind == 1;
        b->node.flags = VFS_BLOCKDEVICE;
        b->node.mask = kind == 1 ? 0440 : 0660;
        b->node.inode = index;
        b->node.length = sectors * 512;
        b->node.read = kind == 1 ? dev_cdrom_read : dev_blockdev_read;
        b->node.write = kind == 1 ? dev_cdrom_write : dev_blockdev_write;
        b->node.open = devfs_open;
        b->node.close = devfs_close;
        b->node.parent = &devfs_root;
        b->node.ref_count = 1;
        b->node.private_data = b;
        if (vfs_node_set_name(&b->node, name) == 0) {
            b->in_use = 1;
            devfs_blockdev_count++;
        } else {
            b = NULL;
        }
    }
    mutex_unlock(&devfs_blk_lock);
    return b;
}

int BLKDEV_HOTPLUG devfs_register_blockdev(const char *name,
                                           uint64_t port_index) {
    blkdev_t *dev = blkdev_get(port_index);

    return devfs_blockdev_add(name, port_index, 0, dev ? dev->sectors : 0,
                              0) ? 0 : -1;
}

int BLKDEV_HOTPLUG devfs_register_cdrom(const char *name,
                                        uint64_t port_index) {
    return devfs_blockdev_add(name, port_index, 0, 0, 1) ? 0 : -1;
}

int devfs_register_partition(const char *name, uint64_t port_index,
                             uint64_t start_lba, uint64_t sector_count) {
    return devfs_blockdev_add(name, port_index, start_lba, sector_count,
                              2) ? 0 : -1;
}

static void devfs_drop_blkdevs(uint64_t port_index, int partitions_only) {
    int i;

    mutex_lock(&devfs_blk_lock);
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (devfs_blockdevs[i]->in_use &&
            devfs_blockdevs[i]->port_index == port_index &&
            (!partitions_only || devfs_blockdevs[i]->is_partition)) {
            devfs_blockdevs[i]->in_use = 0;
            devfs_blockdev_count--;
        }
    }
    mutex_unlock(&devfs_blk_lock);
}

void devfs_remove_blkdevs(uint64_t port_index) {
    devfs_drop_blkdevs(port_index, 0);
}

uint64_t devfs_get_partition_start(vfs_node_t *node) {
    devfs_blockdev_t *bdev;

    if (!node || !node->private_data)
        return 0;

    bdev = (devfs_blockdev_t *)node->private_data;
    return bdev->start_lba;
}

int devfs_is_partition(vfs_node_t *node) {
    devfs_blockdev_t *bdev;

    if (!node || !node->private_data)
        return 0;

    bdev = (devfs_blockdev_t *)node->private_data;
    return bdev->is_partition;
}

static void devfs_part_name(char *out, size_t size, const char *disk,
                            int number) {
    size_t len = strlen(disk);

    snprintf(out, size, "%s%s%d", disk,
             len && disk[len - 1] >= '0' && disk[len - 1] <= '9' ? "p" : "",
             number);
}

void BLKDEV_HOTPLUG devfs_add_blkdevs(void) {
    static uint64_t next;
    partition_table_t ptable;
    char partname[24];
    blkdev_t *dev;
    int pk;

    for (; next < blkdev_count(); next++) {
        dev = blkdev_get(next);
        if (!dev) continue;
        if (dev->type == BLKDEV_CDROM) {
            if (devfs_register_cdrom(dev->name, next) == 0)
                BLKDEV_LOG("BLK: Registered /dev/%s\n", dev->name);
            continue;
        }
        if (devfs_register_blockdev(dev->name, next) != 0) continue;
        BLKDEV_LOG("BLK: Registered /dev/%s (%llu sectors)\n", dev->name,
                   (unsigned long long)dev->sectors);
        if (partition_scan(next, &ptable) == 0 && ptable.count > 0) {
            BLKDEV_LOG("PART: Found %d partition(s) on /dev/%s (%s)\n",
                       ptable.count, dev->name, ptable.is_gpt ? "GPT" : "MBR");
            for (pk = 0; pk < ptable.count; pk++) {
                devfs_part_name(partname, sizeof(partname), dev->name,
                                ptable.parts[pk].part_number);
                devfs_register_partition(partname, next,
                                         ptable.parts[pk].start_lba,
                                         ptable.parts[pk].sector_count);
                BLKDEV_LOG("PART: Registered /dev/%s (start=%llu, sectors=%llu)\n",
                           partname,
                           (unsigned long long)ptable.parts[pk].start_lba,
                           (unsigned long long)ptable.parts[pk].sector_count);
            }
        }
        partition_table_free(&ptable);
    }
}

int devfs_rescan_partitions(const char *devname) {
    int i;
    int pk;
    int found;
    char partname[24];
    uint64_t port_index;
    partition_table_t ptable;

    port_index = 0;
    mutex_lock(&devfs_blk_lock);
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (devfs_blockdevs[i]->in_use && !devfs_blockdevs[i]->is_partition &&
            strcmp(vfs_node_name(&devfs_blockdevs[i]->node), devname) == 0) {
            port_index = devfs_blockdevs[i]->port_index;
            break;
        }
    }
    found = i < devfs_blockdev_capacity;
    mutex_unlock(&devfs_blk_lock);
    if (!found)
        return -1;

    devfs_drop_blkdevs(port_index, 1);

    if (partition_scan(port_index, &ptable) != 0 || ptable.count <= 0) {
        partition_table_free(&ptable);
        return 0;
    }

    for (pk = 0; pk < ptable.count; pk++) {
        devfs_part_name(partname, sizeof(partname), devname,
                        ptable.parts[pk].part_number);
        devfs_register_partition(partname, port_index,
                                 ptable.parts[pk].start_lba,
                                 ptable.parts[pk].sector_count);
    }

    found = ptable.count;
    partition_table_free(&ptable);
    return found;
}

void KERNEL_INIT devfs_register_initrd(void) {
    if (dev_initrd_registered)
        return;

    if (!initrd_get_base())
        return;

    dev_initrd_registered = 1;
}

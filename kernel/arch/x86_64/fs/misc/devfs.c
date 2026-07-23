#include <lebirun/vfs.h>
#include <lebirun/pty.h>
#if CONFIG_VIRT_VFL
#include <lebirun/vfl.h>
#endif
#include <lebirun/mem_map.h>
#include <lebirun/console.h>
#include <lebirun/cmdline.h>
#include <lebirun/initrd.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/partition.h>
#include <lebirun/mouse.h>
#include <lebirun/framebuffer.h>
#include <lebirun/task.h>
#include <lebirun/evdev.h>
#include <lebirun/rng.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DEVFS_INITIAL_BLOCKDEVS 1
#define DEVFS_MAX_BLOCKDEVS 32

typedef struct {
    vfs_node_t node;
    int in_use;
    uint64_t port_index;
    uint64_t start_lba;
    uint64_t sector_count;
    int is_partition;
    int is_cdrom;
} devfs_blockdev_t;

static devfs_blockdev_t *devfs_blockdevs;
static int devfs_blockdev_count;
static int devfs_blockdev_capacity;

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
    DEVFS_NODE_COUNT
};

typedef struct {
    const char *name;
    uint64_t flags;
    uint64_t mask;
    read_type_t read;
    write_type_t write;
    ioctl_type_t ioctl;
    open_type_t open;
    close_type_t close;
} devfs_static_node_desc_t;

static vfs_node_t *devfs_nodes[DEVFS_NODE_COUNT];

static void devfs_open(vfs_node_t *node, uint64_t flags);
static void devfs_close(vfs_node_t *node);

static int devfs_grow_blockdevs(void) {
    int new_cap;
    int i;
    devfs_blockdev_t *new_blockdevs;

    new_cap = devfs_blockdev_capacity * 2;
    if (new_cap <= 0) new_cap = DEVFS_INITIAL_BLOCKDEVS;
    if (new_cap > DEVFS_MAX_BLOCKDEVS) new_cap = DEVFS_MAX_BLOCKDEVS;
    if (new_cap <= devfs_blockdev_capacity) return -1;
    new_blockdevs = (devfs_blockdev_t *)krealloc(devfs_blockdevs, new_cap * sizeof(devfs_blockdev_t));
    if (!new_blockdevs) return -1;
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (new_blockdevs[i].in_use)
            new_blockdevs[i].node.private_data = &new_blockdevs[i];
    }
    for (i = devfs_blockdev_capacity; i < new_cap; i++) {
        memset(&new_blockdevs[i], 0, sizeof(devfs_blockdev_t));
    }
    devfs_blockdevs = new_blockdevs;
    devfs_blockdev_capacity = new_cap;
    return 0;
}

#define DEVFS_DIRENT_POOL_SIZE 4

static dirent_t *devfs_dirent_pool;
static volatile uint64_t devfs_dirent_index;

static dirent_t *devfs_alloc_dirent(void) {
    uint64_t idx;

    if (!devfs_dirent_pool) {
        devfs_dirent_pool = (dirent_t *)kmalloc(DEVFS_DIRENT_POOL_SIZE * sizeof(dirent_t));
        if (!devfs_dirent_pool)
            return NULL;
        memset(devfs_dirent_pool, 0, DEVFS_DIRENT_POOL_SIZE * sizeof(dirent_t));
        devfs_dirent_index = 0;
    }
    idx = devfs_dirent_index;
    devfs_dirent_index = (idx + 1) % DEVFS_DIRENT_POOL_SIZE;
    return &devfs_dirent_pool[idx];
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
    int nread;
    int guard;

    (void)node;
    (void)offset;
    guard = 0;
    while (!mouse_has_data() && guard < 10000) {
        waitq_add(mouse_get_waitq(), current_task);
        block_current();
        guard++;
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

static int dev_fb0_ioctl(vfs_node_t *node, unsigned long request, void *arg) {
    framebuffer_t *fb;
    struct fb_var_screeninfo *var;
    struct fb_fix_screeninfo *fix;

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
    case FBIOGETCMAP:
    case FBIOPUTCMAP:
        return 0;
    default:
        return -22;
    }
}

static uint64_t dev_blockdev_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    devfs_blockdev_t *bdev;
    ahci_port_t *port;
    uint64_t lba;
    uint64_t abs_lba;
    uint64_t sector_count;
    uint8_t *tmp;
    uint64_t skip;
    uint64_t copy_len;

    bdev = (devfs_blockdev_t *)node->private_data;
    if (!bdev)
        return 0;

    port = ahci_get_port(bdev->port_index);
    if (!port)
        return 0;

    lba = offset / 512;
    skip = offset % 512;
    sector_count = (skip + size + 511) / 512;

    if (bdev->is_partition) {
        if (lba + sector_count > bdev->sector_count)
            return 0;
        abs_lba = bdev->start_lba + lba;
    } else {
        abs_lba = lba;
    }

    tmp = (uint8_t *)kmalloc(sector_count * 512);
    if (!tmp)
        return 0;

    if (ahci_read_sectors(port, abs_lba, sector_count, tmp) != 0) {
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
    ahci_port_t *port;
    uint64_t lba;
    uint64_t abs_lba;
    uint64_t sector_count;
    uint8_t *tmp;
    uint64_t skip;
    int wr_ret;

    bdev = (devfs_blockdev_t *)node->private_data;
    if (!bdev)
        return 0;

    port = ahci_get_port(bdev->port_index);
    if (!port)
        return 0;

    lba = offset / 512;
    skip = offset % 512;
    sector_count = (skip + size + 511) / 512;

    if (bdev->is_partition) {
        if (lba + sector_count > bdev->sector_count)
            return 0;
        abs_lba = bdev->start_lba + lba;
    } else {
        abs_lba = lba;
    }

    if (skip == 0 && (size % 512) == 0 && sector_count <= 128) {
        wr_ret = ahci_write_sectors(port, abs_lba, sector_count, buffer);
        if (wr_ret != 0)
            return 0;
        return size;
    }

    tmp = (uint8_t *)kmalloc(sector_count * 512);
    if (!tmp)
        return 0;

    if (skip != 0 || (size % 512) != 0) {
        if (ahci_read_sectors(port, abs_lba, sector_count, tmp) != 0) {
            kfree(tmp);
            return 0;
        }
    }

    memcpy(tmp + skip, buffer, size);

    wr_ret = ahci_write_sectors(port, abs_lba, sector_count, tmp);
    if (wr_ret != 0) {
        kfree(tmp);
        return 0;
    }

    kfree(tmp);
    return size;
}

static uint64_t dev_cdrom_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    devfs_blockdev_t *bdev;
    ahci_port_t *port;
    uint64_t lba;
    uint64_t sector_count;
    uint8_t *tmp;
    uint64_t skip;
    uint64_t copy_len;

    bdev = (devfs_blockdev_t *)node->private_data;
    if (!bdev)
        return 0;

    port = ahci_get_port(bdev->port_index);
    if (!port)
        return 0;

    lba = offset / ATAPI_SECTOR_SIZE;
    skip = offset % ATAPI_SECTOR_SIZE;
    sector_count = (skip + size + ATAPI_SECTOR_SIZE - 1) / ATAPI_SECTOR_SIZE;

    tmp = (uint8_t *)kmalloc(sector_count * ATAPI_SECTOR_SIZE);
    if (!tmp)
        return 0;

    if (ahci_atapi_read(port, lba, sector_count, tmp) != 0) {
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

static const devfs_static_node_desc_t devfs_node_descs[DEVFS_NODE_COUNT] = {
    { "null", VFS_CHARDEVICE, 0666, dev_null_read, dev_null_write, NULL, devfs_open, devfs_close },
    { "zero", VFS_CHARDEVICE, 0666, dev_zero_read, dev_zero_write, NULL, devfs_open, devfs_close },
    { "urandom", VFS_CHARDEVICE, 0666, dev_urandom_read, dev_urandom_write, NULL, devfs_open, devfs_close },
    { "random", VFS_CHARDEVICE, 0666, dev_urandom_read, dev_urandom_write, NULL, devfs_open, devfs_close },
    { "tty", VFS_CHARDEVICE, 0666, dev_tty_read, dev_tty_write, NULL, devfs_open, devfs_close },
    { "console", VFS_CHARDEVICE, 0600, dev_tty_read, dev_tty_write, NULL, devfs_open, devfs_close },
    { "stdin", VFS_SYMLINK, 0777, dev_stdio_link_read, NULL, NULL, NULL, NULL },
    { "stdout", VFS_SYMLINK, 0777, dev_stdio_link_read, NULL, NULL, NULL, NULL },
    { "stderr", VFS_SYMLINK, 0777, dev_stdio_link_read, NULL, NULL, NULL, NULL },
    { "fd", VFS_DIRECTORY, 0755, NULL, NULL, NULL, NULL, NULL },
    { "ptmx", VFS_CHARDEVICE, 0666, NULL, NULL, NULL, devfs_open, devfs_close },
    { "pts", VFS_DIRECTORY, 0755, NULL, NULL, NULL, NULL, NULL },
    { "full", VFS_CHARDEVICE, 0666, dev_full_read, dev_full_write, NULL, devfs_open, devfs_close },
    { "mem", VFS_CHARDEVICE, 0640, NULL, NULL, NULL, devfs_open, devfs_close },
    { "kmem", VFS_CHARDEVICE, 0640, NULL, NULL, NULL, devfs_open, devfs_close },
    { "port", VFS_CHARDEVICE, 0640, NULL, NULL, NULL, devfs_open, devfs_close },
    { "mice", VFS_CHARDEVICE, 0666, dev_mice_read, dev_mice_write, NULL, devfs_open, devfs_close },
    { "fb0", VFS_CHARDEVICE, 0666, dev_fb0_read, dev_fb0_write, dev_fb0_ioctl, devfs_open, devfs_close }
};

static void devfs_init_base_node(vfs_node_t *node, const devfs_static_node_desc_t *desc, int idx) {
    memset(node, 0, sizeof(vfs_node_t));
    strcpy(node->name, desc->name);
    node->flags = desc->flags;
    node->mask = desc->mask;
    node->uid = 0;
    node->gid = 0;
    node->inode = idx + 1;
    node->read = desc->read;
    node->write = desc->write;
    node->ioctl = desc->ioctl;
    node->open = desc->open;
    node->close = desc->close;
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
    char name[8];

    memset(tty, 0, sizeof(vfs_node_t));
    name[0] = 't';
    name[1] = 't';
    name[2] = 'y';
    if (idx >= 10) {
        name[3] = '0' + (idx / 10);
        name[4] = '0' + (idx % 10);
        name[5] = '\0';
    } else {
        name[3] = '0' + idx;
        name[4] = '\0';
    }
    strcpy(tty->name, name);
    tty->flags = VFS_CHARDEVICE;
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

    (void)node;
    
    static const char *base_entries[] = {
        "null", "zero", "urandom", "random", "tty", "console",
        "stdin", "stdout", "stderr", "fd", "ptmx", "pts", "full",
        "mem", "kmem", "port", "mice", "fb0", "input"
#if CONFIG_VIRT_VFL
        , "vfl"
#endif
    };
    
    if (index < sizeof(base_entries)/sizeof(base_entries[0])) {
        d = devfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, base_entries[index]);
        d->inode = index + 1;
        d->type = VFS_CHARDEVICE;
        if (index == 6 || index == 7 || index == 8) d->type = VFS_SYMLINK;
        if (index == 9 || index == 11 || index == 18) d->type = VFS_DIRECTORY;
        return d;
    }
    
    index -= sizeof(base_entries)/sizeof(base_entries[0]);
    if (index < (uint64_t)dev_tty_count) {
        d = devfs_alloc_dirent();
        if (!d) return NULL;
        d->name[0] = 't';
        d->name[1] = 't';
        d->name[2] = 'y';
        if (index >= 10) {
            d->name[3] = '0' + (index / 10);
            d->name[4] = '0' + (index % 10);
            d->name[5] = '\0';
        } else {
            d->name[3] = '0' + index;
            d->name[4] = '\0';
        }
        d->inode = 16 + index + 1;
        d->type = VFS_CHARDEVICE;
        return d;
    }
    
    index -= (uint64_t)dev_tty_count;
    if (index < (uint64_t)devfs_blockdev_count) {
        int count;
        int i;
        count = 0;
        for (i = 0; i < devfs_blockdev_capacity; i++) {
            if (devfs_blockdevs[i].in_use) {
                if (count == (int)index) {
                    d = devfs_alloc_dirent();
                    if (!d) return NULL;
                    strcpy(d->name, devfs_blockdevs[i].node.name);
                    d->inode = 100 + i;
                    d->type = VFS_BLOCKDEVICE;
                    return d;
                }
                count++;
            }
        }
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
    vfs_node_t *root;
    vfs_node_t *base_node;

    (void)node;
    
    base_node = devfs_get_base_node_by_name(name);
    if (base_node) return base_node;
    if (strcmp(name, "input") == 0) return evdev_get_input_dir();
#if CONFIG_VIRT_VFL
    if (strcmp(name, "vfl") == 0) return vfl_get_devfs_node();
#endif
    
    if (name[0] == 't' && name[1] == 't' && name[2] == 'y' && name[3] >= '0' && name[3] <= '9') {
        idx = name[3] - '0';
        if (name[4] >= '0' && name[4] <= '9' && name[5] == '\0')
            idx = idx * 10 + (name[4] - '0');
        else if (name[4] != '\0')
            return NULL;
        return devfs_get_tty_node(idx);
    }
    
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (devfs_blockdevs[i].in_use && strcmp(devfs_blockdevs[i].node.name, name) == 0)
            return &devfs_blockdevs[i].node;
    }

    if (dev_initrd_registered && strcmp(name, "initrd") == 0) {
        root = initrd_get_vfs_root();
        if (!root)
            return NULL;
        strcpy(root->name, "initrd");
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

    devfs_dirent_index = 0;
    devfs_dirent_pool = NULL;
    devfs_blockdev_capacity = 0;
    devfs_blockdevs = NULL;
    for (i = 0; i < DEVFS_NODE_COUNT; i++) {
        devfs_nodes[i] = NULL;
    }

    memset(&devfs_root, 0, sizeof(vfs_node_t));
    strcpy(devfs_root.name, "dev");
    devfs_root.flags = VFS_DIRECTORY;
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

    tty_count_local = cmdline_get_consoles();
    if (tty_count_local <= 0) tty_count_local = 1;
    if (tty_count_local > NUM_CONSOLES) tty_count_local = NUM_CONSOLES;
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

int devfs_register_blockdev(const char *name, uint64_t port_index) {
    int i;
    int slot;
    size_t len;
    ahci_port_t *port;

    slot = -1;
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (!devfs_blockdevs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (devfs_grow_blockdevs() < 0)
            return -1;
        for (i = 0; i < devfs_blockdev_capacity; i++) {
            if (!devfs_blockdevs[i].in_use) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return -1;
    }

    memset(&devfs_blockdevs[slot], 0, sizeof(devfs_blockdev_t));
    devfs_blockdevs[slot].in_use = 1;
    devfs_blockdevs[slot].port_index = port_index;
    devfs_blockdevs[slot].start_lba = 0;
    devfs_blockdevs[slot].is_partition = 0;

    port = ahci_get_port(port_index);
    if (port && port->sector_count > 0) {
        devfs_blockdevs[slot].sector_count = port->sector_count;
        devfs_blockdevs[slot].node.length = (uint64_t)(port->sector_count * 512);
    } else {
        devfs_blockdevs[slot].sector_count = 0;
    }

    len = strlen(name);
    if (len >= VFS_MAX_NAME)
        len = VFS_MAX_NAME - 1;
    memcpy(devfs_blockdevs[slot].node.name, name, len);
    devfs_blockdevs[slot].node.name[len] = '\0';

    devfs_blockdevs[slot].node.flags = VFS_BLOCKDEVICE;
    devfs_blockdevs[slot].node.mask = 0660;
    devfs_blockdevs[slot].node.uid = 0;
    devfs_blockdevs[slot].node.gid = 0;
    devfs_blockdevs[slot].node.inode = port_index;
    devfs_blockdevs[slot].node.read = dev_blockdev_read;
    devfs_blockdevs[slot].node.write = dev_blockdev_write;
    devfs_blockdevs[slot].node.open = devfs_open;
    devfs_blockdevs[slot].node.close = devfs_close;
    devfs_blockdevs[slot].node.parent = &devfs_root;
    devfs_blockdevs[slot].node.ref_count = 1;
    devfs_blockdevs[slot].node.private_data = &devfs_blockdevs[slot];

    devfs_blockdev_count++;
    return 0;
}

int devfs_register_cdrom(const char *name, uint64_t port_index) {
    int i;
    int slot;
    size_t len;

    slot = -1;
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (!devfs_blockdevs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (devfs_grow_blockdevs() < 0)
            return -1;
        for (i = 0; i < devfs_blockdev_capacity; i++) {
            if (!devfs_blockdevs[i].in_use) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return -1;
    }

    memset(&devfs_blockdevs[slot], 0, sizeof(devfs_blockdev_t));
    devfs_blockdevs[slot].in_use = 1;
    devfs_blockdevs[slot].port_index = port_index;
    devfs_blockdevs[slot].start_lba = 0;
    devfs_blockdevs[slot].is_partition = 0;
    devfs_blockdevs[slot].is_cdrom = 1;
    devfs_blockdevs[slot].sector_count = 0;

    len = strlen(name);
    if (len >= VFS_MAX_NAME)
        len = VFS_MAX_NAME - 1;
    memcpy(devfs_blockdevs[slot].node.name, name, len);
    devfs_blockdevs[slot].node.name[len] = '\0';

    devfs_blockdevs[slot].node.flags = VFS_BLOCKDEVICE;
    devfs_blockdevs[slot].node.mask = 0440;
    devfs_blockdevs[slot].node.uid = 0;
    devfs_blockdevs[slot].node.gid = 0;
    devfs_blockdevs[slot].node.inode = port_index;
    devfs_blockdevs[slot].node.read = dev_cdrom_read;
    devfs_blockdevs[slot].node.write = dev_cdrom_write;
    devfs_blockdevs[slot].node.open = devfs_open;
    devfs_blockdevs[slot].node.close = devfs_close;
    devfs_blockdevs[slot].node.parent = &devfs_root;
    devfs_blockdevs[slot].node.ref_count = 1;
    devfs_blockdevs[slot].node.private_data = &devfs_blockdevs[slot];

    devfs_blockdev_count++;
    return 0;
}

int devfs_register_partition(const char *name, uint64_t port_index,
                             uint64_t start_lba, uint64_t sector_count) {
    int i;
    int slot;
    size_t len;

    slot = -1;
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (!devfs_blockdevs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (devfs_grow_blockdevs() < 0)
            return -1;
        for (i = 0; i < devfs_blockdev_capacity; i++) {
            if (!devfs_blockdevs[i].in_use) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return -1;
    }

    memset(&devfs_blockdevs[slot], 0, sizeof(devfs_blockdev_t));
    devfs_blockdevs[slot].in_use = 1;
    devfs_blockdevs[slot].port_index = port_index;
    devfs_blockdevs[slot].start_lba = start_lba;
    devfs_blockdevs[slot].sector_count = sector_count;
    devfs_blockdevs[slot].is_partition = 1;

    len = strlen(name);
    if (len >= VFS_MAX_NAME)
        len = VFS_MAX_NAME - 1;
    memcpy(devfs_blockdevs[slot].node.name, name, len);
    devfs_blockdevs[slot].node.name[len] = '\0';

    devfs_blockdevs[slot].node.flags = VFS_BLOCKDEVICE;
    devfs_blockdevs[slot].node.mask = 0660;
    devfs_blockdevs[slot].node.uid = 0;
    devfs_blockdevs[slot].node.gid = 0;
    devfs_blockdevs[slot].node.inode = port_index;
    devfs_blockdevs[slot].node.length = (uint64_t)(sector_count * 512);
    devfs_blockdevs[slot].node.read = dev_blockdev_read;
    devfs_blockdevs[slot].node.write = dev_blockdev_write;
    devfs_blockdevs[slot].node.open = devfs_open;
    devfs_blockdevs[slot].node.close = devfs_close;
    devfs_blockdevs[slot].node.parent = &devfs_root;
    devfs_blockdevs[slot].node.ref_count = 1;
    devfs_blockdevs[slot].node.private_data = &devfs_blockdevs[slot];

    devfs_blockdev_count++;
    return 0;
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

int devfs_rescan_partitions(const char *devname) {
    int i;
    int pk;
    int found;
    char drive_letter;
    char partname[16];
    uint64_t port_index;
    partition_table_t ptable;

    found = 0;
    port_index = 0;
    drive_letter = 0;
    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (devfs_blockdevs[i].in_use && !devfs_blockdevs[i].is_partition &&
            strcmp(devfs_blockdevs[i].node.name, devname) == 0) {
            port_index = devfs_blockdevs[i].port_index;
            drive_letter = devfs_blockdevs[i].node.name[2];
            found = 1;
            break;
        }
    }

    if (!found)
        return -1;

    for (i = 0; i < devfs_blockdev_capacity; i++) {
        if (devfs_blockdevs[i].in_use && devfs_blockdevs[i].is_partition &&
            devfs_blockdevs[i].port_index == port_index) {
            devfs_blockdevs[i].in_use = 0;
            devfs_blockdev_count--;
        }
    }

    if (partition_scan(port_index, &ptable) != 0 || ptable.count <= 0)
        return 0;

    for (pk = 0; pk < ptable.count; pk++) {
        partname[0] = 's';
        partname[1] = 'd';
        partname[2] = drive_letter;
        if (ptable.parts[pk].part_number >= 10) {
            partname[3] = '0' + (ptable.parts[pk].part_number / 10);
            partname[4] = '0' + (ptable.parts[pk].part_number % 10);
            partname[5] = '\0';
        } else {
            partname[3] = '0' + ptable.parts[pk].part_number;
            partname[4] = '\0';
        }
        devfs_register_partition(partname, port_index,
                                 ptable.parts[pk].start_lba,
                                 ptable.parts[pk].sector_count);
    }

    return ptable.count;
}

void KERNEL_INIT devfs_register_initrd(void) {
    if (dev_initrd_registered)
        return;

    if (!initrd_get_base())
        return;

    dev_initrd_registered = 1;
}

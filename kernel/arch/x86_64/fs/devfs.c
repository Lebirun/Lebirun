#include <kernel/vfs.h>
#include <kernel/pty.h>
#include <kernel/mem_map.h>
#include <kernel/console.h>
#include <kernel/cmdline.h>
#include <kernel/initrd.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/partition.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DEVFS_MAX_BLOCKDEVS 32

typedef struct {
    vfs_node_t node;
    int in_use;
    uint64_t port_index;
    uint64_t start_lba;
    uint64_t sector_count;
    int is_partition;
} devfs_blockdev_t;

static devfs_blockdev_t devfs_blockdevs[DEVFS_MAX_BLOCKDEVS];
static int devfs_blockdev_count;

static vfs_node_t dev_initrd_node;
static int dev_initrd_registered;

static vfs_node_t devfs_root;
static vfs_node_t dev_null;
static vfs_node_t dev_zero;
static vfs_node_t dev_urandom;
static vfs_node_t dev_random;
static vfs_node_t dev_tty;
static vfs_node_t dev_console;
static vfs_node_t dev_stdin;
static vfs_node_t dev_stdout;
static vfs_node_t dev_stderr;
static vfs_node_t dev_fd;
static vfs_node_t dev_ptmx;
static vfs_node_t dev_pts;
static vfs_node_t dev_full;
static vfs_node_t dev_mem;
static vfs_node_t dev_kmem;
static vfs_node_t dev_port;
static vfs_node_t dev_ttys[NUM_CONSOLES];

#define DEVFS_DIRENT_POOL_SIZE 4

static dirent_t devfs_dirent_pool[DEVFS_DIRENT_POOL_SIZE];
static volatile uint64_t devfs_dirent_index;

static dirent_t *devfs_alloc_dirent(void) {
    uint64_t idx;

    idx = devfs_dirent_index;
    devfs_dirent_index = (idx + 1) % DEVFS_DIRENT_POOL_SIZE;
    return &devfs_dirent_pool[idx];
}

static uint64_t lfsr_state = 0xACE1u;
static uint64_t entropy_pool[16];
static int entropy_index = 0;

static void add_entropy(uint64_t val) {
    entropy_pool[entropy_index] ^= val;
    entropy_index = (entropy_index + 1) & 15;
    lfsr_state ^= entropy_pool[entropy_index];
}

static uint64_t lfsr_rand(void) {
    uint64_t bit = ((lfsr_state >> 0) ^ (lfsr_state >> 2) ^ (lfsr_state >> 3) ^ (lfsr_state >> 5)) & 1;
    lfsr_state = (lfsr_state >> 1) | (bit << 31);
    lfsr_state ^= entropy_pool[(entropy_index + (lfsr_state & 15)) & 15];
    return lfsr_state;
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
    extern volatile uint64_t tick_count;
    uint64_t i;
    
    (void)node; (void)offset;
    
    add_entropy(tick_count);
    add_entropy((uint64_t)buffer);
    add_entropy(size);
    
    for (i = 0; i < size; i++) {
        buffer[i] = (uint8_t)(lfsr_rand() & 0xFF);
        if ((i & 15) == 0) {
            add_entropy(tick_count + i);
        }
    }
    return size;
}

static uint64_t dev_urandom_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint64_t i;
    
    (void)node; (void)offset;
    for (i = 0; i < size; i++) {
        add_entropy(buffer[i] | (i << 8));
    }
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

    if (skip != 0 || (size % 512) != 0) {
        if (ahci_read_sectors(port, abs_lba, sector_count, tmp) != 0) {
            kfree(tmp);
            return 0;
        }
    }

    memcpy(tmp + skip, buffer, size);

    if (ahci_write_sectors(port, abs_lba, sector_count, tmp) != 0) {
        kfree(tmp);
        return 0;
    }

    kfree(tmp);
    return size;
}

static uint64_t dev_initrd_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint8_t *base;
    uint64_t total;
    uint64_t avail;

    (void)node;
    base = initrd_get_base();
    total = initrd_get_size();
    if (!base || offset >= total)
        return 0;

    avail = total - offset;
    if (size > avail)
        size = avail;

    memcpy(buffer, base + offset, size);
    return size;
}

static uint64_t dev_initrd_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer; (void)size;
    return (uint64_t)-1;
}

static dirent_t *devfs_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;
    
    static const char *base_entries[] = {
        "null", "zero", "urandom", "random", "tty", "console",
        "stdin", "stdout", "stderr", "fd", "ptmx", "pts", "full",
        "mem", "kmem", "port"
    };
    
    if (index < sizeof(base_entries)/sizeof(base_entries[0])) {
        d = devfs_alloc_dirent();
        strcpy(d->name, base_entries[index]);
        d->inode = index + 1;
        d->type = VFS_CHARDEVICE;
        if (index == 9 || index == 11) d->type = VFS_DIRECTORY;
        return d;
    }
    
    index -= sizeof(base_entries)/sizeof(base_entries[0]);
    if (index < (uint64_t)cmdline_get_consoles()) {
        d = devfs_alloc_dirent();
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
    
    index -= (uint64_t)cmdline_get_consoles();
    if (index < (uint64_t)devfs_blockdev_count) {
        int count;
        int i;
        count = 0;
        for (i = 0; i < DEVFS_MAX_BLOCKDEVS; i++) {
            if (devfs_blockdevs[i].in_use) {
                if (count == (int)index) {
                    d = devfs_alloc_dirent();
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
        strcpy(d->name, "initrd");
        d->inode = 200;
        d->type = VFS_BLOCKDEVICE;
        return d;
    }

    return NULL;
}

static vfs_node_t *devfs_finddir(vfs_node_t *node, const char *name) {
    int i;

    (void)node;
    
    if (strcmp(name, "null") == 0) return &dev_null;
    if (strcmp(name, "zero") == 0) return &dev_zero;
    if (strcmp(name, "urandom") == 0) return &dev_urandom;
    if (strcmp(name, "random") == 0) return &dev_random;
    if (strcmp(name, "tty") == 0) return &dev_tty;
    if (strcmp(name, "console") == 0) return &dev_console;
    if (strcmp(name, "stdin") == 0) return &dev_stdin;
    if (strcmp(name, "stdout") == 0) return &dev_stdout;
    if (strcmp(name, "stderr") == 0) return &dev_stderr;
    if (strcmp(name, "fd") == 0) return &dev_fd;
    if (strcmp(name, "ptmx") == 0) return &dev_ptmx;
    if (strcmp(name, "pts") == 0) return &dev_pts;
    if (strcmp(name, "full") == 0) return &dev_full;
    if (strcmp(name, "mem") == 0) return &dev_mem;
    if (strcmp(name, "kmem") == 0) return &dev_kmem;
    if (strcmp(name, "port") == 0) return &dev_port;
    
    if (name[0] == 't' && name[1] == 't' && name[2] == 'y' && name[3] >= '0' && name[3] <= '9') {
        int idx = name[3] - '0';
        if (name[4] >= '0' && name[4] <= '9' && name[5] == '\0')
            idx = idx * 10 + (name[4] - '0');
        else if (name[4] != '\0')
            return NULL;
        if (idx >= 0 && idx < cmdline_get_consoles())
            return &dev_ttys[idx];
    }
    
    for (i = 0; i < DEVFS_MAX_BLOCKDEVS; i++) {
        if (devfs_blockdevs[i].in_use && strcmp(devfs_blockdevs[i].node.name, name) == 0)
            return &devfs_blockdevs[i].node;
    }

    if (dev_initrd_registered && strcmp(name, "initrd") == 0)
        return &dev_initrd_node;

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

void devfs_init(void) {
    int i;
    char name[8];
    vfs_fs_type_t *fs_type;

    devfs_dirent_index = 0;

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

    memset(&dev_null, 0, sizeof(vfs_node_t));
    strcpy(dev_null.name, "null");
    dev_null.flags = VFS_CHARDEVICE;
    dev_null.mask = 0666;
    dev_null.uid = 0;
    dev_null.gid = 0;
    dev_null.read = dev_null_read;
    dev_null.write = dev_null_write;
    dev_null.open = devfs_open;
    dev_null.close = devfs_close;
    dev_null.parent = &devfs_root;
    dev_null.ref_count = 1;
    dev_null.ptr = NULL;
    dev_null.private_data = NULL;

    memset(&dev_zero, 0, sizeof(vfs_node_t));
    strcpy(dev_zero.name, "zero");
    dev_zero.flags = VFS_CHARDEVICE;
    dev_zero.mask = 0666;
    dev_zero.uid = 0;
    dev_zero.gid = 0;
    dev_zero.read = dev_zero_read;
    dev_zero.write = dev_zero_write;
    dev_zero.open = devfs_open;
    dev_zero.close = devfs_close;
    dev_zero.parent = &devfs_root;
    dev_zero.ref_count = 1;
    dev_zero.ptr = NULL;
    dev_zero.private_data = NULL;

    memset(&dev_urandom, 0, sizeof(vfs_node_t));
    strcpy(dev_urandom.name, "urandom");
    dev_urandom.flags = VFS_CHARDEVICE;
    dev_urandom.mask = 0666;
    dev_urandom.uid = 0;
    dev_urandom.gid = 0;
    dev_urandom.read = dev_urandom_read;
    dev_urandom.write = dev_urandom_write;
    dev_urandom.open = devfs_open;
    dev_urandom.close = devfs_close;
    dev_urandom.parent = &devfs_root;
    dev_urandom.ref_count = 1;
    dev_urandom.ptr = NULL;
    dev_urandom.private_data = NULL;

    memset(&dev_random, 0, sizeof(vfs_node_t));
    strcpy(dev_random.name, "random");
    dev_random.flags = VFS_CHARDEVICE;
    dev_random.mask = 0666;
    dev_random.uid = 0;
    dev_random.gid = 0;
    dev_random.read = dev_urandom_read;
    dev_random.write = dev_urandom_write;
    dev_random.open = devfs_open;
    dev_random.close = devfs_close;
    dev_random.parent = &devfs_root;
    dev_random.ref_count = 1;
    dev_random.ptr = NULL;
    dev_random.private_data = NULL;

    memset(&dev_tty, 0, sizeof(vfs_node_t));
    strcpy(dev_tty.name, "tty");
    dev_tty.flags = VFS_CHARDEVICE;
    dev_tty.mask = 0666;
    dev_tty.uid = 0;
    dev_tty.gid = 0;
    dev_tty.read = dev_tty_read;
    dev_tty.write = dev_tty_write;
    dev_tty.open = devfs_open;
    dev_tty.close = devfs_close;
    dev_tty.parent = &devfs_root;
    dev_tty.ref_count = 1;
    dev_tty.ptr = NULL;
    dev_tty.private_data = NULL;

    memset(&dev_console, 0, sizeof(vfs_node_t));
    strcpy(dev_console.name, "console");
    dev_console.flags = VFS_CHARDEVICE;
    dev_console.mask = 0600;
    dev_console.uid = 0;
    dev_console.gid = 0;
    dev_console.read = dev_tty_read;
    dev_console.write = dev_tty_write;
    dev_console.open = devfs_open;
    dev_console.close = devfs_close;
    dev_console.parent = &devfs_root;
    dev_console.ref_count = 1;
    dev_console.ptr = NULL;
    dev_console.private_data = NULL;

    memset(&dev_stdin, 0, sizeof(vfs_node_t));
    strcpy(dev_stdin.name, "stdin");
    dev_stdin.flags = VFS_SYMLINK;
    dev_stdin.mask = 0777;
    dev_stdin.uid = 0;
    dev_stdin.gid = 0;
    dev_stdin.parent = &devfs_root;
    dev_stdin.ref_count = 1;
    dev_stdin.ptr = NULL;
    dev_stdin.private_data = NULL;

    memset(&dev_stdout, 0, sizeof(vfs_node_t));
    strcpy(dev_stdout.name, "stdout");
    dev_stdout.flags = VFS_SYMLINK;
    dev_stdout.mask = 0777;
    dev_stdout.uid = 0;
    dev_stdout.gid = 0;
    dev_stdout.parent = &devfs_root;
    dev_stdout.ref_count = 1;
    dev_stdout.ptr = NULL;
    dev_stdout.private_data = NULL;

    memset(&dev_stderr, 0, sizeof(vfs_node_t));
    strcpy(dev_stderr.name, "stderr");
    dev_stderr.flags = VFS_SYMLINK;
    dev_stderr.mask = 0777;
    dev_stderr.uid = 0;
    dev_stderr.gid = 0;
    dev_stderr.parent = &devfs_root;
    dev_stderr.ref_count = 1;
    dev_stderr.ptr = NULL;
    dev_stderr.private_data = NULL;

    memset(&dev_fd, 0, sizeof(vfs_node_t));
    strcpy(dev_fd.name, "fd");
    dev_fd.flags = VFS_DIRECTORY;
    dev_fd.mask = 0755;
    dev_fd.uid = 0;
    dev_fd.gid = 0;
    dev_fd.parent = &devfs_root;
    dev_fd.ref_count = 1;
    dev_fd.ptr = NULL;
    dev_fd.private_data = NULL;

    memset(&dev_ptmx, 0, sizeof(vfs_node_t));
    strcpy(dev_ptmx.name, "ptmx");
    dev_ptmx.flags = VFS_CHARDEVICE;
    dev_ptmx.mask = 0666;
    dev_ptmx.uid = 0;
    dev_ptmx.gid = 0;
    dev_ptmx.open = devfs_open;
    dev_ptmx.close = devfs_close;
    dev_ptmx.parent = &devfs_root;
    dev_ptmx.ref_count = 1;
    dev_ptmx.ptr = NULL;
    dev_ptmx.private_data = NULL;

    memset(&dev_pts, 0, sizeof(vfs_node_t));
    strcpy(dev_pts.name, "pts");
    dev_pts.flags = VFS_DIRECTORY;
    dev_pts.mask = 0755;
    dev_pts.uid = 0;
    dev_pts.gid = 0;
    dev_pts.parent = &devfs_root;
    dev_pts.ref_count = 1;
    dev_pts.ptr = NULL;
    dev_pts.private_data = NULL;

    memset(&dev_full, 0, sizeof(vfs_node_t));
    strcpy(dev_full.name, "full");
    dev_full.flags = VFS_CHARDEVICE;
    dev_full.mask = 0666;
    dev_full.uid = 0;
    dev_full.gid = 0;
    dev_full.read = dev_full_read;
    dev_full.write = dev_full_write;
    dev_full.open = devfs_open;
    dev_full.close = devfs_close;
    dev_full.parent = &devfs_root;
    dev_full.ref_count = 1;
    dev_full.ptr = NULL;
    dev_full.private_data = NULL;

    memset(&dev_mem, 0, sizeof(vfs_node_t));
    strcpy(dev_mem.name, "mem");
    dev_mem.flags = VFS_CHARDEVICE;
    dev_mem.mask = 0640;
    dev_mem.uid = 0;
    dev_mem.gid = 0;
    dev_mem.open = devfs_open;
    dev_mem.close = devfs_close;
    dev_mem.parent = &devfs_root;
    dev_mem.ref_count = 1;
    dev_mem.ptr = NULL;
    dev_mem.private_data = NULL;

    memset(&dev_kmem, 0, sizeof(vfs_node_t));
    strcpy(dev_kmem.name, "kmem");
    dev_kmem.flags = VFS_CHARDEVICE;
    dev_kmem.mask = 0640;
    dev_kmem.uid = 0;
    dev_kmem.gid = 0;
    dev_kmem.open = devfs_open;
    dev_kmem.close = devfs_close;
    dev_kmem.parent = &devfs_root;
    dev_kmem.ref_count = 1;
    dev_kmem.ptr = NULL;
    dev_kmem.private_data = NULL;

    memset(&dev_port, 0, sizeof(vfs_node_t));
    strcpy(dev_port.name, "port");
    dev_port.flags = VFS_CHARDEVICE;
    dev_port.mask = 0640;
    dev_port.uid = 0;
    dev_port.gid = 0;
    dev_port.open = devfs_open;
    dev_port.close = devfs_close;
    dev_port.parent = &devfs_root;
    dev_port.ref_count = 1;
    dev_port.ptr = NULL;
    dev_port.private_data = NULL;

    for (i = 0; i < 16; i++) {
        entropy_pool[i] = 0x5A5A5A5A ^ ((uint64_t)i * 0x13579BDF);
    }

    for (i = 0; i < cmdline_get_consoles(); i++) {
        memset(&dev_ttys[i], 0, sizeof(vfs_node_t));
        name[0] = 't';
        name[1] = 't';
        name[2] = 'y';
        if (i >= 10) {
            name[3] = '0' + (i / 10);
            name[4] = '0' + (i % 10);
            name[5] = '\0';
        } else {
            name[3] = '0' + i;
            name[4] = '\0';
        }
        strcpy(dev_ttys[i].name, name);
        dev_ttys[i].flags = VFS_CHARDEVICE;
        dev_ttys[i].mask = 0620;
        dev_ttys[i].uid = 0;
        dev_ttys[i].gid = 0;
        dev_ttys[i].inode = i;
        dev_ttys[i].read = dev_ttyN_read;
        dev_ttys[i].write = dev_ttyN_write;
        dev_ttys[i].open = devfs_open;
        dev_ttys[i].close = devfs_close;
        dev_ttys[i].parent = &devfs_root;
        dev_ttys[i].ref_count = 1;
        dev_ttys[i].ptr = NULL;
        dev_ttys[i].private_data = NULL;
    }

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
    for (i = 0; i < DEVFS_MAX_BLOCKDEVS; i++) {
        if (!devfs_blockdevs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0)
        return -1;

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

int devfs_register_partition(const char *name, uint64_t port_index,
                             uint64_t start_lba, uint64_t sector_count) {
    int i;
    int slot;
    size_t len;

    slot = -1;
    for (i = 0; i < DEVFS_MAX_BLOCKDEVS; i++) {
        if (!devfs_blockdevs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0)
        return -1;

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
    for (i = 0; i < DEVFS_MAX_BLOCKDEVS; i++) {
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

    for (i = 0; i < DEVFS_MAX_BLOCKDEVS; i++) {
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

void devfs_register_initrd(void) {
    if (dev_initrd_registered)
        return;

    if (!initrd_get_base())
        return;

    memset(&dev_initrd_node, 0, sizeof(vfs_node_t));
    strcpy(dev_initrd_node.name, "initrd");
    dev_initrd_node.flags = VFS_BLOCKDEVICE;
    dev_initrd_node.mask = 0440;
    dev_initrd_node.uid = 0;
    dev_initrd_node.gid = 0;
    dev_initrd_node.length = initrd_get_size();
    dev_initrd_node.read = dev_initrd_read;
    dev_initrd_node.write = dev_initrd_write;
    dev_initrd_node.open = devfs_open;
    dev_initrd_node.close = devfs_close;
    dev_initrd_node.parent = &devfs_root;
    dev_initrd_node.ref_count = 1;

    dev_initrd_registered = 1;
}

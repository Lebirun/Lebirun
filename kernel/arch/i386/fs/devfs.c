#include <kernel/vfs.h>
#include <kernel/pty.h>
#include <string.h>
#include <stdio.h>

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

static dirent_t dev_dirent;

static uint32_t lfsr_state = 0xACE1u;
static uint32_t entropy_pool[64];
static int entropy_index = 0;

static void add_entropy(uint32_t val) {
    entropy_pool[entropy_index] ^= val;
    entropy_index = (entropy_index + 1) & 63;
    lfsr_state ^= entropy_pool[entropy_index];
}

static uint32_t lfsr_rand(void) {
    uint32_t bit = ((lfsr_state >> 0) ^ (lfsr_state >> 2) ^ (lfsr_state >> 3) ^ (lfsr_state >> 5)) & 1;
    lfsr_state = (lfsr_state >> 1) | (bit << 31);
    lfsr_state ^= entropy_pool[(entropy_index + (lfsr_state & 63)) & 63];
    return lfsr_state;
}

static uint32_t dev_null_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static uint32_t dev_null_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

static uint32_t dev_zero_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    memset(buffer, 0, size);
    return size;
}

static uint32_t dev_zero_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

static uint32_t dev_full_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    memset(buffer, 0, size);
    return size;
}

static uint32_t dev_full_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer; (void)size;
    return (uint32_t)-28;
}

static uint32_t dev_urandom_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    
    extern volatile uint32_t tick_count;
    add_entropy(tick_count);
    add_entropy((uint32_t)buffer);
    add_entropy(size);
    
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)(lfsr_rand() & 0xFF);
        if ((i & 15) == 0) {
            add_entropy(tick_count + i);
        }
    }
    return size;
}

static uint32_t dev_urandom_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    for (uint32_t i = 0; i < size; i++) {
        add_entropy(buffer[i] | (i << 8));
    }
    return size;
}

static uint32_t dev_tty_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static uint32_t dev_tty_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    extern int console_write_to(int id, const char *buf, size_t len);
    extern int console_get_current(void);
    int con = console_get_current();
    console_write_to(con, (const char *)buffer, size);
    return size;
}

static dirent_t *devfs_readdir(vfs_node_t *node, uint32_t index) {
    (void)node;
    
    static const char *entries[] = {
        "null", "zero", "urandom", "random", "tty", "console",
        "stdin", "stdout", "stderr", "fd", "ptmx", "pts", "full",
        "mem", "kmem", "port"
    };
    
    if (index < sizeof(entries)/sizeof(entries[0])) {
        strcpy(dev_dirent.name, entries[index]);
        dev_dirent.inode = index + 1;
        dev_dirent.type = VFS_CHARDEVICE;
        if (index == 9 || index == 11) dev_dirent.type = VFS_DIRECTORY;
        return &dev_dirent;
    }
    
    return NULL;
}

static vfs_node_t *devfs_finddir(vfs_node_t *node, const char *name) {
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
    
    return NULL;
}

static vfs_node_t *devfs_mount(const char *device, const char *mountpoint) {
    (void)device; (void)mountpoint;
    return &devfs_root;
}

static int devfs_unmount(vfs_node_t *node) {
    (void)node;
    return 0;
}

static vfs_fs_type_t devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
    .unmount = devfs_unmount,
    .next = NULL
};

void devfs_init(void) {
    memset(&devfs_root, 0, sizeof(vfs_node_t));
    strcpy(devfs_root.name, "dev");
    devfs_root.flags = VFS_DIRECTORY;
    devfs_root.readdir = devfs_readdir;
    devfs_root.finddir = devfs_finddir;
    devfs_root.ref_count = 1;
    
    memset(&dev_null, 0, sizeof(vfs_node_t));
    strcpy(dev_null.name, "null");
    dev_null.flags = VFS_CHARDEVICE;
    dev_null.mask = 0666;
    dev_null.read = dev_null_read;
    dev_null.write = dev_null_write;
    dev_null.parent = &devfs_root;
    dev_null.ref_count = 1;
    
    memset(&dev_zero, 0, sizeof(vfs_node_t));
    strcpy(dev_zero.name, "zero");
    dev_zero.flags = VFS_CHARDEVICE;
    dev_zero.mask = 0666;
    dev_zero.read = dev_zero_read;
    dev_zero.write = dev_zero_write;
    dev_zero.parent = &devfs_root;
    dev_zero.ref_count = 1;
    
    memset(&dev_urandom, 0, sizeof(vfs_node_t));
    strcpy(dev_urandom.name, "urandom");
    dev_urandom.flags = VFS_CHARDEVICE;
    dev_urandom.mask = 0666;
    dev_urandom.read = dev_urandom_read;
    dev_urandom.write = dev_urandom_write;
    dev_urandom.parent = &devfs_root;
    dev_urandom.ref_count = 1;
    
    memset(&dev_random, 0, sizeof(vfs_node_t));
    strcpy(dev_random.name, "random");
    dev_random.flags = VFS_CHARDEVICE;
    dev_random.mask = 0666;
    dev_random.read = dev_urandom_read;
    dev_random.write = dev_urandom_write;
    dev_random.parent = &devfs_root;
    dev_random.ref_count = 1;
    
    memset(&dev_tty, 0, sizeof(vfs_node_t));
    strcpy(dev_tty.name, "tty");
    dev_tty.flags = VFS_CHARDEVICE;
    dev_tty.mask = 0666;
    dev_tty.read = dev_tty_read;
    dev_tty.write = dev_tty_write;
    dev_tty.parent = &devfs_root;
    dev_tty.ref_count = 1;
    
    memset(&dev_console, 0, sizeof(vfs_node_t));
    strcpy(dev_console.name, "console");
    dev_console.flags = VFS_CHARDEVICE;
    dev_console.mask = 0600;
    dev_console.read = dev_tty_read;
    dev_console.write = dev_tty_write;
    dev_console.parent = &devfs_root;
    dev_console.ref_count = 1;
    
    memset(&dev_stdin, 0, sizeof(vfs_node_t));
    strcpy(dev_stdin.name, "stdin");
    dev_stdin.flags = VFS_SYMLINK;
    dev_stdin.parent = &devfs_root;
    dev_stdin.ref_count = 1;
    
    memset(&dev_stdout, 0, sizeof(vfs_node_t));
    strcpy(dev_stdout.name, "stdout");
    dev_stdout.flags = VFS_SYMLINK;
    dev_stdout.parent = &devfs_root;
    dev_stdout.ref_count = 1;
    
    memset(&dev_stderr, 0, sizeof(vfs_node_t));
    strcpy(dev_stderr.name, "stderr");
    dev_stderr.flags = VFS_SYMLINK;
    dev_stderr.parent = &devfs_root;
    dev_stderr.ref_count = 1;
    
    memset(&dev_fd, 0, sizeof(vfs_node_t));
    strcpy(dev_fd.name, "fd");
    dev_fd.flags = VFS_DIRECTORY;
    dev_fd.parent = &devfs_root;
    dev_fd.ref_count = 1;
    
    memset(&dev_ptmx, 0, sizeof(vfs_node_t));
    strcpy(dev_ptmx.name, "ptmx");
    dev_ptmx.flags = VFS_CHARDEVICE;
    dev_ptmx.mask = 0666;
    dev_ptmx.parent = &devfs_root;
    dev_ptmx.ref_count = 1;
    
    memset(&dev_pts, 0, sizeof(vfs_node_t));
    strcpy(dev_pts.name, "pts");
    dev_pts.flags = VFS_DIRECTORY;
    dev_pts.mask = 0755;
    dev_pts.parent = &devfs_root;
    dev_pts.ref_count = 1;
    
    memset(&dev_full, 0, sizeof(vfs_node_t));
    strcpy(dev_full.name, "full");
    dev_full.flags = VFS_CHARDEVICE;
    dev_full.mask = 0666;
    dev_full.read = dev_full_read;
    dev_full.write = dev_full_write;
    dev_full.parent = &devfs_root;
    dev_full.ref_count = 1;
    
    memset(&dev_mem, 0, sizeof(vfs_node_t));
    strcpy(dev_mem.name, "mem");
    dev_mem.flags = VFS_CHARDEVICE;
    dev_mem.mask = 0640;
    dev_mem.parent = &devfs_root;
    dev_mem.ref_count = 1;
    
    memset(&dev_kmem, 0, sizeof(vfs_node_t));
    strcpy(dev_kmem.name, "kmem");
    dev_kmem.flags = VFS_CHARDEVICE;
    dev_kmem.mask = 0640;
    dev_kmem.parent = &devfs_root;
    dev_kmem.ref_count = 1;
    
    memset(&dev_port, 0, sizeof(vfs_node_t));
    strcpy(dev_port.name, "port");
    dev_port.flags = VFS_CHARDEVICE;
    dev_port.mask = 0640;
    dev_port.parent = &devfs_root;
    dev_port.ref_count = 1;
    
    for (int i = 0; i < 64; i++) {
        entropy_pool[i] = 0x5A5A5A5A ^ ((uint32_t)i * 0x13579BDF);
    }
    
    pty_init();
    
    vfs_register_fs(&devfs_type);
}

#include <kernel/initrd.h>
#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/vfs.h>
#include <kernel/ramfs.h>
#include <kernel/squashfs.h>
#include <kernel/debug.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static inline bool initrd_should_log(void) { return debug_initrd; }

static initrd_header_t *initrd_header = NULL; 
static initrd_file_header_t *file_headers = NULL;
static uint8_t *initrd_base = NULL;
static initrd_file_t *files = NULL;
static uint64_t file_count = 0;
static uint64_t initrd_version = 1;

static uint64_t initrd_mod0_phys_start = 0;
static uint64_t initrd_mod0_phys_end = 0;
static uint64_t initrd_mod1_phys_start = 0;
static uint64_t initrd_mod1_phys_end = 0;

initrd_fd_t fd_table[INITRD_MAX_FDS];

static vfs_node_t *initrd_vfs_nodes = NULL;
static uint64_t initrd_vfs_node_count = 0;
static vfs_node_t *initrd_vfs_root = NULL;
static dirent_t initrd_dirent;

static inline int initrd_is_root_parent(uint16_t parent_index) {
    if (parent_index == 0xFFFF) return 1;
    if (initrd_version <= 1 && parent_index == 0) return 1;
    return 0;
}

static void serial_printf_hex(uint64_t val) {
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        int nib = (val >> (i * 4)) & 0xF;
        buf[7-i] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
    }
    buf[8] = '\0';
    serial_puts(buf);
}

static void serial_printf_dec(uint64_t val) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (val == 0) { serial_putchar('0'); return; }
    while (val && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    serial_puts(&buf[i+1]);
}


void initrd_init(uint64_t mods_count, uint64_t mods_addr) {
    uint64_t mods_start_page;
    uint64_t mods_end_page;
    uint64_t mod_start_phys;
    uint64_t mod_end_phys;
    uint64_t mod_size;
    uint64_t start_page;
    uint64_t end_page;
    uint64_t total_bytes;
    if (initrd_should_log()) {
        serial_puts("\n=== INITRD INIT ===\n");
        serial_puts("mods_count="); serial_printf_dec(mods_count);
        serial_puts(" mods_addr=0x"); serial_printf_hex(mods_addr); serial_puts("\n");
        printf("\n=== INITRD INIT ===\nmods_count=%lu mods_addr=0x%016lX\n", mods_count, mods_addr);
        printf("INITRD: initrd_init called mods_count=%lu mods_addr=0x%016lX\n", mods_count, mods_addr);
    }

    if (mods_count == 0) {
        serial_puts("INITRD: No modules loaded!\n");
        printf("INITRD: No modules loaded\n");
        return;
    }

    mods_start_page = mods_addr & ~0xFFF;
    mods_end_page = (mods_addr + mods_count * sizeof(multiboot_module_t) + 0xFFF) & ~0xFFF;
    if (initrd_should_log()) printf("INITRD: Mapping multiboot array phys 0x%016lX - 0x%016lX\n", mods_start_page, mods_end_page);
    for (uint64_t phys = mods_start_page; phys < mods_end_page; phys += 0x1000) {
        uint64_t virt = phys + KERNEL_VMA;
        vmm_map_page(virt, phys, 0x003);
    }

    multiboot_module_t *mod = (multiboot_module_t *)(mods_addr + KERNEL_VMA);
    if (initrd_should_log()) printf("INITRD: Using module at virtual 0x%016lX (phys 0x%016lX - 0x%016lX)\n", (uint64_t)mod, (uint64_t)mod->mod_start, (uint64_t)mod->mod_end);
    
    mod_start_phys = mod->mod_start;
    mod_end_phys = mod->mod_end;
    mod_size = mod_end_phys - mod_start_phys;

    initrd_mod0_phys_start = mod_start_phys;
    initrd_mod0_phys_end = mod_end_phys;
    
    if (initrd_should_log()) printf("INITRD: Module at phys 0x%016lX - 0x%016lX (%lu bytes) cmdline=0x%016lX\n", 
           mod_start_phys, mod_end_phys, mod_size, mod->cmdline);

    if (mod_size < sizeof(initrd_header_t)) {
        printf("INITRD: Module too small to contain header (%u bytes)\n", mod_size);
        return;
    }

    start_page = mod_start_phys & ~0xFFF;
    end_page = (mod_end_phys + 0xFFF) & ~0xFFF;
    if (initrd_should_log()) printf("INITRD: Mapping phys 0x%016lX - 0x%016lX into kernel space\n", start_page, end_page);
    for (uint64_t phys = start_page; phys < end_page; phys += 0x1000) {
        uint64_t virt = phys + KERNEL_VMA;
        vmm_map_page(virt, phys, 0x003);
    }

    initrd_base = (uint8_t *)(mod_start_phys + KERNEL_VMA);

    if (initrd_should_log()) {
        serial_puts("INITRD: First 64 bytes: ");
        printf("INITRD: First 64 bytes of module: ");
        for (uint64_t i = 0; i < 64 && i < mod_size; i++) {
            char hex[3];
            uint8_t b = initrd_base[i];
            hex[0] = (b >> 4) < 10 ? '0' + (b >> 4) : 'A' + (b >> 4) - 10;
            hex[1] = (b & 0xF) < 10 ? '0' + (b & 0xF) : 'A' + (b & 0xF) - 10;
            hex[2] = '\0';
            serial_puts(hex);
            printf("%02X", initrd_base[i]);
            if (i % 4 == 3) { serial_putchar(' '); printf(" "); }
        }
        serial_puts("\n");
        printf("\n");
    }

    initrd_header = (initrd_header_t *)initrd_base;

    if (initrd_header->magic != INITRD_MAGIC) {
        serial_puts("INITRD: Invalid magic! got=0x"); serial_printf_hex(initrd_header->magic);
        serial_puts(" expected=0x"); serial_printf_hex(INITRD_MAGIC); serial_puts("\n");
        printf("INITRD: Invalid magic (got 0x%08lX, expected 0x%08lX)\n", 
               initrd_header->magic, INITRD_MAGIC);
        return;
    }
    if (initrd_should_log()) serial_puts("INITRD: Magic OK\n");

    initrd_version = initrd_header->version;
    if (initrd_version == 0) initrd_version = 1;

    file_count = initrd_header->num_entries;
    if (initrd_should_log()) {
        serial_puts("INITRD: Found "); serial_printf_dec(file_count); serial_puts(" files (version ");
        serial_printf_dec(initrd_version); serial_puts(")\n");
        printf("INITRD: Found %u files (version %u)\n", file_count, initrd_version);
    }

    file_headers = (initrd_file_header_t *)(initrd_base + sizeof(initrd_header_t));

    if ((uint8_t*)file_headers + file_count * sizeof(initrd_file_header_t) > initrd_base + mod_size) {
        printf("INITRD: File header array overruns module (headers end beyond module)\n");
        return;
    }

    if (initrd_should_log()) {
        printf("INITRD: Headers OK - listing headers:\n");
        for (uint64_t i = 0; i < file_count; i++) {
            char tmpname[65];
            memcpy(tmpname, file_headers[i].name, 64);
            tmpname[64] = '\0';
            printf("  header[%u]: name='%s' off=%u len=%u type=%u perm=0x%02X parent=%u\n", 
                   i, tmpname, file_headers[i].offset, file_headers[i].length,
                   file_headers[i].type, file_headers[i].permissions, file_headers[i].parent_index);
        }
    }

    files = (initrd_file_t *)kmalloc(file_count * sizeof(initrd_file_t));
    if (!files) {
        printf("INITRD: Failed to allocate file array\n");
        return;
    }

    total_bytes = 0;

    for (uint64_t i = 0; i < file_count; i++) {
        uint64_t hdr_off;
        uint64_t len;
        memcpy(files[i].name, file_headers[i].name, 64);
        files[i].length = file_headers[i].length;
        files[i].type = file_headers[i].type;
        files[i].permissions = file_headers[i].permissions;
        files[i].parent_index = file_headers[i].parent_index;
        files[i].uid = file_headers[i].uid;
        files[i].gid = file_headers[i].gid;

        hdr_off = file_headers[i].offset;
        len = file_headers[i].length;

        if (files[i].type == INITRD_TYPE_FILE) {
            if (hdr_off + len > mod_size) {
                printf("INITRD: File %u (%s) has out-of-bounds offset/length (off=%u len=%u mod_size=%u)\n", i, file_headers[i].name, hdr_off, len, mod_size);
                kfree(files);
                files = NULL;
                file_count = 0;
                return;
            }

            files[i].offset = hdr_off;
            files[i].data = initrd_base + hdr_off;

            total_bytes += len;
        } else {
            files[i].offset = 0;
            files[i].data = NULL;
        }
    }

    initrd_init_fds();

    if (initrd_should_log()) {
        serial_puts("INITRD: Initialized successfully!\n");
        printf("INITRD: Initialized successfully\n");
    }
}

uint64_t initrd_get_file_count(void) {
    return file_count;
}

initrd_file_t *initrd_get_file(uint64_t index) {
    if (index >= file_count) return NULL;
    return &files[index];
}

initrd_file_t *initrd_find_file(const char *name) {
    for (uint64_t i = 0; i < file_count; i++) {
        if (strcmp(files[i].name, name) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

void initrd_list_files(void) {
    printf("INITRD: File listing (%u files, version %u):\n", file_count, initrd_version);
    if (file_count == 0) {
        printf("  (no files or initrd not initialized)\n");
        return;
    }
    for (uint64_t i = 0; i < file_count; i++) {
        char tname[65];
        char typechar;
        char rperm;
        char wperm;
        char xperm;
        memcpy(tname, files[i].name, 64);
        tname[64] = '\0';
        typechar = (files[i].type == INITRD_TYPE_DIR) ? 'd' : '-';
        rperm = (files[i].permissions & INITRD_PERM_READ) ? 'r' : '-';
        wperm = (files[i].permissions & INITRD_PERM_WRITE) ? 'w' : '-';
        xperm = (files[i].permissions & INITRD_PERM_EXEC) ? 'x' : '-';
        printf("  [%u] %c%c%c%c %s (off=%u len=%u) parent=%u\n", i, typechar, rperm, wperm, xperm, tname, files[i].offset, files[i].length, files[i].parent_index);
    }
}

void initrd_init_fds(void) {
    for (int i = 0; i < INITRD_MAX_FDS; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].file_index = 0;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }
}

static int find_free_fd(void) {
    for (int i = 3; i < INITRD_MAX_FDS; i++) {
        if (!fd_table[i].in_use) return i;
    }
    return -1;
}

initrd_file_t *initrd_find_path(const char *path) {
    char component[65];
    uint16_t current_parent;
    if (!path || !files || file_count == 0) return NULL;

    while (*path == '/') path++;
    if (*path == '\0') {
        for (uint64_t i = 0; i < file_count; i++) {
            if (files[i].type == INITRD_TYPE_DIR && files[i].parent_index == 0xFFFF) {
                return &files[i];
            }
        }
        return NULL;
    }

    current_parent = 0xFFFF;

    while (*path) {
        int len;
        while (*path == '/') path++;
        if (*path == '\0') break;

        len = 0;
        while (path[len] && path[len] != '/' && len < 64) {
            component[len] = path[len];
            len++;
        }
        component[len] = '\0';
        path += len;

        initrd_file_t *found = NULL;
        for (uint64_t i = 0; i < file_count; i++) {
            if (files[i].parent_index == current_parent && strcmp(files[i].name, component) == 0) {
                found = &files[i];
                current_parent = (uint16_t)i;
                break;
            }
        }

        if (!found) return NULL;

        if (*path == '\0' || (*path == '/' && *(path+1) == '\0')) {
            return found;
        }

        if (found->type != INITRD_TYPE_DIR) {
            return NULL;
        }
    }

    return NULL;
}

int initrd_open(const char *path, int flags) {
    int is_dir;
    int fd;
    uint64_t idx;
    if (!path) return -1;

    initrd_file_t *f = initrd_find_path(path);
    if (!f) {
        f = initrd_find_file(path);
    }
    if (!f) return -1;

    is_dir = (f->type == INITRD_TYPE_DIR);
    
    if (flags & 0200000) {
        if (!is_dir) return -20;
    } else {
        if (is_dir) return -21;
    }

    fd = find_free_fd();
    if (fd < 0) return -1;

    idx = 0;
    for (uint64_t i = 0; i < file_count; i++) {
        if (&files[i] == f) { idx = i; break; }
    }

    fd_table[fd].in_use = 1;
    fd_table[fd].file_index = idx;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = flags;

    return fd;
}

int initrd_read(int fd, void *buf, uint64_t count) {
    uint64_t idx;
    uint64_t off;
    uint64_t avail;
    uint64_t to_read;
    if (fd < 0 || fd >= INITRD_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;
    if (!buf || count == 0) return 0;

    idx = fd_table[fd].file_index;
    if (idx >= file_count) return -1;

    initrd_file_t *f = &files[idx];
    off = fd_table[fd].offset;

    if (off >= f->length) return 0;

    avail = f->length - off;
    to_read = (count < avail) ? count : avail;

    memcpy(buf, f->data + off, to_read);
    fd_table[fd].offset += to_read;

    return (int)to_read;
}

int initrd_close(int fd) {
    if (fd < 0 || fd >= INITRD_MAX_FDS) return -1;
    if (!fd_table[fd].in_use) return -1;

    fd_table[fd].in_use = 0;
    fd_table[fd].file_index = 0;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = 0;

    return 0;
}

int initrd_stat(const char *path, uint64_t *size, uint8_t *type, uint8_t *perms) {
    if (!path) return -1;

    initrd_file_t *f = initrd_find_path(path);
    if (!f) f = initrd_find_file(path);
    if (!f) return -1;

    if (size) *size = f->length;
    if (type) *type = f->type;
    if (perms) *perms = f->permissions;

    return 0;
}

static uint64_t initrd_vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint64_t idx;
    uint64_t avail;
    uint64_t to_read;
    if (!node || !buffer) return 0;
    idx = node->inode;
    if (idx >= file_count) return 0;
    initrd_file_t *f = &files[idx];
    if (f->type != INITRD_TYPE_FILE) return 0;
    if (offset >= f->length) return 0;
    avail = f->length - offset;
    to_read = (size < avail) ? size : avail;

    if (initrd_should_log()) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        printf("INITRD: read dest=0x%016lX src=0x%016lX offset=%u to_read=%lu cr3=0x%016lX\n",
               (uint64_t)buffer, (uint64_t)(f->data + offset), offset, to_read, cr3);
        if (to_read >= 4) {
            printf("INITRD: src first4: %02X %02X %02X %02X\n",
                   f->data[offset], f->data[offset+1], f->data[offset+2], f->data[offset+3]);
            printf("INITRD: dest-before first4: %02X %02X %02X %02X\n",
                   ((uint8_t*)buffer)[0], ((uint8_t*)buffer)[1], ((uint8_t*)buffer)[2], ((uint8_t*)buffer)[3]);
        }
    }

    memcpy(buffer, f->data + offset, to_read);

    if (initrd_should_log()) {
        if (to_read >= 4) {
            printf("INITRD: dest-after first4: %02X %02X %02X %02X\n",
                   ((uint8_t*)buffer)[0], ((uint8_t*)buffer)[1], ((uint8_t*)buffer)[2], ((uint8_t*)buffer)[3]);
        }
    }

    return to_read;
}

static void initrd_vfs_open(vfs_node_t *node, uint64_t flags) {
    (void)node;
    (void)flags;
}

static void initrd_vfs_close(vfs_node_t *node) {
    (void)node;
}

static dirent_t *initrd_vfs_readdir(vfs_node_t *node, uint64_t index) {
    uint64_t parent_idx;
    uint64_t count;
    if (!node || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    parent_idx = node->inode;
    count = 0;
    for (uint64_t i = 0; i < file_count; i++) {
        uint16_t pi = files[i].parent_index;
        int match = 0;
        if (node == initrd_vfs_root) {
            match = initrd_is_root_parent(pi);
        } else {
            match = (pi == parent_idx);
        }
        if (match) {
            if (count == index) {
                for (int j = 0; j < VFS_MAX_NAME - 1 && files[i].name[j]; j++) {
                    initrd_dirent.name[j] = files[i].name[j];
                    initrd_dirent.name[j + 1] = '\0';
                }
                initrd_dirent.inode = i;
                initrd_dirent.type = (files[i].type == INITRD_TYPE_DIR) ? VFS_DIRECTORY : VFS_FILE;
                return &initrd_dirent;
            }
            count++;
        }
    }
    return NULL;
}

static vfs_node_t *initrd_vfs_finddir(vfs_node_t *node, const char *name) {
    uint64_t parent_idx;
    if (!node || !name || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    parent_idx = node->inode;
    for (uint64_t i = 0; i < file_count; i++) {
        uint16_t pi = files[i].parent_index;
        int match = 0;
        if (node == initrd_vfs_root) {
            match = initrd_is_root_parent(pi);
        } else {
            match = (pi == parent_idx);
        }
        if (match && strcmp(files[i].name, name) == 0) {
            return &initrd_vfs_nodes[i];
        }
    }
    return NULL;
}

static vfs_node_t *initrd_vfs_do_mount(const char *device, const char *mountpoint) {
    (void)device;
    (void)mountpoint;

    if (initrd_vfs_root)
        return initrd_vfs_root;
    
    if (!files || file_count == 0) {
        printf("INITRD_VFS: No files to mount\n");
        return NULL;
    }
    
    initrd_vfs_node_count = file_count + 1;
    initrd_vfs_nodes = (vfs_node_t *)kmalloc(initrd_vfs_node_count * sizeof(vfs_node_t));
    if (!initrd_vfs_nodes) {
        printf("INITRD_VFS: Failed to allocate nodes\n");
        return NULL;
    }
    memset(initrd_vfs_nodes, 0, initrd_vfs_node_count * sizeof(vfs_node_t));
    
    initrd_vfs_root = &initrd_vfs_nodes[file_count];
    initrd_vfs_root->name[0] = '/';
    initrd_vfs_root->name[1] = '\0';
    initrd_vfs_root->flags = VFS_DIRECTORY;
    initrd_vfs_root->inode = 0xFFFFFFFF;
    initrd_vfs_root->length = 0;
    initrd_vfs_root->readdir = initrd_vfs_readdir;
    initrd_vfs_root->finddir = initrd_vfs_finddir;
    initrd_vfs_root->parent = NULL;
    
    for (uint64_t i = 0; i < file_count; i++) {
        vfs_node_t *n = &initrd_vfs_nodes[i];
        for (int j = 0; j < VFS_MAX_NAME - 1 && files[i].name[j]; j++) {
            n->name[j] = files[i].name[j];
            n->name[j + 1] = '\0';
        }
        n->inode = i;
        n->length = files[i].length;
        n->uid = files[i].uid;
        n->gid = files[i].gid;
        n->mask = files[i].permissions;
        if (files[i].type == INITRD_TYPE_DIR) {
            n->flags = VFS_DIRECTORY;
            n->readdir = initrd_vfs_readdir;
            n->finddir = initrd_vfs_finddir;
        } else {
            n->flags = VFS_FILE;
            n->read = initrd_vfs_read;
        }
        n->open = initrd_vfs_open;
        n->close = initrd_vfs_close;
        if (files[i].parent_index == 0xFFFF) {
            n->parent = initrd_vfs_root;
        } else if (files[i].parent_index < file_count) {
            n->parent = &initrd_vfs_nodes[files[i].parent_index];
        } else {
            n->parent = initrd_vfs_root;
        }
    }
    
    printf("INITRD_VFS: Mounted %u files\n", file_count);
    return initrd_vfs_root;
}

static vfs_fs_type_t initrd_fs_type;

vfs_node_t *initrd_get_vfs_root(void) {
    if (initrd_vfs_root)
        return initrd_vfs_root;
    if (!files || file_count == 0)
        return NULL;
    return initrd_vfs_do_mount(NULL, NULL);
}

void initrd_vfs_register(void) {
    initrd_fs_type.name = "initrd";
    initrd_fs_type.mount = initrd_vfs_do_mount;
    initrd_fs_type.unmount = NULL;
    initrd_fs_type.next = NULL;

    vfs_register_fs(&initrd_fs_type);
}

void initrd_copy_to_root(void) {
    uint64_t dirs_created;
    uint64_t files_copied;
    uint64_t errors;
    const char **p;
    int r;
    uint64_t i;
    char destpath[INITRD_MAX_PATH];
    char tmp[INITRD_MAX_PATH];
    int cur;
    char namebuf[VFS_MAX_NAME];
    char newtmp[INITRD_MAX_PATH];
    int k;
    bool is_root_level_dir;
    bool is_standard_root;
    int dlen;
    int cp;
    char sub[INITRD_MAX_PATH];
    int sublen;
    int ret;
    int written;
    initrd_file_t *f;
    const char *root_dirs[] = {
        "/bin", "/dev", "/etc", "/home", "/lib", "/sbin", 
        "/usr", "/var", "/tmp", "/proc", "/run", "/root",
        NULL
    };
    const char *nested_dirs[] = {
        "/usr/bin", "/usr/lib", "/usr/sbin", "/usr/share",
        "/var/log", "/var/run", "/var/tmp",
        NULL
    };

    if (!files || file_count == 0) {
        printf("INITRD: No files to copy to root\n");
        return;
    }
    
    printf("INITRD: Copying %u files to /...\n", file_count);
    
    dirs_created = 0;
    files_copied = 0;
    errors = 0;

    for (p = root_dirs; *p; p++) {
        r = ramfs_create_dir(*p, 0755);
        if (r == 0) {
            printf("INITRD: Created root dir %s\n", *p);
            dirs_created++;
        } else {
            printf("INITRD: Failed to create root dir %s (%d)\n", *p, r);
            errors++;
        }
    }
    
    for (p = nested_dirs; *p; p++) {
        r = ramfs_create_dir(*p, 0755);
        if (r == 0) {
            printf("INITRD: Created nested dir %s\n", *p);
            dirs_created++;
        } else {
            printf("INITRD: Failed to create nested dir %s (%d)\n", *p, r);
            errors++;
        }
    }
    
    for (i = 0; i < file_count; i++) {
        f = &files[i];
        
        if (!f || f->name[0] == '\0') {
            continue;
        }
        
        destpath[0] = '\0';
        tmp[0] = '\0';
        cur = (int)i;
        while (cur >= 0 && cur < (int)file_count) {
            k = 0;
            while (k < VFS_MAX_NAME - 1 && files[cur].name[k]) { 
                namebuf[k] = files[cur].name[k]; 
                k++; 
            }
            namebuf[k] = '\0';
            if (tmp[0] == '\0') {
                snprintf(newtmp, sizeof(newtmp), "/%s", namebuf);
            } else {
                snprintf(newtmp, sizeof(newtmp), "/%s%s", namebuf, tmp);
            }
            strncpy(tmp, newtmp, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            if (files[cur].parent_index == 0xFFFF || files[cur].parent_index >= file_count) {
                break;
            }
            cur = (int)files[cur].parent_index;
        }
        strncpy(destpath, tmp, sizeof(destpath) - 1);
        destpath[sizeof(destpath) - 1] = '\0';

        printf("INITRD_COPY: i=%u name='%s' parent=%u type=%u dest='%s'\n", 
               (unsigned)i, files[i].name, files[i].parent_index, files[i].type, destpath);

        is_root_level_dir = (files[i].parent_index == 0xFFFF && files[i].type == INITRD_TYPE_DIR);
        if (is_root_level_dir) {
            is_standard_root = (strcmp(destpath, "/bin") == 0 || strcmp(destpath, "/dev") == 0 ||
                                     strcmp(destpath, "/etc") == 0 || strcmp(destpath, "/home") == 0 ||
                                     strcmp(destpath, "/lib") == 0 || strcmp(destpath, "/sbin") == 0 ||
                                     strcmp(destpath, "/usr") == 0 || strcmp(destpath, "/var") == 0 ||
                                     strcmp(destpath, "/tmp") == 0 || strcmp(destpath, "/proc") == 0 ||
                                     strcmp(destpath, "/run") == 0 || strcmp(destpath, "/root") == 0);
            if (is_standard_root) {
                printf("INITRD: Skipping root-level dir %s (already created independently)\n", destpath);
                continue;
            }
        }

        dlen = (int)strlen(destpath);
        for (cp = 1; cp < dlen; cp++) {
            if (destpath[cp] == '/') {
                sublen = cp;
                memcpy(sub, destpath, sublen);
                sub[sublen] = '\0';
                if (sublen > 1) {
                    r = ramfs_create_dir(sub, 0755);
                    (void)r; 
                }
            }
        }

        if (f->type == INITRD_TYPE_DIR) {
            ret = ramfs_create_dir(destpath, 0755);
            if (ret == 0) {
                dirs_created++;
            } else if (ret != RAMFS_ERR_EXIST) {
                printf("  mkdir %s FAILED (%d)\n", destpath, ret);
                errors++;
            }
        } else {
            ret = ramfs_create_file(destpath, 0644);
            if (ret == 0 || ret == RAMFS_ERR_EXIST) {
                if (f->data && f->length > 0) {
                    written = ramfs_write(destpath, 0, f->data, f->length);
                    if (written >= 0) {
                        files_copied++;
                    } else {
                        printf("  write %s FAILED (%d)\n", destpath, written);
                        errors++;
                    }
                } else {
                    files_copied++;
                }
            } else {
                printf("  create %s FAILED (%d)\n", destpath, ret);
                errors++;
            }
        }
    }
    
    printf("INITRD: Copied %u dirs, %u files (%u errors)\n", dirs_created, files_copied, errors);
}

void rootfs_init(uint64_t mods_count, uint64_t mods_addr) {
    uint64_t mods_start_page;
    uint64_t mods_end_page;
    uint64_t phys;
    uint64_t virt;
    uint64_t mod_start_phys;
    uint64_t mod_end_phys;
    uint64_t mod_size;
    uint64_t start_page;
    uint64_t end_page;
    uint8_t *rootfs_base;
    uint64_t num_entries;
    uint64_t i;
    uint64_t dirs_created;
    uint64_t files_copied;
    uint64_t errors;
    const char **p;
    int r;
    static char destpath[INITRD_MAX_PATH];
    static char tmp[INITRD_MAX_PATH];
    static char namebuf[VFS_MAX_NAME];
    static char newtmp[INITRD_MAX_PATH];
    int cur;
    int depth;
    int k;
    bool is_root_level_dir;
    bool is_standard_root;
    static char sub[INITRD_MAX_PATH];
    int dlen;
    int cp;
    int sublen;
    uint8_t perms;
    int ret;
    uint64_t hdr_off;
    uint64_t hdr_len;
    multiboot_module_t *mods;
    multiboot_module_t *mod;
    initrd_header_t *hdr;
    initrd_file_header_t *hdrs;
    initrd_file_t *rfiles;
    const char *root_dirs[] = {
        "/bin", "/dev", "/etc", "/home", "/lib", "/sbin", 
        "/usr", "/var", "/tmp", "/proc", "/run", "/root",
        NULL
    };
    const char *nested_dirs[] = {
        "/usr/bin", "/usr/lib", "/usr/sbin", "/usr/share",
        "/var/log", "/var/run", "/var/tmp",
        NULL
    };
    initrd_file_t *f;

    if (mods_count < 2) {
        printf("ROOTFS: No rootfs module loaded (need at least 2 modules)\n");
        return;
    }

    mods_start_page = mods_addr & ~0xFFF;
    mods_end_page = (mods_addr + mods_count * sizeof(multiboot_module_t) + 0xFFF) & ~0xFFF;
    for (phys = mods_start_page; phys < mods_end_page; phys += 0x1000) {
        virt = phys + KERNEL_VMA;
        vmm_map_page(virt, phys, 0x003);
    }

    mods = (multiboot_module_t *)(mods_addr + KERNEL_VMA);
    mod = &mods[1];
    
    mod_start_phys = mod->mod_start;
    mod_end_phys = mod->mod_end;
    mod_size = mod_end_phys - mod_start_phys;

    initrd_mod1_phys_start = mod_start_phys;
    initrd_mod1_phys_end = mod_end_phys;
    
    printf("ROOTFS: Module at phys 0x%016lX - 0x%016lX (%lu bytes)\n", 
           mod_start_phys, mod_end_phys, mod_size);

    if (mod_size < sizeof(initrd_header_t)) {
        printf("ROOTFS: Module too small\n");
        return;
    }

    start_page = mod_start_phys & ~0xFFF;
    end_page = (mod_end_phys + 0xFFF) & ~0xFFF;
    for (phys = start_page; phys < end_page; phys += 0x1000) {
        virt = phys + KERNEL_VMA;
        vmm_map_page(virt, phys, 0x003);
    }

    rootfs_base = (uint8_t *)(mod_start_phys + KERNEL_VMA);
    hdr = (initrd_header_t *)rootfs_base;

    if (hdr->magic != INITRD_MAGIC) {
        printf("ROOTFS: Invalid magic (got 0x%08lX, expected 0x%08lX)\n", 
               hdr->magic, INITRD_MAGIC);
        return;
    }

    num_entries = hdr->num_entries;
    printf("ROOTFS: Found %u entries\n", num_entries);

    if (num_entries == 0) {
        printf("ROOTFS: No entries to copy\n");
        return;
    }

    if (num_entries > 1000) {
        printf("ROOTFS: Too many entries (%u), limiting to 1000\n", num_entries);
        num_entries = 1000;
    }

    hdrs = (initrd_file_header_t *)(rootfs_base + sizeof(initrd_header_t));

    rfiles = (initrd_file_t *)kmalloc(num_entries * sizeof(initrd_file_t));
    if (!rfiles) {
        printf("ROOTFS: Failed to allocate file array\n");
        return;
    }
    memset(rfiles, 0, num_entries * sizeof(initrd_file_t));

    for (i = 0; i < num_entries; i++) {
        memcpy(rfiles[i].name, hdrs[i].name, 64);
        rfiles[i].length = hdrs[i].length;
        rfiles[i].type = hdrs[i].type;
        rfiles[i].permissions = hdrs[i].permissions;
        rfiles[i].parent_index = hdrs[i].parent_index;
        rfiles[i].uid = hdrs[i].uid;
        rfiles[i].gid = hdrs[i].gid;

        if (rfiles[i].type == INITRD_TYPE_FILE) {
            hdr_off = hdrs[i].offset;
            hdr_len = hdrs[i].length;

            if (hdr_off + hdr_len > mod_size) {
                printf("ROOTFS: File %u (%s) out-of-bounds\n",
                       i, rfiles[i].name);
                rfiles[i].offset = hdr_off;
                rfiles[i].data = NULL;
                rfiles[i].length = 0;
                continue;
            }

            rfiles[i].offset = hdr_off;
            rfiles[i].data = rootfs_base + hdr_off;
            rfiles[i].length = hdr_len;
            rfiles[i].offset = 0;
            rfiles[i].length = hdr_len;
        } else {
            rfiles[i].offset = 0;
            rfiles[i].data = NULL;
        }
    }

    printf("ROOTFS: Copying %u files to /...\n", num_entries);
    
    dirs_created = 0;
    files_copied = 0;
    errors = 0;

    for (p = root_dirs; *p; p++) {
        r = ramfs_create_dir(*p, 0755);
        if (r == 0) dirs_created++;
    }
    
    for (p = nested_dirs; *p; p++) {
        r = ramfs_create_dir(*p, 0755);
        if (r == 0) dirs_created++;
    }

    for (i = 0; i < num_entries; i++) {
        f = &rfiles[i];
        
        if (!f || f->name[0] == '\0') continue;
        
        destpath[0] = '\0';
        tmp[0] = '\0';
        namebuf[0] = '\0';
        newtmp[0] = '\0';
        
        cur = (int)i;
        depth = 0;
        while (cur >= 0 && cur < (int)num_entries && depth < 16) {
            k = 0;
            while (k < VFS_MAX_NAME - 1 && rfiles[cur].name[k]) { 
                namebuf[k] = rfiles[cur].name[k]; 
                k++; 
            }
            namebuf[k] = '\0';
            if (tmp[0] == '\0') {
                snprintf(newtmp, sizeof(newtmp), "/%s", namebuf);
            } else {
                snprintf(newtmp, sizeof(newtmp), "/%s%s", namebuf, tmp);
            }
            strncpy(tmp, newtmp, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            if (rfiles[cur].parent_index == 0xFFFF || rfiles[cur].parent_index >= num_entries) {
                break;
            }
            cur = (int)rfiles[cur].parent_index;
            depth++;
        }
        strncpy(destpath, tmp, sizeof(destpath) - 1);
        destpath[sizeof(destpath) - 1] = '\0';
        
        if (destpath[0] == '\0') continue;

        is_root_level_dir = (rfiles[i].parent_index == 0xFFFF && rfiles[i].type == INITRD_TYPE_DIR);
        if (is_root_level_dir) {
            is_standard_root = (strcmp(destpath, "/bin") == 0 || strcmp(destpath, "/dev") == 0 ||
                                     strcmp(destpath, "/etc") == 0 || strcmp(destpath, "/home") == 0 ||
                                     strcmp(destpath, "/lib") == 0 || strcmp(destpath, "/sbin") == 0 ||
                                     strcmp(destpath, "/usr") == 0 || strcmp(destpath, "/var") == 0 ||
                                     strcmp(destpath, "/tmp") == 0 || strcmp(destpath, "/proc") == 0 ||
                                     strcmp(destpath, "/run") == 0 || strcmp(destpath, "/root") == 0);
            if (is_standard_root) continue;
        }

        dlen = (int)strlen(destpath);
        for (cp = 1; cp < dlen; cp++) {
            if (destpath[cp] == '/') {
                sublen = cp;
                memcpy(sub, destpath, sublen);
                sub[sublen] = '\0';
                if (sublen > 1) {
                    ramfs_create_dir(sub, 0755);
                }
            }
        }

        perms = f->permissions;
        if (!perms) {
            perms = (f->type == INITRD_TYPE_DIR) ? 0755 : 0644;
        }

        if (f->type == INITRD_TYPE_DIR) {
            ret = ramfs_create_dir(destpath, perms);
            if (ret == 0) dirs_created++;
            else if (ret != RAMFS_ERR_EXIST) errors++;
        } else {
            ret = ramfs_create_file(destpath, perms);
            if (ret == 0 || ret == RAMFS_ERR_EXIST) {
                if (f->data && f->length > 0) {
                    ret = ramfs_set_backing(destpath, f->data, f->length);
                    if (ret == 0) files_copied++;
                    else errors++;
                } else {
                    DEBUG_INITRD("WARNING: no data for '%s' (data=%p length=%u)\n", destpath, f->data, f->length);
                    files_copied++;
                }
            } else {
                DEBUG_INITRD("ERROR: ramfs_create_file failed for '%s' ret=%d\n", destpath, ret);
                errors++;
            }
        }
    }
    
    kfree(rfiles);
    printf("ROOTFS: Created %u dirs, %u files with backing (%u errors)\n", dirs_created, files_copied, errors);
}

static void initrd_free_region(uint64_t phys_start, uint64_t phys_end) {
    uint64_t page_start;
    uint64_t page_end;
    uint64_t phys;
    uint64_t freed;

    if (phys_start == 0 || phys_end == 0 || phys_end <= phys_start) return;

    page_start = phys_start & ~0xFFFu;
    page_end = (phys_end + 0xFFFu) & ~0xFFFu;
    freed = 0;

    for (phys = page_start; phys < page_end; phys += PAGE_SIZE) {
        pfa_free(phys);
        freed++;
    }

    printf("INITRD: Freed %lu pages (phys 0x%016lX-0x%016lX, %lu KB)\n",
           freed, page_start, page_end, freed * 4);
}

void initrd_free_pages(void) {
    if (initrd_mod0_phys_start && initrd_mod0_phys_end) {
        initrd_free_region(initrd_mod0_phys_start, initrd_mod0_phys_end);
        initrd_mod0_phys_start = 0;
        initrd_mod0_phys_end = 0;
    }

    if (initrd_mod1_phys_start && initrd_mod1_phys_end) {
        if (!squashfs_get_context()) {
            initrd_free_region(initrd_mod1_phys_start, initrd_mod1_phys_end);
            initrd_mod1_phys_start = 0;
            initrd_mod1_phys_end = 0;
        }
    }

    if (files) {
        kfree(files);
        files = NULL;
        file_count = 0;
    }

    if (initrd_vfs_nodes) {
        kfree(initrd_vfs_nodes);
        initrd_vfs_nodes = NULL;
        initrd_vfs_node_count = 0;
    }

    initrd_base = NULL;
    initrd_header = NULL;
    file_headers = NULL;
}

uint8_t *initrd_get_base(void) {
    return initrd_base;
}

uint64_t initrd_get_size(void) {
    if (!initrd_base)
        return 0;
    return initrd_mod0_phys_end - initrd_mod0_phys_start;
}

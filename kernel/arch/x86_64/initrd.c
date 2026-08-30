#include <lebirun/initrd.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/vfs.h>
#include <lebirun/ramfs.h>
#include <lebirun/squashfs.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#define INITRD_INITIAL_FDS 8

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

static initrd_fd_t *fd_table = NULL;
static int fd_table_capacity;

static vfs_node_t *initrd_vfs_nodes = NULL;
static uint64_t initrd_vfs_node_count = 0;
static vfs_node_t *initrd_vfs_root = NULL;
static dirent_t initrd_dirent;

static int initrd_is_root_parent_version(uint32_t parent_index,
                                         uint64_t version) {
    if (version >= 3) return parent_index == UINT32_MAX;
    if (parent_index == 0xFFFFu) return 1;
    if (version <= 1 && parent_index == 0) return 1;
    return 0;
}

static int initrd_is_root_parent(uint32_t parent_index) {
    return initrd_is_root_parent_version(parent_index, initrd_version);
}

static int KERNEL_INIT initrd_decode_parent(
    const initrd_file_header_t *header, uint64_t version,
    uint32_t *parent_index) {
    uint32_t high;

    if (!header || !parent_index) return -1;
    if (version < 3) {
        *parent_index = header->parent_index;
        return 0;
    }
    if (header->reserved & 0xFFFF0000u) return -1;
    high = header->reserved & 0xFFFFu;
    *parent_index = header->parent_index | (high << 16);
    return 0;
}

static int KERNEL_INIT initrd_parent_graph_valid(initrd_file_t *entries,
                                                 uint64_t count,
                                                 uint64_t version) {
    uint64_t i;
    uint64_t current;
    uint64_t traversed;
    uint32_t parent;

    for (i = 0; i < count; i++) {
        current = i;
        traversed = 0;
        for (;;) {
            parent = entries[current].parent_index;
            if (initrd_is_root_parent_version(parent, version)) break;
            if ((uint64_t)parent >= count) return 0;
            current = parent;
            traversed++;
            if (traversed >= count) return 0;
        }
    }
    return 1;
}

void KERNEL_INIT initrd_init(uint64_t mods_count, uint64_t mods_addr) {
    uint64_t mods_start_page;
    uint64_t mods_end_page;
    uint64_t mod_start_phys;
    uint64_t mod_end_phys;
    uint64_t mod_size;
    uint64_t start_page;
    uint64_t end_page;
    uint64_t module_array_size;
    uint64_t module_array_end;
    uint64_t headers_size;
    uint64_t phys;
    uint64_t virt;
    uint64_t i;
    uint64_t hdr_off;
    uint64_t len;
    multiboot_module_t *mod;

    if (mods_count == 0) {
        KERNEL_INIT_LOG("INITRD: No modules loaded\n");
        return;
    }

    if (mods_count > UINT64_MAX / sizeof(multiboot_module_t)) {
        KERNEL_INIT_LOG("INITRD: Invalid module count\n");
        return;
    }
    module_array_size = mods_count * sizeof(multiboot_module_t);
    if (module_array_size > UINT64_MAX - mods_addr) {
        KERNEL_INIT_LOG("INITRD: Invalid module array range\n");
        return;
    }
    module_array_end = mods_addr + module_array_size;
    if (module_array_end > UINT64_MAX - 0xFFF) {
        KERNEL_INIT_LOG("INITRD: Invalid module array range\n");
        return;
    }

    mods_start_page = mods_addr & ~0xFFF;
    mods_end_page = (module_array_end + 0xFFF) & ~0xFFF;
    for (phys = mods_start_page; phys < mods_end_page; phys += 0x1000) {
        virt = phys + KERNEL_VMA;
        vmm_map_page(virt, phys, 0x003);
    }

    mod = (multiboot_module_t *)(mods_addr + KERNEL_VMA);
    
    mod_start_phys = mod->mod_start;
    mod_end_phys = mod->mod_end;
    if (mod_end_phys < mod_start_phys) {
        KERNEL_INIT_LOG("INITRD: Invalid module range\n");
        return;
    }
    mod_size = mod_end_phys - mod_start_phys;

    initrd_mod0_phys_start = mod_start_phys;
    initrd_mod0_phys_end = mod_end_phys;
    
    if (mod_size < sizeof(initrd_header_t)) {
        KERNEL_INIT_LOG("INITRD: Module too small to contain header (%u bytes)\n", mod_size);
        return;
    }

    start_page = mod_start_phys & ~0xFFF;
    if (mod_end_phys > UINT64_MAX - 0xFFF) {
        KERNEL_INIT_LOG("INITRD: Invalid module end\n");
        return;
    }
    end_page = (mod_end_phys + 0xFFF) & ~0xFFF;
    for (phys = start_page; phys < end_page; phys += 0x1000) {
        virt = phys + KERNEL_VMA;
        vmm_map_page(virt, phys, 0x003);
    }

    initrd_base = (uint8_t *)(mod_start_phys + KERNEL_VMA);

    initrd_header = (initrd_header_t *)initrd_base;

    if (initrd_header->magic != INITRD_MAGIC) {
        KERNEL_INIT_LOG("INITRD: Invalid magic (got 0x%08X, expected 0x%08X)\n",
               (unsigned int)initrd_header->magic, (unsigned int)INITRD_MAGIC);
        return;
    }
    initrd_version = initrd_header->version;
    if (initrd_version == 0) initrd_version = 1;
    if (initrd_version > INITRD_VERSION) {
        KERNEL_INIT_LOG("INITRD: Unsupported version %u\n", initrd_version);
        return;
    }

    file_count = initrd_header->num_entries;

    file_headers = (initrd_file_header_t *)(initrd_base + sizeof(initrd_header_t));

    if (file_count > UINT64_MAX / sizeof(initrd_file_header_t)) {
        KERNEL_INIT_LOG("INITRD: Invalid file count\n");
        return;
    }
    headers_size = file_count * sizeof(initrd_file_header_t);
    if (headers_size > mod_size - sizeof(initrd_header_t)) {
        KERNEL_INIT_LOG("INITRD: File header array overruns module (headers end beyond module)\n");
        return;
    }

    if (file_count > SIZE_MAX / sizeof(initrd_file_t)) {
        KERNEL_INIT_LOG("INITRD: File array is too large\n");
        return;
    }

    if (file_count == 0) {
        files = NULL;
        KERNEL_INIT_LOG("INITRD: Initialized 0 files (version %u)\n", initrd_version);
        return;
    }

    files = (initrd_file_t *)kmalloc(file_count * sizeof(initrd_file_t));
    if (!files) {
        KERNEL_INIT_LOG("INITRD: Failed to allocate file array\n");
        return;
    }

    for (i = 0; i < file_count; i++) {
        memcpy(files[i].name, file_headers[i].name, 64);
        files[i].name[63] = '\0';
        files[i].length = file_headers[i].length;
        files[i].type = file_headers[i].type;
        files[i].permissions = file_headers[i].permissions;
        if (initrd_decode_parent(&file_headers[i], initrd_version,
                                 &files[i].parent_index) != 0) {
            KERNEL_INIT_LOG("INITRD: File %u has invalid parent encoding\n", i);
            kfree(files);
            files = NULL;
            file_count = 0;
            return;
        }
        files[i].uid = file_headers[i].uid;
        files[i].gid = file_headers[i].gid;

        hdr_off = file_headers[i].offset;
        len = file_headers[i].length;

        if (files[i].type != INITRD_TYPE_FILE &&
            files[i].type != INITRD_TYPE_DIR) {
            KERNEL_INIT_LOG("INITRD: File %u (%s) has invalid type %u\n",
                   i, files[i].name, files[i].type);
            kfree(files);
            files = NULL;
            file_count = 0;
            return;
        }

        if (files[i].type == INITRD_TYPE_FILE) {
            if (hdr_off > mod_size || len > mod_size - hdr_off) {
                KERNEL_INIT_LOG("INITRD: File %u (%s) has out-of-bounds offset/length (off=%u len=%u mod_size=%u)\n",
                       i, files[i].name, hdr_off, len, mod_size);
                kfree(files);
                files = NULL;
                file_count = 0;
                return;
            }

            files[i].offset = hdr_off;
            files[i].data = initrd_base + hdr_off;

        } else {
            files[i].offset = 0;
            files[i].data = NULL;
        }
    }

    if (!initrd_parent_graph_valid(files, file_count, initrd_version)) {
        KERNEL_INIT_LOG("INITRD: Invalid parent graph\n");
        kfree(files);
        files = NULL;
        file_count = 0;
        return;
    }

    KERNEL_INIT_LOG("INITRD: Initialized %u files (version %u)\n", file_count, initrd_version);
}

uint64_t initrd_get_file_count(void) {
    return file_count;
}

initrd_file_t *initrd_get_file(uint64_t index) {
    if (index >= file_count) return NULL;
    return &files[index];
}

initrd_file_t *initrd_find_file(const char *name) {
    uint64_t i;

    for (i = 0; i < file_count; i++) {
        if (strcmp(files[i].name, name) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

void initrd_list_files(void) {
    uint64_t i;
    char tname[65];
    char typechar;
    char rperm;
    char wperm;
    char xperm;

    printf("INITRD: File listing (%u files, version %u):\n", file_count, initrd_version);
    if (file_count == 0) {
        printf("  (no files or initrd not initialized)\n");
        return;
    }
    for (i = 0; i < file_count; i++) {
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
    int i;

    if (!fd_table) {
        fd_table = (initrd_fd_t *)kmalloc(INITRD_INITIAL_FDS * sizeof(initrd_fd_t));
        if (!fd_table) return;
        fd_table_capacity = INITRD_INITIAL_FDS;
    }
    for (i = 0; i < fd_table_capacity; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].file_index = 0;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }
}

static int find_free_fd(void) {
    initrd_fd_t *new_table;
    int old_capacity;
    int new_capacity;
    int i;

    if (!fd_table) initrd_init_fds();
    if (!fd_table) return -1;
    for (i = 3; i < fd_table_capacity; i++) {
        if (!fd_table[i].in_use) return i;
    }
    if (fd_table_capacity > INT32_MAX / 2) return -1;
    old_capacity = fd_table_capacity;
    new_capacity = old_capacity * 2;
    if ((uint64_t)new_capacity > UINT64_MAX / sizeof(initrd_fd_t)) return -1;
    new_table = (initrd_fd_t *)krealloc(fd_table,
                                        (uint64_t)new_capacity * sizeof(initrd_fd_t));
    if (!new_table) return -1;
    fd_table = new_table;
    fd_table_capacity = new_capacity;
    for (i = old_capacity; i < new_capacity; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].file_index = 0;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }
    return old_capacity;
}

initrd_file_t *initrd_find_path(const char *path) {
    char component[65];
    uint32_t current_parent;
    uint64_t i;
    int len;
    initrd_file_t *found;

    if (!path || !files || file_count == 0) return NULL;

    while (*path == '/') path++;
    if (*path == '\0') {
        for (i = 0; i < file_count; i++) {
            if (files[i].type == INITRD_TYPE_DIR &&
                initrd_is_root_parent(files[i].parent_index)) {
                return &files[i];
            }
        }
        return NULL;
    }

    current_parent = initrd_version >= 3 ? UINT32_MAX : 0xFFFFu;

    while (*path) {
        while (*path == '/') path++;
        if (*path == '\0') break;

        len = 0;
        while (path[len] && path[len] != '/' && len < 64) {
            component[len] = path[len];
            len++;
        }
        component[len] = '\0';
        path += len;

        found = NULL;
        for (i = 0; i < file_count; i++) {
            if (files[i].parent_index == current_parent && strcmp(files[i].name, component) == 0) {
                found = &files[i];
                current_parent = (uint32_t)i;
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
    uint64_t i;
    initrd_file_t *f;

    if (!path) return -1;
    f = initrd_find_path(path);
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
    for (i = 0; i < file_count; i++) {
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
    initrd_file_t *f;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table) return -1;
    if (!fd_table[fd].in_use) return -1;
    if (!buf || count == 0) return 0;

    idx = fd_table[fd].file_index;
    if (idx >= file_count) return -1;

    f = &files[idx];
    off = fd_table[fd].offset;

    if (off >= f->length) return 0;

    avail = f->length - off;
    to_read = (count < avail) ? count : avail;

    memcpy(buf, f->data + off, to_read);
    fd_table[fd].offset += to_read;

    return (int)to_read;
}

int initrd_close(int fd) {
    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table) return -1;
    if (!fd_table[fd].in_use) return -1;

    fd_table[fd].in_use = 0;
    fd_table[fd].file_index = 0;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = 0;

    return 0;
}

int initrd_fstat_fd(int fd, uint64_t *size, uint8_t *type) {
    uint64_t idx;
    initrd_file_t *f;

    if (fd < 0 || fd >= fd_table_capacity) return -1;
    if (!fd_table) return -1;
    if (!fd_table[fd].in_use) return -1;
    idx = fd_table[fd].file_index;
    if (idx >= file_count) return -1;
    f = &files[idx];
    if (size) *size = f->length;
    if (type) *type = f->type;
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
    initrd_file_t *f;

    if (!node || !buffer) return 0;
    idx = node->inode;
    if (idx >= file_count) return 0;
    f = &files[idx];
    if (f->type != INITRD_TYPE_FILE) return 0;
    if (offset >= f->length) return 0;
    avail = f->length - offset;
    to_read = (size < avail) ? size : avail;

    memcpy(buffer, f->data + offset, to_read);

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
    uint64_t i;
    uint32_t pi;
    int match;
    int j;

    if (!node || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    parent_idx = node->inode;
    count = 0;
    for (i = 0; i < file_count; i++) {
        pi = files[i].parent_index;
        match = 0;
        if (node == initrd_vfs_root) {
            match = initrd_is_root_parent(pi);
        } else {
            match = (pi == parent_idx);
        }
        if (match) {
            if (count == index) {
                for (j = 0; j < VFS_MAX_NAME - 1 && files[i].name[j]; j++) {
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
    uint64_t i;
    uint32_t pi;
    int match;

    if (!node || !name || VFS_GET_TYPE(node->flags) != VFS_DIRECTORY) return NULL;
    parent_idx = node->inode;
    for (i = 0; i < file_count; i++) {
        pi = files[i].parent_index;
        match = 0;
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
    uint64_t i;
    uint64_t k;
    vfs_node_t *node;

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
    if (vfs_node_set_name(initrd_vfs_root, "/") != 0) {
        kfree(initrd_vfs_nodes);
        initrd_vfs_nodes = NULL;
        initrd_vfs_root = NULL;
        initrd_vfs_node_count = 0;
        return NULL;
    }
    initrd_vfs_root->flags = VFS_DIRECTORY;
    initrd_vfs_root->inode = 0xFFFFFFFF;
    initrd_vfs_root->length = 0;
    initrd_vfs_root->readdir = initrd_vfs_readdir;
    initrd_vfs_root->finddir = initrd_vfs_finddir;
    initrd_vfs_root->parent = NULL;
    
    for (i = 0; i < file_count; i++) {
        node = &initrd_vfs_nodes[i];
        node->inode = i;
        node->length = files[i].length;
        node->uid = files[i].uid;
        node->gid = files[i].gid;
        node->mask = files[i].permissions;
        if (files[i].type == INITRD_TYPE_DIR) {
            node->flags = VFS_DIRECTORY;
            node->readdir = initrd_vfs_readdir;
            node->finddir = initrd_vfs_finddir;
        } else {
            node->flags = VFS_FILE;
            node->read = initrd_vfs_read;
        }
        if (vfs_node_set_name(node, files[i].name) != 0) {
            for (k = 0; k <= i; k++)
                vfs_node_release_name(&initrd_vfs_nodes[k]);
            vfs_node_release_name(initrd_vfs_root);
            kfree(initrd_vfs_nodes);
            initrd_vfs_nodes = NULL;
            initrd_vfs_root = NULL;
            initrd_vfs_node_count = 0;
            return NULL;
        }
        node->open = initrd_vfs_open;
        node->close = initrd_vfs_close;
        if (initrd_is_root_parent(files[i].parent_index)) {
            node->parent = initrd_vfs_root;
        } else if (files[i].parent_index < file_count) {
            node->parent = &initrd_vfs_nodes[files[i].parent_index];
        } else {
            node->parent = initrd_vfs_root;
        }
    }
    
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

void KERNEL_INIT initrd_vfs_register(void) {
    initrd_fs_type.name = "initrd";
    initrd_fs_type.mount = initrd_vfs_do_mount;
    initrd_fs_type.unmount = NULL;
    initrd_fs_type.next = NULL;

    vfs_register_fs(&initrd_fs_type);
}

static char *KERNEL_INIT initrd_build_path(initrd_file_t *entries,
                                           uint64_t count, uint64_t index,
                                           uint64_t version) {
    uint64_t cur;
    uint64_t traversed;
    size_t total;
    size_t length;
    size_t pos;
    char *path;

    if (!entries || index >= count) return NULL;
    cur = index;
    traversed = 0;
    total = 0;
    while (cur < count && traversed < count) {
        length = strlen(entries[cur].name);
        if (length > SIZE_MAX - total - 1) return NULL;
        total += length + 1;
        if (initrd_is_root_parent_version(entries[cur].parent_index,
                                           version) ||
            entries[cur].parent_index >= count) break;
        cur = entries[cur].parent_index;
        traversed++;
    }
    if (traversed >= count && cur < count &&
        !initrd_is_root_parent_version(entries[cur].parent_index, version) &&
        entries[cur].parent_index < count) return NULL;
    path = (char *)kmalloc(total + 1);
    if (!path) return NULL;
    path[total] = '\0';
    pos = total;
    cur = index;
    traversed = 0;
    while (cur < count && traversed < count) {
        length = strlen(entries[cur].name);
        pos -= length;
        memcpy(path + pos, entries[cur].name, length);
        path[--pos] = '/';
        if (initrd_is_root_parent_version(entries[cur].parent_index,
                                           version) ||
            entries[cur].parent_index >= count) break;
        cur = entries[cur].parent_index;
        traversed++;
    }
    return path;
}

void KERNEL_INIT initrd_copy_to_root(void) {
    uint64_t dirs_created;
    uint64_t files_copied;
    uint64_t errors;
    const char **p;
    int r;
    uint64_t i;
    char *destpath;
    bool is_root_level_dir;
    bool is_standard_root;
    size_t dlen;
    size_t cp;
    char saved;
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
        KERNEL_INIT_LOG("INITRD: No files to copy to root\n");
        return;
    }
    
    KERNEL_INIT_LOG("INITRD: Copying %u files to /...\n", file_count);
    
    dirs_created = 0;
    files_copied = 0;
    errors = 0;

    for (p = root_dirs; *p; p++) {
        r = ramfs_create_dir(*p, 0755);
        if (r == 0) {
            dirs_created++;
        } else {
            KERNEL_INIT_LOG("INITRD: Failed to create root dir %s (%d)\n",
                            *p, r);
            errors++;
        }
    }
    
    for (p = nested_dirs; *p; p++) {
        r = ramfs_create_dir(*p, 0755);
        if (r == 0) {
            dirs_created++;
        } else {
            KERNEL_INIT_LOG("INITRD: Failed to create nested dir %s (%d)\n",
                            *p, r);
            errors++;
        }
    }
    
    for (i = 0; i < file_count; i++) {
        f = &files[i];
        
        if (!f || f->name[0] == '\0') {
            continue;
        }
        
        destpath = initrd_build_path(files, file_count, i, initrd_version);
        if (!destpath) {
            errors++;
            continue;
        }

        is_root_level_dir = (initrd_is_root_parent(files[i].parent_index) &&
                             files[i].type == INITRD_TYPE_DIR);
        if (is_root_level_dir) {
            is_standard_root = (strcmp(destpath, "/bin") == 0 || strcmp(destpath, "/dev") == 0 ||
                                     strcmp(destpath, "/etc") == 0 || strcmp(destpath, "/home") == 0 ||
                                     strcmp(destpath, "/lib") == 0 || strcmp(destpath, "/sbin") == 0 ||
                                     strcmp(destpath, "/usr") == 0 || strcmp(destpath, "/var") == 0 ||
                                     strcmp(destpath, "/tmp") == 0 || strcmp(destpath, "/proc") == 0 ||
                                     strcmp(destpath, "/run") == 0 || strcmp(destpath, "/root") == 0);
            if (is_standard_root) {
                kfree(destpath);
                continue;
            }
        }

        dlen = strlen(destpath);
        for (cp = 1; cp < dlen; cp++) {
            if (destpath[cp] == '/') {
                saved = destpath[cp];
                destpath[cp] = '\0';
                if (cp > 1) {
                    r = ramfs_create_dir(destpath, 0755);
                    (void)r; 
                }
                destpath[cp] = saved;
            }
        }

        if (f->type == INITRD_TYPE_DIR) {
            ret = ramfs_create_dir(destpath, 0755);
            if (ret == 0) {
                dirs_created++;
            } else if (ret != RAMFS_ERR_EXIST) {
                KERNEL_INIT_LOG("  mkdir %s FAILED (%d)\n", destpath, ret);
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
                        KERNEL_INIT_LOG("  write %s FAILED (%d)\n",
                                        destpath, written);
                        errors++;
                    }
                } else {
                    files_copied++;
                }
            } else {
                KERNEL_INIT_LOG("  create %s FAILED (%d)\n", destpath, ret);
                errors++;
            }
        }
        kfree(destpath);
    }
    
    KERNEL_INIT_LOG("INITRD: Copied %u dirs, %u files (%u errors)\n",
                    dirs_created, files_copied, errors);
}

void KERNEL_INIT rootfs_init(uint64_t mods_count, uint64_t mods_addr) {
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
    char *destpath;
    bool is_root_level_dir;
    bool is_standard_root;
    size_t dlen;
    size_t cp;
    char saved;
    uint8_t perms;
    int ret;
    uint64_t hdr_off;
    uint64_t hdr_len;
    uint64_t rootfs_version;
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
        KERNEL_INIT_LOG("ROOTFS: No rootfs module loaded (need at least 2 modules)\n");
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
    
    KERNEL_INIT_LOG("ROOTFS: Module at phys 0x%016lX - 0x%016lX (%lu bytes)\n",
                    mod_start_phys, mod_end_phys, mod_size);

    if (mod_size < sizeof(initrd_header_t)) {
        KERNEL_INIT_LOG("ROOTFS: Module too small\n");
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
        KERNEL_INIT_LOG("ROOTFS: Invalid magic (got 0x%08X, expected 0x%08X)\n",
                        (unsigned int)hdr->magic,
                        (unsigned int)INITRD_MAGIC);
        return;
    }

    rootfs_version = hdr->version;
    if (rootfs_version == 0) rootfs_version = 1;
    if (rootfs_version > INITRD_VERSION) {
        KERNEL_INIT_LOG("ROOTFS: Unsupported version %u\n", rootfs_version);
        return;
    }

    num_entries = hdr->num_entries;
    KERNEL_INIT_LOG("ROOTFS: Found %u entries\n", num_entries);

    if (num_entries == 0) {
        KERNEL_INIT_LOG("ROOTFS: No entries to copy\n");
        return;
    }

    if ((uint64_t)num_entries >
        (mod_size - sizeof(initrd_header_t)) / sizeof(initrd_file_header_t)) {
        KERNEL_INIT_LOG("ROOTFS: File header array exceeds module size\n");
        return;
    }

    hdrs = (initrd_file_header_t *)(rootfs_base + sizeof(initrd_header_t));

    rfiles = (initrd_file_t *)kmalloc(num_entries * sizeof(initrd_file_t));
    if (!rfiles) {
        KERNEL_INIT_LOG("ROOTFS: Failed to allocate file array\n");
        return;
    }
    memset(rfiles, 0, num_entries * sizeof(initrd_file_t));

    for (i = 0; i < num_entries; i++) {
        memcpy(rfiles[i].name, hdrs[i].name, 64);
        rfiles[i].name[63] = '\0';
        rfiles[i].length = hdrs[i].length;
        rfiles[i].type = hdrs[i].type;
        rfiles[i].permissions = hdrs[i].permissions;
        if (initrd_decode_parent(&hdrs[i], rootfs_version,
                                 &rfiles[i].parent_index) != 0) {
            KERNEL_INIT_LOG("ROOTFS: Entry %u has invalid parent encoding\n",
                            i);
            kfree(rfiles);
            return;
        }
        rfiles[i].uid = hdrs[i].uid;
        rfiles[i].gid = hdrs[i].gid;

        if (rfiles[i].type == INITRD_TYPE_FILE) {
            hdr_off = hdrs[i].offset;
            hdr_len = hdrs[i].length;

            if (hdr_off > mod_size || hdr_len > mod_size - hdr_off) {
                KERNEL_INIT_LOG("ROOTFS: File %u (%s) out-of-bounds\n",
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

    if (!initrd_parent_graph_valid(rfiles, num_entries, rootfs_version)) {
        KERNEL_INIT_LOG("ROOTFS: Invalid parent graph\n");
        kfree(rfiles);
        return;
    }

    KERNEL_INIT_LOG("ROOTFS: Copying %u files to /...\n", num_entries);
    
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
        
        destpath = initrd_build_path(rfiles, num_entries, i,
                                     rootfs_version);
        if (!destpath) {
            errors++;
            continue;
        }
        if (destpath[0] == '\0') {
            kfree(destpath);
            continue;
        }

        is_root_level_dir =
            (initrd_is_root_parent_version(rfiles[i].parent_index,
                                           rootfs_version) &&
             rfiles[i].type == INITRD_TYPE_DIR);
        if (is_root_level_dir) {
            is_standard_root = (strcmp(destpath, "/bin") == 0 || strcmp(destpath, "/dev") == 0 ||
                                     strcmp(destpath, "/etc") == 0 || strcmp(destpath, "/home") == 0 ||
                                     strcmp(destpath, "/lib") == 0 || strcmp(destpath, "/sbin") == 0 ||
                                     strcmp(destpath, "/usr") == 0 || strcmp(destpath, "/var") == 0 ||
                                     strcmp(destpath, "/tmp") == 0 || strcmp(destpath, "/proc") == 0 ||
                                     strcmp(destpath, "/run") == 0 || strcmp(destpath, "/root") == 0);
            if (is_standard_root) {
                kfree(destpath);
                continue;
            }
        }

        dlen = strlen(destpath);
        for (cp = 1; cp < dlen; cp++) {
            if (destpath[cp] == '/') {
                saved = destpath[cp];
                destpath[cp] = '\0';
                if (cp > 1) ramfs_create_dir(destpath, 0755);
                destpath[cp] = saved;
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
                    files_copied++;
                }
            } else {
                errors++;
            }
        }
        kfree(destpath);
    }
    
    kfree(rfiles);
    KERNEL_INIT_LOG("ROOTFS: Created %u dirs, %u files with backing (%u errors)\n",
                    dirs_created, files_copied, errors);
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
    uint64_t i;

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
        for (i = 0; i < initrd_vfs_node_count; i++)
            vfs_node_release_name(&initrd_vfs_nodes[i]);
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

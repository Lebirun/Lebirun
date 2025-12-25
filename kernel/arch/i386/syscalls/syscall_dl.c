#include "syscall_defs.h"
#include <kernel/elf.h>
#include <kernel/vfs.h>

extern void *syscall_table[];

#define RTLD_LAZY    0x00001
#define RTLD_NOW     0x00002
#define RTLD_GLOBAL  0x00100
#define RTLD_LOCAL   0x00000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

#define DL_BASE_ADDR 0x20000000

static dl_handle_t dl_handles[DL_MAX_HANDLES];
static char dl_error_msg[128] = "";
static int dl_initialized = 0;
static uint32_t dl_next_base = DL_BASE_ADDR;

static void init_dl(void) {
    if (dl_initialized) return;
    dl_initialized = 1;
    memset(dl_handles, 0, sizeof(dl_handles));
    dl_error_msg[0] = '\0';
    dl_next_base = DL_BASE_ADDR;
}

static int read_file_data(const char *path, uint8_t **out_data, uint32_t *out_size) {
    vfs_node_t *node = vfs_namei(path);
    if (!node) {
        return -1;
    }
    
    uint32_t size = node->length;
    if (size == 0) {
        return -2;
    }
    
    uint8_t *data = (uint8_t *)kmalloc(size);
    if (!data) {
        return -3;
    }
    
    uint32_t read = vfs_read(node, 0, size, data);
    if (read != size) {
        kfree(data);
        return -4;
    }
    
    *out_data = data;
    *out_size = size;
    return 0;
}

static int sys_dlopen(int filename_ptr, const char *flags_ptr, int unused) {
    (void)unused;
    init_dl();
    
    int flags = (int)(uintptr_t)flags_ptr;
    (void)flags;
    
    if (!filename_ptr) {
        strcpy(dl_error_msg, "dlopen: NULL filename not supported");
        return 0;
    }
    
    uint32_t name_addr = (uint32_t)filename_ptr;
    if (name_addr >= 0xC0000000 || name_addr < 0x1000) {
        strcpy(dl_error_msg, "dlopen: invalid filename pointer");
        return 0;
    }
    
    const char *filename = (const char *)name_addr;
    
    int slot = -1;
    for (int i = 0; i < DL_MAX_HANDLES; i++) {
        if (!dl_handles[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        strcpy(dl_error_msg, "dlopen: too many open handles");
        return 0;
    }
    
    int len = 0;
    while (filename[len] && len < 63) len++;
    
    for (int i = 0; i < DL_MAX_HANDLES; i++) {
        if (dl_handles[i].in_use) {
            int match = 1;
            for (int j = 0; j < len; j++) {
                if (dl_handles[i].name[j] != filename[j]) {
                    match = 0;
                    break;
                }
            }
            if (match && dl_handles[i].name[len] == '\0') {
                return (int)(i + 1);
            }
        }
    }
    
    char full_path[128];
    int path_len = 0;
    
    if (filename[0] == '/') {
        while (filename[path_len] && path_len < 127) {
            full_path[path_len] = filename[path_len];
            path_len++;
        }
    } else {
        const char *lib_prefix = "/lib/";
        int prefix_len = 5;
        for (int i = 0; i < prefix_len; i++) {
            full_path[path_len++] = lib_prefix[i];
        }
        for (int i = 0; i < len && path_len < 127; i++) {
            full_path[path_len++] = filename[i];
        }
    }
    full_path[path_len] = '\0';
    
    uint8_t *file_data = NULL;
    uint32_t file_size = 0;
    int ret = read_file_data(full_path, &file_data, &file_size);
    if (ret != 0) {
        strcpy(dl_error_msg, "dlopen: failed to read file");
        return 0;
    }
    
    int valid = elf_validate_so(file_data, file_size);
    if (valid != 0) {
        kfree(file_data);
        snprintf(dl_error_msg, sizeof(dl_error_msg), "dlopen: invalid shared object (code=%d)", valid);
        return 0;
    }
    
    uint32_t load_base = dl_next_base;
    dl_next_base += 0x100000;
    
    uint32_t pd_phys = current_task->pd_phys;
    
    ret = elf_load_so(pd_phys, file_data, file_size, load_base, &dl_handles[slot]);
    kfree(file_data);
    
    if (ret != 0) {
        strcpy(dl_error_msg, "dlopen: failed to load shared object");
        return 0;
    }
    
    for (int i = 0; i < len; i++) {
        dl_handles[slot].name[i] = filename[i];
    }
    dl_handles[slot].name[len] = '\0';
    dl_handles[slot].in_use = 1;
    
    dl_error_msg[0] = '\0';
    
    return (int)(slot + 1);
}

static int sys_dlsym(int handle, const char *symbol_ptr, int unused) {
    (void)unused;
    init_dl();
    
    if (handle <= 0 || handle > DL_MAX_HANDLES) {
        strcpy(dl_error_msg, "dlsym: invalid handle");
        return 0;
    }
    
    int slot = handle - 1;
    if (!dl_handles[slot].in_use) {
        strcpy(dl_error_msg, "dlsym: handle not open");
        return 0;
    }
    
    if (!symbol_ptr) {
        strcpy(dl_error_msg, "dlsym: NULL symbol");
        return 0;
    }
    
    uint32_t sym_addr = (uint32_t)(uintptr_t)symbol_ptr;
    if (sym_addr >= 0xC0000000 || sym_addr < 0x1000) {
        strcpy(dl_error_msg, "dlsym: invalid symbol pointer");
        return 0;
    }
    
    const char *symbol = (const char *)sym_addr;
    
    uint32_t addr = elf_so_find_symbol(&dl_handles[slot], symbol);
    if (addr == 0) {
        strcpy(dl_error_msg, "dlsym: symbol not found");
        return 0;
    }
    
    dl_error_msg[0] = '\0';
    return (int)addr;
}

static int sys_dlclose(int handle, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_dl();
    
    if (handle <= 0 || handle > DL_MAX_HANDLES) {
        strcpy(dl_error_msg, "dlclose: invalid handle");
        return -1;
    }
    
    int slot = handle - 1;
    if (!dl_handles[slot].in_use) {
        strcpy(dl_error_msg, "dlclose: handle not open");
        return -1;
    }
    
    if (dl_handles[slot].symtab) {
        kfree(dl_handles[slot].symtab);
        dl_handles[slot].symtab = NULL;
    }
    
    if (dl_handles[slot].strtab) {
        kfree(dl_handles[slot].strtab);
        dl_handles[slot].strtab = NULL;
    }

    if (dl_handles[slot].symtab2) {
        kfree(dl_handles[slot].symtab2);
        dl_handles[slot].symtab2 = NULL;
    }

    if (dl_handles[slot].strtab2) {
        kfree(dl_handles[slot].strtab2);
        dl_handles[slot].strtab2 = NULL;
    }
    
    if (dl_handles[slot].file_data) {
        kfree(dl_handles[slot].file_data);
        dl_handles[slot].file_data = NULL;
    }
    
    if (dl_handles[slot].pages) {
        kfree(dl_handles[slot].pages);
        dl_handles[slot].pages = NULL;
    }
    
    dl_handles[slot].in_use = 0;
    dl_handles[slot].name[0] = '\0';
    
    return 0;
}

static int sys_dlerror(int buf_ptr, const char *size_ptr, int unused) {
    (void)unused;
    init_dl();
    
    uint32_t buf_addr = (uint32_t)buf_ptr;
    uint32_t size = (uint32_t)(uintptr_t)size_ptr;
    
    if (!buf_addr || buf_addr >= 0xC0000000 || buf_addr < 0x1000) {
        return 0;
    }
    
    if (dl_error_msg[0] == '\0') {
        return 0;
    }
    
    char *buf = (char *)buf_addr;
    int len = 0;
    while (dl_error_msg[len] && (uint32_t)len < size - 1) {
        buf[len] = dl_error_msg[len];
        len++;
    }
    buf[len] = '\0';
    
    dl_error_msg[0] = '\0';
    
    return len;
}

void syscalls_dl_init(void) {
    init_dl();
    syscall_table[SYSCALL_DLOPEN] = sys_dlopen;
    syscall_table[SYSCALL_DLSYM] = sys_dlsym;
    syscall_table[SYSCALL_DLCLOSE] = sys_dlclose;
    syscall_table[SYSCALL_DLERROR] = sys_dlerror;
}

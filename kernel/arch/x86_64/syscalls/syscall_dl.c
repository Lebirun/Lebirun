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

static dl_handle_t *dl_handles;
static int dl_capacity = 0;
static char dl_error_msg[128] = "";
static int dl_initialized = 0;
static uint64_t dl_next_base = DL_BASE_ADDR;

static void init_dl(void) {
    if (dl_initialized) return;
    dl_initialized = 1;
    dl_handles = NULL;
    dl_capacity = 0;
    dl_error_msg[0] = '\0';
    dl_next_base = DL_BASE_ADDR;
}

static void dl_call_init(dl_handle_t *h) {
    uint64_t *arr;
    uint64_t count;
    uint64_t idx;
    typedef void (*init_fn_t)(void);
    init_fn_t fn;

    if (h->init_func) {
        fn = (init_fn_t)h->init_func;
        fn();
    }

    if (h->init_array_vaddr && h->init_array_size > 0) {
        arr = (uint64_t *)h->init_array_vaddr;
        count = h->init_array_size / sizeof(uint64_t);
        for (idx = 0; idx < count; idx++) {
            if (arr[idx] && arr[idx] != (uint64_t)-1) {
                fn = (init_fn_t)arr[idx];
                fn();
            }
        }
    }
}

static void dl_call_fini(dl_handle_t *h) {
    uint64_t *arr;
    uint64_t count;
    uint64_t idx;
    typedef void (*fini_fn_t)(void);
    fini_fn_t fn;

    if (h->fini_array_vaddr && h->fini_array_size > 0) {
        arr = (uint64_t *)h->fini_array_vaddr;
        count = h->fini_array_size / sizeof(uint64_t);
        for (idx = count; idx > 0; idx--) {
            if (arr[idx - 1] && arr[idx - 1] != (uint64_t)-1) {
                fn = (fini_fn_t)arr[idx - 1];
                fn();
            }
        }
    }

    if (h->fini_func) {
        fn = (fini_fn_t)h->fini_func;
        fn();
    }
}

static int read_file_data(const char *path, uint8_t **out_data, uint64_t *out_size) {
    vfs_node_t *node = vfs_namei(path);
    if (!node) {
        return -1;
    }
    
    uint64_t size = node->length;
    if (size == 0) {
        return -2;
    }
    
    uint8_t *data = (uint8_t *)kmalloc(size);
    if (!data) {
        return -3;
    }
    
    uint64_t read = vfs_read(node, 0, size, data);
    if (read != size) {
        kfree(data);
        return -4;
    }
    
    *out_data = data;
    *out_size = size;
    return 0;
}

static int dl_open_by_name(const char *name, int name_len) {
    int slot;
    int new_cap;
    dl_handle_t *new_arr;
    char full_path[128];
    int path_len;
    uint8_t *file_data;
    uint64_t file_size;
    int ret;
    int valid;
    uint64_t load_base;
    uint64_t pd_phys;
    int d;
    int nlen;

    init_dl();

    for (int i = 0; i < dl_capacity; i++) {
        if (dl_handles[i].in_use) {
            int match = 1;
            for (int j = 0; j < name_len; j++) {
                if (dl_handles[i].name[j] != name[j]) {
                    match = 0;
                    break;
                }
            }
            if (match && dl_handles[i].name[name_len] == '\0') {
                return (int)(i + 1);
            }
        }
    }

    slot = -1;
    for (int i = 0; i < dl_capacity; i++) {
        if (!dl_handles[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        new_cap = dl_capacity == 0 ? 4 : dl_capacity * 2;
        new_arr = (dl_handle_t *)kmalloc(new_cap * sizeof(dl_handle_t));
        if (!new_arr) {
            return 0;
        }
        memset(new_arr, 0, new_cap * sizeof(dl_handle_t));
        if (dl_handles) {
            memcpy(new_arr, dl_handles, dl_capacity * sizeof(dl_handle_t));
            kfree(dl_handles);
        }
        slot = dl_capacity;
        dl_handles = new_arr;
        dl_capacity = new_cap;
    }

    path_len = 0;
    if (name[0] == '/') {
        for (int i = 0; i < name_len && path_len < 127; i++) {
            full_path[path_len++] = name[i];
        }
    } else {
        const char *lib_prefix = "/lib/";
        for (int i = 0; i < 5; i++) {
            full_path[path_len++] = lib_prefix[i];
        }
        for (int i = 0; i < name_len && path_len < 127; i++) {
            full_path[path_len++] = name[i];
        }
    }
    full_path[path_len] = '\0';

    file_data = NULL;
    file_size = 0;
    ret = read_file_data(full_path, &file_data, &file_size);
    if (ret != 0 && name[0] != '/') {
        path_len = 0;
        const char *usr_prefix = "/usr/lib/";
        for (int i = 0; i < 9; i++) {
            full_path[path_len++] = usr_prefix[i];
        }
        for (int i = 0; i < name_len && path_len < 127; i++) {
            full_path[path_len++] = name[i];
        }
        full_path[path_len] = '\0';
        ret = read_file_data(full_path, &file_data, &file_size);
    }
    if (ret != 0) {
        return 0;
    }

    valid = elf_validate_so(file_data, file_size);
    if (valid != 0) {
        kfree(file_data);
        return 0;
    }

    load_base = dl_next_base;
    dl_next_base += 0x100000;

    pd_phys = current_task->pml4_phys;

    ret = elf_load_so(pd_phys, file_data, file_size, load_base, &dl_handles[slot]);
    kfree(file_data);

    if (ret != 0) {
        return 0;
    }

    for (int i = 0; i < name_len && i < 63; i++) {
        dl_handles[slot].name[i] = name[i];
    }
    dl_handles[slot].name[name_len < 63 ? name_len : 63] = '\0';
    dl_handles[slot].in_use = 1;

    elf_relocate_so(pd_phys, &dl_handles[slot], dl_handles, dl_capacity);

    for (d = 0; d < dl_handles[slot].needed_count; d++) {
        nlen = 0;
        while (dl_handles[slot].needed[d][nlen] && nlen < 63) nlen++;
        dl_open_by_name(dl_handles[slot].needed[d], nlen);
    }

    dl_call_init(&dl_handles[slot]);

    return (int)(slot + 1);
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
    
    uint64_t name_addr = (uint64_t)filename_ptr;
    if (name_addr >= KERNEL_VMA || name_addr < 0x1000) {
        strcpy(dl_error_msg, "dlopen: invalid filename pointer");
        return 0;
    }
    
    const char *filename = (const char *)name_addr;
    
    int slot = -1;
    for (int i = 0; i < dl_capacity; i++) {
        if (!dl_handles[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        int new_cap = dl_capacity == 0 ? 4 : dl_capacity * 2;
        dl_handle_t *new_arr = (dl_handle_t *)kmalloc(new_cap * sizeof(dl_handle_t));
        if (!new_arr) {
            strcpy(dl_error_msg, "dlopen: out of memory");
            return 0;
        }
        memset(new_arr, 0, new_cap * sizeof(dl_handle_t));
        if (dl_handles) {
            memcpy(new_arr, dl_handles, dl_capacity * sizeof(dl_handle_t));
            kfree(dl_handles);
        }
        slot = dl_capacity;
        dl_handles = new_arr;
        dl_capacity = new_cap;
    }
    
    int len = 0;
    while (filename[len] && len < 63) len++;
    
    for (int i = 0; i < dl_capacity; i++) {
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
    uint64_t file_size = 0;
    int ret = read_file_data(full_path, &file_data, &file_size);
    if (ret != 0 && filename[0] != '/') {
        path_len = 0;
        const char *usr_prefix = "/usr/lib/";
        int uprefix_len = 9;
        for (int i = 0; i < uprefix_len; i++) {
            full_path[path_len++] = usr_prefix[i];
        }
        for (int i = 0; i < len && path_len < 127; i++) {
            full_path[path_len++] = filename[i];
        }
        full_path[path_len] = '\0';
        ret = read_file_data(full_path, &file_data, &file_size);
    }
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
    
    uint64_t load_base = dl_next_base;
    dl_next_base += 0x100000;
    
    uint64_t pd_phys = current_task->pml4_phys;
    
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

    elf_relocate_so(pd_phys, &dl_handles[slot], dl_handles, dl_capacity);

    for (int d = 0; d < dl_handles[slot].needed_count; d++) {
        int nlen = 0;
        while (dl_handles[slot].needed[d][nlen] && nlen < 63) nlen++;
        dl_open_by_name(dl_handles[slot].needed[d], nlen);
    }

    dl_call_init(&dl_handles[slot]);
    
    dl_error_msg[0] = '\0';
    
    return (int)(slot + 1);
}

static int sys_dlsym(int handle, const char *symbol_ptr, int unused) {
    (void)unused;
    init_dl();
    
    if (handle <= 0 || handle > dl_capacity) {
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
    
    uint64_t sym_addr = (uint64_t)(uintptr_t)symbol_ptr;
    if (sym_addr >= KERNEL_VMA || sym_addr < 0x1000) {
        strcpy(dl_error_msg, "dlsym: invalid symbol pointer");
        return 0;
    }
    
    const char *symbol = (const char *)sym_addr;
    
    uint64_t addr = elf_so_find_symbol(&dl_handles[slot], symbol);
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
    
    if (handle <= 0 || handle > dl_capacity) {
        strcpy(dl_error_msg, "dlclose: invalid handle");
        return -1;
    }
    
    int slot = handle - 1;
    if (!dl_handles[slot].in_use) {
        strcpy(dl_error_msg, "dlclose: handle not open");
        return -1;
    }

    dl_call_fini(&dl_handles[slot]);
    
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
    
    uint64_t buf_addr = (uint64_t)buf_ptr;
    uint64_t size = (uint64_t)(uintptr_t)size_ptr;
    
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) {
        return 0;
    }
    
    if (dl_error_msg[0] == '\0') {
        return 0;
    }
    
    char *buf = (char *)buf_addr;
    int len = 0;
    while (dl_error_msg[len] && (uint64_t)len < size - 1) {
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

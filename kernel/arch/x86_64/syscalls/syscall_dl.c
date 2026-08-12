#include "syscall_defs.h"
#include <lebirun/elf.h>
#include <lebirun/vfs.h>
#include <lebirun/task.h>


#define RTLD_LAZY    0x00001
#define RTLD_NOW     0x00002
#define RTLD_GLOBAL  0x00100
#define RTLD_LOCAL   0x00000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

#define DL_BASE_ADDR USER_HIGH_DYNAMIC_BASE

static dl_handle_t *dl_handles;
static int dl_capacity = 0;
static char *dl_error_msg;
static size_t dl_error_capacity;
static int dl_initialized = 0;
static uint64_t dl_next_base = DL_BASE_ADDR;

static int dl_ensure_error(size_t needed) {
    char *msg;

    if (needed == 0) needed = 1;
    if (dl_error_msg && dl_error_capacity >= needed) return 1;
    msg = (char *)krealloc(dl_error_msg, needed);
    if (!msg) return 0;
    if (!dl_error_msg) msg[0] = '\0';
    dl_error_msg = msg;
    dl_error_capacity = needed;
    return 1;
}

static void dl_set_error(const char *msg) {
    size_t length;

    if (!msg) return;
    length = strlen(msg);
    if (length == SIZE_MAX || !dl_ensure_error(length + 1)) return;
    memcpy(dl_error_msg, msg, length + 1);
}

static void dl_clear_error(void) {
    if (dl_error_msg) dl_error_msg[0] = '\0';
}

static void init_dl(void) {
    if (dl_initialized) return;
    dl_initialized = 1;
    dl_handles = NULL;
    dl_capacity = 0;
    dl_error_msg = NULL;
    dl_error_capacity = 0;
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
    vfs_node_t *node;
    uint64_t size;
    uint8_t *data;
    uint64_t read;

    node = vfs_namei(path);
    if (!node) {
        return -1;
    }
    
    size = node->length;
    if (size == 0) {
        vfs_release(node);
        return -2;
    }
    
    data = (uint8_t *)kmalloc(size);
    if (!data) {
        vfs_release(node);
        return -3;
    }
    
    read = vfs_read(node, 0, size, data);
    vfs_release(node);
    if (read != size) {
        kfree(data);
        return -4;
    }
    
    *out_data = data;
    *out_size = size;
    return 0;
}

static char *dl_join_path(const char *prefix, const char *name,
                          size_t name_len) {
    size_t prefix_len;
    char *path;

    prefix_len = prefix ? strlen(prefix) : 0;
    if (prefix_len > SIZE_MAX - name_len - 1) return NULL;
    path = (char *)kmalloc(prefix_len + name_len + 1);
    if (!path) return NULL;
    if (prefix_len) memcpy(path, prefix, prefix_len);
    memcpy(path + prefix_len, name, name_len);
    path[prefix_len + name_len] = '\0';
    return path;
}

static int dl_copy_user_name(uint64_t address, char **out, size_t *out_len) {
    uint64_t current;
    uint64_t page_end;
    size_t length;
    char *copy;

    if (!out || !out_len || !current_task || address < 0x1000 ||
        address >= KERNEL_VMA) return -1;
    *out = NULL;
    length = 0;
    for (;;) {
        current = address + length;
        if (current < address || current >= KERNEL_VMA) return -1;
        if (!vmm_get_phys_in_pml4(current_task->pml4_phys,
                                  current & ~(PAGE_SIZE - 1))) return -1;
        page_end = (current | (PAGE_SIZE - 1)) + 1;
        while (current < page_end && current < KERNEL_VMA) {
            if (*(const char *)(uintptr_t)current == '\0') {
                if (length == SIZE_MAX) return -1;
                copy = (char *)kmalloc(length + 1);
                if (!copy) return -1;
                memcpy(copy, (const void *)(uintptr_t)address, length + 1);
                *out = copy;
                *out_len = length;
                return 0;
            }
            current++;
            length++;
        }
    }
}

static uint64_t dl_find_mapping_base(uint64_t span) {
    uint64_t candidate;
    uint64_t page;
    uint64_t end;
    uint64_t pd_phys;

    if (!current_task || span == 0 ||
        span > USER_HIGH_DYNAMIC_LIMIT - USER_HIGH_DYNAMIC_BASE) return 0;
    pd_phys = current_task->pml4_phys;
    candidate = dl_next_base;
    if (candidate < USER_HIGH_DYNAMIC_BASE) candidate = USER_HIGH_DYNAMIC_BASE;
    candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    while (candidate <= USER_HIGH_DYNAMIC_LIMIT - span) {
        end = candidate + span;
        for (page = candidate; page < end; page += PAGE_SIZE) {
            if (vmm_get_phys_in_pml4(pd_phys, page)) break;
        }
        if (page == end) return candidate;
        if (page > USER_HIGH_DYNAMIC_LIMIT - PAGE_SIZE) return 0;
        candidate = page + PAGE_SIZE;
    }
    return 0;
}

static int dl_open_by_name(const char *name, size_t name_len) {
    int slot;
    int new_cap;
    dl_handle_t *new_arr;
    char *full_path;
    char *stored_name;
    uint8_t *file_data;
    uint64_t file_size;
    int ret;
    int valid;
    uint64_t load_base;
    uint64_t mapping_span;
    uint64_t pd_phys;
    uint64_t next_base;
    uint64_t d;
    size_t nlen;
    int i;

    init_dl();

    if (!name || name_len == SIZE_MAX) return 0;
    for (i = 0; i < dl_capacity; i++) {
        if (dl_handles[i].in_use && dl_handles[i].name &&
            strlen(dl_handles[i].name) == name_len &&
            memcmp(dl_handles[i].name, name, name_len) == 0)
            return i + 1;
    }

    slot = -1;
    for (i = 0; i < dl_capacity; i++) {
        if (!dl_handles[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (dl_capacity > INT32_MAX / 2) return 0;
        new_cap = dl_capacity == 0 ? 4 : dl_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(dl_handle_t)) return 0;
        new_arr = (dl_handle_t *)kmalloc(new_cap * sizeof(dl_handle_t));
        if (!new_arr) return 0;
        memset(new_arr, 0, new_cap * sizeof(dl_handle_t));
        if (dl_handles) {
            memcpy(new_arr, dl_handles, dl_capacity * sizeof(dl_handle_t));
            kfree(dl_handles);
        }
        slot = dl_capacity;
        dl_handles = new_arr;
        dl_capacity = new_cap;
    }

    full_path = dl_join_path(name[0] == '/' ? NULL : "/lib/",
                             name, name_len);
    if (!full_path) return 0;

    file_data = NULL;
    file_size = 0;
    ret = read_file_data(full_path, &file_data, &file_size);
    if (ret != 0 && name[0] != '/') {
        kfree(full_path);
        full_path = dl_join_path("/usr/lib/", name, name_len);
        if (!full_path) return 0;
        ret = read_file_data(full_path, &file_data, &file_size);
    }
    kfree(full_path);
    if (ret != 0) {
        return 0;
    }

    valid = elf_validate_so(file_data, file_size);
    if (valid != 0) {
        kfree(file_data);
        return 0;
    }

    if (elf_so_mapping_span(file_data, file_size, &mapping_span) != 0) {
        kfree(file_data);
        return 0;
    }

    stored_name = dl_join_path(NULL, name, name_len);
    if (!stored_name) {
        kfree(file_data);
        return 0;
    }
    load_base = dl_find_mapping_base(mapping_span);
    if (!load_base) {
        kfree(stored_name);
        kfree(file_data);
        return 0;
    }
    pd_phys = current_task->pml4_phys;

    ret = elf_load_so(pd_phys, file_data, file_size, load_base, &dl_handles[slot]);
    kfree(file_data);

    if (ret != 0) {
        kfree(stored_name);
        return 0;
    }

    next_base = load_base + mapping_span;
    dl_next_base = next_base;
    dl_handles[slot].name = stored_name;
    dl_handles[slot].in_use = 1;

    elf_relocate_so(pd_phys, &dl_handles[slot], dl_handles, dl_capacity);

    for (d = 0; d < dl_handles[slot].needed_count; d++) {
        nlen = strlen(dl_handles[slot].needed[d]);
        dl_open_by_name(dl_handles[slot].needed[d], nlen);
    }

    dl_call_init(&dl_handles[slot]);

    return (int)(slot + 1);
}

static int sys_dlopen(uint64_t filename_ptr, const char *flags_ptr, int unused) {
    char *filename;
    size_t length;
    int handle;
    int flags;

    (void)unused;
    init_dl();
    flags = (int)(uintptr_t)flags_ptr;
    (void)flags;

    if (!filename_ptr) {
        dl_set_error("dlopen: NULL filename not supported");
        return 0;
    }

    if (dl_copy_user_name(filename_ptr, &filename, &length) != 0) {
        dl_set_error("dlopen: invalid filename pointer");
        return 0;
    }
    handle = dl_open_by_name(filename, length);
    kfree(filename);
    if (!handle) {
        dl_set_error("dlopen: failed to load shared object");
        return 0;
    }
    dl_clear_error();
    return handle;
}

static int64_t sys_dlsym(int handle, const char *symbol_ptr, int unused) {
    (void)unused;
    init_dl();
    
    if (handle <= 0 || handle > dl_capacity) {
        dl_set_error("dlsym: invalid handle");
        return 0;
    }
    
    int slot = handle - 1;
    if (!dl_handles[slot].in_use) {
        dl_set_error("dlsym: handle not open");
        return 0;
    }
    
    if (!symbol_ptr) {
        dl_set_error("dlsym: NULL symbol");
        return 0;
    }
    
    uint64_t sym_addr = (uint64_t)(uintptr_t)symbol_ptr;
    if (sym_addr >= KERNEL_VMA || sym_addr < 0x1000) {
        dl_set_error("dlsym: invalid symbol pointer");
        return 0;
    }
    
    const char *symbol = (const char *)sym_addr;
    
    uint64_t addr = elf_so_find_symbol(&dl_handles[slot], symbol);
    if (addr == 0) {
        dl_set_error("dlsym: symbol not found");
        return 0;
    }
    
    dl_clear_error();
    return (int64_t)addr;
}

static int sys_dlclose(int handle, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_dl();
    
    if (handle <= 0 || handle > dl_capacity) {
        dl_set_error("dlclose: invalid handle");
        return -1;
    }
    
    int slot = handle - 1;
    if (!dl_handles[slot].in_use) {
        dl_set_error("dlclose: handle not open");
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

    elf_free_needed(&dl_handles[slot]);
    
    if (dl_handles[slot].file_data) {
        kfree(dl_handles[slot].file_data);
        dl_handles[slot].file_data = NULL;
    }
    
    if (dl_handles[slot].pages) {
        kfree(dl_handles[slot].pages);
        dl_handles[slot].pages = NULL;
    }

    if (dl_handles[slot].name) {
        kfree(dl_handles[slot].name);
        dl_handles[slot].name = NULL;
    }
    
    dl_handles[slot].in_use = 0;
    
    return 0;
}

static int sys_dlerror(uint64_t buf_ptr, const char *size_ptr, int unused) {
    uint64_t buf_addr;
    uint64_t size;
    char *buf;
    int len;

    (void)unused;
    init_dl();
    buf_addr = (uint64_t)buf_ptr;
    size = (uint64_t)(uintptr_t)size_ptr;
    
    if (!buf_addr || buf_addr >= KERNEL_VMA || buf_addr < 0x1000) {
        return 0;
    }
    
    if (size == 0 || !dl_error_msg || dl_error_msg[0] == '\0') {
        return 0;
    }

    buf = (char *)buf_addr;
    len = 0;
    while (dl_error_msg[len] && (uint64_t)len < size - 1) {
        buf[len] = dl_error_msg[len];
        len++;
    }
    buf[len] = '\0';
    
    dl_clear_error();
    
    return len;
}

void syscalls_dl_init(void) {
    init_dl();
    syscall_table_set(SYSCALL_DLOPEN, (void *)(sys_dlopen));
    syscall_table_set(SYSCALL_DLSYM, (void *)(sys_dlsym));
    syscall_table_set(SYSCALL_DLCLOSE, (void *)(sys_dlclose));
    syscall_table_set(SYSCALL_DLERROR, (void *)(sys_dlerror));
}

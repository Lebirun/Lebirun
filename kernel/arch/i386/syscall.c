#include <kernel/registers.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>
#include <kernel/keyboard.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/mem_map.h>
#include <kernel/initrd.h>
#include <kernel/framebuffer.h>
#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/drivers/net/net.h>

extern mutex_t print_lock;

#define SYSCALL_EXIT 0
#define SYSCALL_WRITE 1
#define SYSCALL_GETPID 2
#define SYSCALL_READ 3
#define SYSCALL_YIELD 4
#define SYSCALL_SLEEP 5
#define SYSCALL_WAITPID 6
#define SYSCALL_SBRK 7
#define SYSCALL_MMAP 8
#define SYSCALL_KILL 9
#define SYSCALL_GETTICKS 10
#define SYSCALL_TIME 11
#define SYSCALL_ISATTY 12
#define SYSCALL_FORK 13
#define SYSCALL_EXEC 14
#define SYSCALL_INITRD_COUNT 15
#define SYSCALL_INITRD_STAT 16
#define SYSCALL_INITRD_READ 17
#define SYSCALL_OPEN 18
#define SYSCALL_CLOSE 19
#define SYSCALL_FSTAT 20
#define SYSCALL_FB_PUTPIXEL 21
#define SYSCALL_FB_SETCOLORS 22
#define SYSCALL_FB_GETINFO 23
#define SYSCALL_FB_CLEAR 24
#define SYSCALL_CONSOLE_SWITCH 25
#define SYSCALL_CONSOLE_GETCUR 26
#define SYSCALL_CONSOLE_CLEAR 27
#define SYSCALL_VFS_OPEN 28
#define SYSCALL_VFS_CLOSE 29
#define SYSCALL_VFS_READ 30
#define SYSCALL_VFS_READDIR 31
#define SYSCALL_VFS_STAT 32
#define SYSCALL_VFS_MOUNTS 33
#define SYSCALL_VFS_WRITE 34
#define SYSCALL_VFS_CREATE 35
#define SYSCALL_VFS_MKDIR 36
#define SYSCALL_VFS_UNLINK 37
#define SYSCALL_CONSOLE_SETCURSOR 38
#define SYSCALL_READ_NB 39
#define SYSCALL_SATA_TEST 40
#define SYSCALL_SATA_INFO 41
#define SYSCALL_SATA_SMART 42
#define SYSCALL_SATA_IRQ 43
#define SYSCALL_NET_IFCONFIG 44
#define SYSCALL_NET_PING 45
#define SYSCALL_NET_ARP 46
#define SYSCALL_NET_DNS 47
#define SYSCALL_NET_DHCP 48
#define SYSCALL_NET_GETINFO 49
#define SYSCALL_NET_ARP_GET 50
#define SYSCALL_NET_PING_ONE 51
#define SYSCALL_NET_DNS_RESOLVE 52

#define NR_SYSCALLS 53

static void *syscall_table[NR_SYSCALLS] = {0};

static int sys_getpid(int unused, const char *unused2, int unused3) {
    (void)unused;
    (void)unused2;
    (void)unused3;
    return (int)getpid();
}

static int sys_yield(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    schedule();
    return 0;
}

static int sys_sleep(int ms, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (ms <= 0) return -1;
    sleep_ms((uint32_t)ms);
    return 0;
}

static int sys_waitpid(int pid, const char *status_ptr, int unused) {
    (void)unused;
    if (pid <= 0) return -1;
    task_t* t = task_find((pid_t)pid);
    if (!t) return -1;
    uint32_t exit_code = 0;
    int r = task_join(t, &exit_code);
    if (r != 0) return -1;

    if (status_ptr) {
        uint32_t addr = (uint32_t)status_ptr;
        if (addr >= 0xC0000000 || addr < 0x1000) return -1;
        if (addr + sizeof(uint32_t) >= 0xC0000000) return -1;
        memcpy((void*)addr, &exit_code, sizeof(uint32_t));
    }
    return (int)pid;
}

static int sys_sbrk(int inc, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (inc == 0) {
        return (int)current_task->user_brk;
    }
    if (!current_task) return -1;
    if ((int)inc < 0) return -1;
    
    uint32_t old = current_task->user_brk;
    uint32_t newbrk = (old + (uint32_t)inc + 0xFFF) & ~0xFFFu;
    if (newbrk >= 0x007F0000 || newbrk < old) return -1;
    
    
    uint32_t start_page = (old + 0xFFF) & ~0xFFFu;
    uint32_t num_new_pages = (newbrk - start_page) / 0x1000;
    
    if (num_new_pages > 0) {

        uint32_t page_count = 0;
        uint32_t *new_pages = vmm_map_range_in_pd_tracked(
            current_task->pd_phys, start_page, newbrk - start_page, 0x7, &page_count);
        
        if (!new_pages && num_new_pages > 0) {
            return -1; 
        }
        
        if (new_pages && page_count > 0) {
            uint32_t old_count = current_task->user_pages_count;
            uint32_t new_count = old_count + page_count;
            uint32_t *expanded = (uint32_t *)kmalloc(new_count * sizeof(uint32_t));
            if (expanded) {
                if (current_task->user_pages && old_count > 0) {
                    memcpy(expanded, current_task->user_pages, old_count * sizeof(uint32_t));
                    kfree(current_task->user_pages);
                }
                memcpy(expanded + old_count, new_pages, page_count * sizeof(uint32_t));
                current_task->user_pages = expanded;
                current_task->user_pages_count = new_count;
            }
            kfree(new_pages);
        }
    }
    
    current_task->user_brk = newbrk;
    return (int)old;
}

static int sys_mmap(int len, const char *unused, int prot) {
    (void)unused; (void)prot;
    if (len <= 0 || !current_task) return -1;
    
    uint32_t base = 0x00600000;
    uint32_t size = (len + 0xFFF) & ~0xFFFu;
    
    uint32_t page_count = 0;
    uint32_t *new_pages = vmm_map_range_in_pd_tracked(
        current_task->pd_phys, base, size, 0x7, &page_count);
    
    if (!new_pages && size > 0) {
        return -1; 
    }
    
    if (new_pages && page_count > 0) {
        uint32_t old_count = current_task->user_pages_count;
        uint32_t new_count = old_count + page_count;
        uint32_t *expanded = (uint32_t *)kmalloc(new_count * sizeof(uint32_t));
        if (expanded) {
            if (current_task->user_pages && old_count > 0) {
                memcpy(expanded, current_task->user_pages, old_count * sizeof(uint32_t));
                kfree(current_task->user_pages);
            }
            memcpy(expanded + old_count, new_pages, page_count * sizeof(uint32_t));
            current_task->user_pages = expanded;
            current_task->user_pages_count = new_count;
        }
        kfree(new_pages);
    }
    
    return (int)base;
}

static int sys_kill(int pid, const char *unused, int code) {
    (void)unused;
    task_t* t = task_find((pid_t)pid);
    if (!t) return -1;
    task_kill(t, (uint32_t)code);
    return 0;
}

static int sys_getticks(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint32_t tick_count;
    return (int)tick_count;
}

static int sys_time(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    extern volatile uint32_t tick_count;
    extern uint32_t pit_freq;
    if (pit_freq == 0) return 0;
    return (int)(tick_count / pit_freq);
}

static int sys_isatty(int fd, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
}

static registers_t *fork_regs_ptr = NULL;

static int sys_fork(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    if (!fork_regs_ptr) {
        printf("sys_fork: no registers pointer\n");
        return -1;
    }
    return (int)task_fork(fork_regs_ptr);
}

static int sys_exec(int bin_ptr, const char *size_ptr, int unused) {
    (void)unused;
    uint32_t bin_addr = (uint32_t)bin_ptr;
    uint32_t bin_size = (uint32_t)(uintptr_t)size_ptr;

    if (bin_addr >= 0xC0000000 || bin_addr < 0x1000) {
        printf("sys_exec: invalid binary pointer 0x%08X\n", bin_addr);
        return -1;
    }
    if (bin_size == 0 || bin_size > 0x100000) {
        printf("sys_exec: invalid size %u\n", bin_size);
        return -1;
    }

    if (!fork_regs_ptr) {
        printf("sys_exec: no registers pointer\n");
        return -1;
    }

    return task_exec((const uint8_t *)bin_addr, bin_size, fork_regs_ptr);
}

static int sys_initrd_count(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    return (int)initrd_get_file_count();
}

static int sys_initrd_stat(int index, const char *name_buf, int len_ptr) {
    initrd_file_t *f = initrd_get_file((uint32_t)index);
    if (!f) return -1;
    
    uint32_t name_addr = (uint32_t)name_buf;
    uint32_t len_addr = (uint32_t)len_ptr;
    
    if (name_addr && name_addr < 0xC0000000 && name_addr >= 0x1000) {
        int copylen = 63;
        int i = 0;
        for (; i < copylen && f->name[i]; i++) {
            ((char*)name_addr)[i] = f->name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }
    
    if (len_addr && len_addr < 0xC0000000 && len_addr >= 0x1000) {
        *(uint32_t*)len_addr = f->length;
    }
    
    return 0;
}

static int sys_initrd_read(int index, const char *buf, int maxlen) {
    initrd_file_t *f = initrd_get_file((uint32_t)index);
    if (!f) return -1;
    
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    
    uint32_t to_copy = (uint32_t)maxlen;
    if (to_copy > f->length) to_copy = f->length;
    
    memcpy((void*)buf_addr, f->data, to_copy);
    return (int)to_copy;
}

static int sys_open(int path_ptr, const char *flags_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    
    const char *path = (const char *)path_addr;
    int flags = (int)(uintptr_t)flags_ptr;
    
    return initrd_open(path, flags);
}

static int sys_close(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    return initrd_close(fd);
}

static int sys_fstat(int fd, const char *size_ptr, int type_ptr) {
    uint32_t size_addr = (uint32_t)size_ptr;
    uint32_t type_addr = (uint32_t)type_ptr;
    
    if (fd < 0 || fd >= INITRD_MAX_FDS) return -1;
    
    extern initrd_fd_t fd_table[];
    if (!fd_table[fd].in_use) return -1;
    
    initrd_file_t *f = initrd_get_file(fd_table[fd].file_index);
    if (!f) return -1;
    
    if (size_addr && size_addr < 0xC0000000 && size_addr >= 0x1000) {
        *(uint32_t *)size_addr = f->length;
    }
    if (type_addr && type_addr < 0xC0000000 && type_addr >= 0x1000) {
        *(uint8_t *)type_addr = f->type;
    }
    
    return 0;
}

static int sys_fb_putpixel(int x, const char *y_ptr, int color) {
    uint32_t y = (uint32_t)(uintptr_t)y_ptr;
    fb_putpixel((uint32_t)x, y, (uint32_t)color);
    return 0;
}

static int sys_fb_setcolors(int fg, const char *bg_ptr, int unused) {
    (void)unused;
    uint32_t bg = (uint32_t)(uintptr_t)bg_ptr;
    fb_set_colors((uint32_t)fg, bg);
    return 0;
}

static int sys_fb_getinfo(int info_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint32_t info_addr = (uint32_t)info_ptr;
    if (info_addr >= 0xC0000000 || info_addr < 0x1000) return -1;

    framebuffer_t *fb = fb_get();
    uint32_t *info = (uint32_t *)info_addr;
    info[0] = fb->width;
    info[1] = fb->height;
    info[2] = (uint32_t)fb->bpp;
    info[3] = fb->font ? fb->font->height : 16;
    info[4] = fb->rows;
    info[5] = fb->cursor_y;
    return 0;
}

static int sys_fb_clear(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    fb_clear();
    return 0;
}

static int sys_console_switch(int console_num, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (console_num < 0 || console_num >= NUM_CONSOLES) return -1;
    console_switch(console_num);
    return 0;
}

static int sys_console_getcur(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return console_get_current();
}

static int sys_console_clear(int console_num, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (console_num < 0 || console_num >= NUM_CONSOLES) return -1;
    console_clear(console_num);
    return 0;
}

static int sys_console_setcursor(int x, const char *y_ptr, int unused) {
    (void)unused;
    int y = (int)(uintptr_t)y_ptr;
    int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
    console_setcursor(con_id, x, y);
    return 0;
}

static int sys_exit(int code, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    asm volatile("cli");
    printf("sys_exit: user task exiting with code %d\n", code);
    asm volatile("sti");
    task_exit((uint32_t)code);
    return 0;
}

static int sys_write(int fd, const char *buf, int len) {
    if ((fd != 1 && fd != 2) || !buf || len < 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len >= 0xC0000000) return -1;

    framebuffer_t *fb = fb_get();
    int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
    
    int written = 0;
    while (written < len) {
        int chunk = len - written;
        if (chunk > 64) chunk = 64; 
        
        asm volatile("cli");
        if (fb && fb->font && console_is_initialized()) {
            console_write_to(con_id, (const char *)(buf_addr + written), (size_t)chunk);
        } else {
            for (int i = 0; i < chunk; i++) {
                terminal_putchar(((const char *)buf_addr)[written + i]);
            }
        }
        asm volatile("sti");
        
        written += chunk;
    }
    return len;
}

static int sys_read(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (fd == 0) {
        int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        
        while (!keyboard_has_data_for(con_id)) {
            wait_queue_t *wq = keyboard_get_waitq_for(con_id);
            if (!wq) return -1;
            waitq_add(wq, current_task);
            block_current();
            con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        }
        clear_syscall_frame();

        int key = keyboard_getchar_nb_for(con_id);
        if (key >= 0) {
            char c = (char)key;
            memcpy((void*)buf_addr, &c, 1);
            return 1;
        }
        return 0;
    }
    
    return initrd_read(fd, (void *)buf_addr, (uint32_t)len);
}

static int sys_read_nb(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (fd == 0) {
        int con_id = (current_task && current_task->console_id >= 0) ? current_task->console_id : console_get_current();
        
        if (!keyboard_has_data_for(con_id)) {
            return 0;
        }

        int key = keyboard_getchar_nb_for(con_id);
        if (key >= 0) {
            char c = (char)key;
            memcpy((void*)buf_addr, &c, 1);
            return 1;
        }
        return 0;
    }
    
    return -1;
}

static int sys_vfs_open(int path_ptr, const char *flags_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    int flags = (int)(uintptr_t)flags_ptr;
    return vfs_open_path((const char *)path_addr, flags);
}

static int sys_vfs_close(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    return vfs_close_fd(fd);
}

static int sys_vfs_read(int fd, const char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len >= 0xC0000000) return -1;
    return vfs_read_fd(fd, (void *)buf_addr, (uint32_t)len);
}

static int sys_vfs_readdir(registers_t *regs) {
    int fd = (int)regs->ebx;
    uint32_t name_addr = regs->ecx;
    uint32_t type_addr = regs->edx;
    uint32_t index = regs->esi;
    
    if (name_addr && (name_addr >= 0xC0000000 || name_addr < 0x1000)) return -1;
    if (type_addr && (type_addr >= 0xC0000000 || type_addr < 0x1000)) return -1;
    
    dirent_t entry;
    int ret = vfs_readdir_fd(fd, &entry, index);
    if (ret < 0) return ret;
    
    if (name_addr) {
        int i = 0;
        for (; i < 63 && entry.name[i]; i++) {
            ((char*)name_addr)[i] = entry.name[i];
        }
        ((char*)name_addr)[i] = '\0';
    }
    
    if (type_addr) {
        *(uint32_t*)type_addr = entry.type;
    }
    
    return 0;
}

static int sys_vfs_stat(int fd, const char *size_ptr, int type_ptr) {
    uint32_t size_addr = (uint32_t)size_ptr;
    uint32_t type_addr = (uint32_t)type_ptr;
    uint32_t size = 0, flags = 0;
    
    int ret = vfs_stat_fd(fd, &size, &flags);
    if (ret < 0) return ret;
    
    if (size_addr && size_addr < 0xC0000000 && size_addr >= 0x1000) {
        *(uint32_t*)size_addr = size;
    }
    if (type_addr && type_addr < 0xC0000000 && type_addr >= 0x1000) {
        *(uint32_t*)type_addr = flags;
    }
    return 0;
}

static int sys_vfs_mounts(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    vfs_list_mounts();
    return vfs_get_mount_count();
}

static int sys_vfs_write(int fd, const char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len >= 0xC0000000) return -1;
    return vfs_write_fd(fd, (const void *)buf_addr, (uint32_t)len);
}

static int sys_vfs_create(int path_ptr, const char *perms_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    const char *path = (const char *)path_addr;
    uint32_t perms = (uint32_t)(uintptr_t)perms_ptr;
    
    char parent_path[256];
    char filename[64];
    int len = 0;
    while (path[len]) len++;
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash < 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        for (int i = 0; i < len && i < 63; i++) filename[i] = path[i];
        filename[len < 63 ? len : 63] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        int j = 0;
        for (int i = 1; i < len && j < 63; i++, j++) filename[j] = path[i];
        filename[j] = '\0';
    } else {
        for (int i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
        parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        int j = 0;
        for (int i = last_slash + 1; i < len && j < 63; i++, j++) filename[j] = path[i];
        filename[j] = '\0';
    }
    
    vfs_node_t *parent = vfs_namei(parent_path);
    if (!parent) return -1;
    return vfs_create(parent, filename, perms);
}

static int sys_vfs_mkdir(int path_ptr, const char *perms_ptr, int unused) {
    (void)unused;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    const char *path = (const char *)path_addr;
    uint32_t perms = (uint32_t)(uintptr_t)perms_ptr;
    
    char parent_path[256];
    char dirname[64];
    int len = 0;
    while (path[len]) len++;
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash < 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        for (int i = 0; i < len && i < 63; i++) dirname[i] = path[i];
        dirname[len < 63 ? len : 63] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        int j = 0;
        for (int i = 1; i < len && j < 63; i++, j++) dirname[j] = path[i];
        dirname[j] = '\0';
    } else {
        for (int i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
        parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        int j = 0;
        for (int i = last_slash + 1; i < len && j < 63; i++, j++) dirname[j] = path[i];
        dirname[j] = '\0';
    }
    
    vfs_node_t *parent = vfs_namei(parent_path);
    if (!parent) return -1;
    return vfs_mkdir(parent, dirname, perms);
}

static int sys_vfs_unlink(int path_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint32_t path_addr = (uint32_t)path_ptr;
    if (path_addr >= 0xC0000000 || path_addr < 0x1000) return -1;
    const char *path = (const char *)path_addr;
    
    char parent_path[256];
    char filename[64];
    int len = 0;
    while (path[len]) len++;
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash < 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        for (int i = 0; i < len && i < 63; i++) filename[i] = path[i];
        filename[len < 63 ? len : 63] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        int j = 0;
        for (int i = 1; i < len && j < 63; i++, j++) filename[j] = path[i];
        filename[j] = '\0';
    } else {
        for (int i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
        parent_path[last_slash < 255 ? last_slash : 255] = '\0';
        int j = 0;
        for (int i = last_slash + 1; i < len && j < 63; i++, j++) filename[j] = path[i];
        filename[j] = '\0';
    }
    
    vfs_node_t *parent = vfs_namei(parent_path);
    if (!parent) return -1;
    return vfs_unlink(parent, filename);
}

static int sys_sata_test(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return ahci_test_rw();
}

static int sys_sata_info(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    ahci_controller_t *ctrl = ahci_get_controller();
    if (!ctrl || !ctrl->initialized) {
        printf("SATA: No AHCI controller initialized\n");
        return -1;
    }
    ahci_debug_info();
    return (int)ctrl->num_ports;
}

static int sys_sata_smart(int port_num, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    ahci_port_t *port = ahci_get_port((uint32_t)port_num);
    if (!port) {
        printf("SMART: No SATA drive on port %d\n", port_num);
        return -1;
    }
    
    int status = ahci_smart_get_status(port);
    if (status < 0) {
        printf("SMART: Failed to get status (enabling SMART...)\n");
        if (ahci_smart_enable(port) < 0) {
            printf("SMART: Failed to enable\n");
            return -1;
        }
        status = ahci_smart_get_status(port);
    }
    
    if (status == 0) {
        printf("SMART: Drive health OK\n");
    } else if (status == 1) {
        printf("SMART: Drive health WARNING - failure predicted!\n");
    }
    
    smart_data_t data;
    if (ahci_smart_read_data(port, &data) == 0) {
        ahci_smart_print(&data);
    }
    
    return status;
}

static int sys_sata_irq(int enable, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    if (enable) {
        ahci_enable_interrupts();
    } else {
        ahci_disable_interrupts();
    }
    return 0;
}

static int sys_net_ifconfig(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    netif_t *netif = netif_get_default();
    if (!netif) {
        printf("No network interface found\n");
        return -1;
    }
    netif_print_info(netif);
    return 0;
}

static int sys_net_ping(int ip_packed, const char *unused2, int count) {
    (void)unused2;
    printf("[DEBUG] sys_net_ping called with ip=0x%08X count=%d\n", ip_packed, count);
    ipv4_addr_t target = u32_to_ipv4((uint32_t)ip_packed);
    if (count <= 0) count = 4;
    if (count > 100) count = 100;
    return ping(target, count, 3000);
}

static int sys_net_arp(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    arp_print_cache();
    return 0;
}

static int sys_net_dns(int unused, const char *hostname, int result_ptr) {
    (void)unused;
    printf("[DEBUG] sys_net_dns called with hostname=%s\n", hostname ? hostname : "(null)");
    if (!hostname) return -1;
    ipv4_addr_t resolved;
    int ret = dns_resolve(hostname, &resolved);
    if (ret == 0) {
        printf("DNS: %s -> %u.%u.%u.%u\n", hostname,
               resolved.octets[0], resolved.octets[1],
               resolved.octets[2], resolved.octets[3]);
        if (result_ptr) {
            *(uint32_t *)result_ptr = ipv4_to_u32(resolved);
        }
    } else {
        printf("DNS: Failed to resolve %s\n", hostname);
    }
    return ret;
}

static int sys_net_dhcp(int cmd, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    netif_t *netif = netif_get_default();
    if (!netif) {
        printf("No network interface found\n");
        return -1;
    }
    if (cmd == 0) {
        if (dhcp_is_bound(netif)) {
            printf("DHCP: Bound\n");
            return 1;
        } else {
            printf("DHCP: Not bound\n");
            return 0;
        }
    } else if (cmd == 1) {
        printf("DHCP: Starting...\n");
        dhcp_start(netif);
        return 0;
    }
    return -1;
}

typedef struct {
    char name[16];
    uint8_t mac[6];
    uint8_t _pad1[2];
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint32_t mtu;
    uint8_t link_up;
    uint8_t dhcp_configured;
    uint8_t _pad2[2];
} __attribute__((packed)) netinfo_user_t;

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t valid;
    uint8_t _pad;
} __attribute__((packed)) arp_entry_user_t;

static int sys_net_getinfo(int buf_ptr, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    if (!buf_ptr) return -1;
    
    netif_t *netif = netif_get_default();
    if (!netif) return -1;
    
    netinfo_user_t *info = (netinfo_user_t *)buf_ptr;
    
    for (int i = 0; i < 15 && netif->name[i]; i++) {
        info->name[i] = netif->name[i];
    }
    info->name[15] = '\0';
    
    for (int i = 0; i < 6; i++) {
        info->mac[i] = netif->mac.addr[i];
    }
    
    info->ipv4 = ipv4_to_u32(netif->ipv4);
    info->netmask = ipv4_to_u32(netif->netmask);
    info->gateway = ipv4_to_u32(netif->gateway);
    info->dns = ipv4_to_u32(netif->dns_server);
    info->mtu = netif->mtu;
    info->link_up = netif->link_up;
    info->dhcp_configured = netif->dhcp_configured;
    
    return 0;
}

extern int arp_get_cache(uint32_t *ips, uint8_t *macs, int max_entries);

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} __attribute__((packed)) arp_user_entry_t;

static int sys_net_arp_get(int buf_ptr, const char *count_ptr, int max_entries) {
    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    
    uint32_t ips[16];
    uint8_t macs[16 * 6];
    
    if (max_entries > 16) max_entries = 16;
    
    int count = arp_get_cache(ips, macs, max_entries);
    
    arp_user_entry_t *entries = (arp_user_entry_t *)buf_ptr;
    for (int i = 0; i < count; i++) {
        entries[i].ip = ips[i];
        for (int j = 0; j < 6; j++) {
            entries[i].mac[j] = macs[i * 6 + j];
        }
    }
    
    *(int *)count_ptr = count;
    
    return 0;
}

extern int ping_one(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms);

static int sys_net_ping_one(int ip_packed, const char *seq_ptr, int timeout_ms) {
    ipv4_addr_t target = u32_to_ipv4((uint32_t)ip_packed);
    uint16_t seq = (uint16_t)(int)(size_t)seq_ptr;
    if (timeout_ms <= 0) timeout_ms = 3000;
    return ping_one(target, seq, (uint32_t)timeout_ms);
}

static int sys_net_dns_resolve(int hostname_ptr, const char *result_ptr, int unused) {
    (void)unused;
    const char *hostname = (const char *)hostname_ptr;
    if (!hostname || !result_ptr) return -1;
    
    ipv4_addr_t resolved;
    int ret = dns_resolve(hostname, &resolved);
    if (ret == 0) {
        *(uint32_t *)result_ptr = ipv4_to_u32(resolved);
    }
    return ret;
}

void do_syscall(registers_t *regs) {
    int num = regs->eax;

    set_syscall_frame(regs);
    fork_regs_ptr = regs;

    if (num < 0 || num >= NR_SYSCALLS || !syscall_table[num]) {
        clear_syscall_frame();
        fork_regs_ptr = NULL;
        regs->eax = -38;
        return;
    }

    if (num == SYSCALL_VFS_READDIR) {
        regs->eax = sys_vfs_readdir(regs);
    } else {
        regs->eax = ((int (*)(int, const char *, int))syscall_table[num])(
            regs->ebx, (const char *)regs->ecx, regs->edx);
    }
    
    clear_syscall_frame();
    fork_regs_ptr = NULL;
}

void syscall_init(void) {
    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_WRITE] = sys_write;
    syscall_table[SYSCALL_GETPID] = sys_getpid;
    syscall_table[SYSCALL_READ] = sys_read;
    syscall_table[SYSCALL_YIELD] = sys_yield;
    syscall_table[SYSCALL_SLEEP] = sys_sleep;
    syscall_table[SYSCALL_WAITPID] = sys_waitpid;
    syscall_table[SYSCALL_SBRK] = sys_sbrk;
    syscall_table[SYSCALL_MMAP] = sys_mmap;
    syscall_table[SYSCALL_KILL] = sys_kill;
    syscall_table[SYSCALL_GETTICKS] = sys_getticks;
    syscall_table[SYSCALL_TIME] = sys_time;
    syscall_table[SYSCALL_ISATTY] = sys_isatty;
    syscall_table[SYSCALL_FORK] = sys_fork;
    syscall_table[SYSCALL_EXEC] = sys_exec;
    syscall_table[SYSCALL_INITRD_COUNT] = sys_initrd_count;
    syscall_table[SYSCALL_INITRD_STAT] = sys_initrd_stat;
    syscall_table[SYSCALL_INITRD_READ] = sys_initrd_read;
    syscall_table[SYSCALL_OPEN] = sys_open;
    syscall_table[SYSCALL_CLOSE] = sys_close;
    syscall_table[SYSCALL_FSTAT] = sys_fstat;
    syscall_table[SYSCALL_FB_PUTPIXEL] = sys_fb_putpixel;
    syscall_table[SYSCALL_FB_SETCOLORS] = sys_fb_setcolors;
    syscall_table[SYSCALL_FB_GETINFO] = sys_fb_getinfo;
    syscall_table[SYSCALL_FB_CLEAR] = sys_fb_clear;
    syscall_table[SYSCALL_CONSOLE_SWITCH] = sys_console_switch;
    syscall_table[SYSCALL_CONSOLE_GETCUR] = sys_console_getcur;
    syscall_table[SYSCALL_CONSOLE_CLEAR] = sys_console_clear;
    syscall_table[SYSCALL_CONSOLE_SETCURSOR] = sys_console_setcursor;
    syscall_table[SYSCALL_VFS_OPEN] = sys_vfs_open;
    syscall_table[SYSCALL_VFS_CLOSE] = sys_vfs_close;
    syscall_table[SYSCALL_VFS_READ] = sys_vfs_read;
    syscall_table[SYSCALL_VFS_READDIR] = (void*)1;
    syscall_table[SYSCALL_VFS_STAT] = sys_vfs_stat;
    syscall_table[SYSCALL_VFS_MOUNTS] = sys_vfs_mounts;
    syscall_table[SYSCALL_VFS_WRITE] = sys_vfs_write;
    syscall_table[SYSCALL_VFS_CREATE] = sys_vfs_create;
    syscall_table[SYSCALL_VFS_MKDIR] = sys_vfs_mkdir;
    syscall_table[SYSCALL_VFS_UNLINK] = sys_vfs_unlink;
    syscall_table[SYSCALL_READ_NB] = sys_read_nb;
    syscall_table[SYSCALL_SATA_TEST] = sys_sata_test;
    syscall_table[SYSCALL_SATA_INFO] = sys_sata_info;
    syscall_table[SYSCALL_SATA_SMART] = sys_sata_smart;
    syscall_table[SYSCALL_SATA_IRQ] = sys_sata_irq;
    syscall_table[SYSCALL_NET_IFCONFIG] = sys_net_ifconfig;
    syscall_table[SYSCALL_NET_PING] = sys_net_ping;
    syscall_table[SYSCALL_NET_ARP] = sys_net_arp;
    syscall_table[SYSCALL_NET_DNS] = sys_net_dns;
    syscall_table[SYSCALL_NET_DHCP] = sys_net_dhcp;
    syscall_table[SYSCALL_NET_GETINFO] = sys_net_getinfo;
    syscall_table[SYSCALL_NET_ARP_GET] = sys_net_arp_get;
    syscall_table[SYSCALL_NET_PING_ONE] = sys_net_ping_one;
    syscall_table[SYSCALL_NET_DNS_RESOLVE] = sys_net_dns_resolve;
}
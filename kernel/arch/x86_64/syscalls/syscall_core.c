#include "syscall_defs.h"
#include <lebirun/mem_map.h>
#include <lebirun/cmdline.h>
#include <lebirun/panic.h>
#include <lebirun/evdev.h>

extern mutex_t print_lock;
extern void serial_write_direct(const char *buf, size_t len);
extern int task_has_pending_signals(void);
extern int is_socket_fd(int fd);
extern int socket_write(int fd, const void *buf, int len);
extern int socket_read(int fd, void *buf, int len);
extern int is_epoll_special_fd(int fd);
extern int event_descriptor_write(int fd, const void *buffer, int length);
extern int event_descriptor_read(int fd, void *buffer, int length);

#define LINE_BUF_SIZE 256
static char **line_buffers;
static int *line_len;
static int *line_cursor;
static int *line_ready;
static int *line_last_cr;

#define HISTORY_COUNT 4
#define HISTORY_LINE_SIZE 128
#define SYS_RW_STACK_BUF 512
#define SYS_RW_HEAP_LIMIT 65536
static char ***history;
static int *history_head;
static int *history_count;
static int *history_browse;
static char **history_saved;
static int *history_saved_len;

static int *esc_state;
static char (*esc_buf)[8];
static int *esc_len;

static int syscall_core_interrupted(void) {
    return task_has_pending_signals();
}

static int *in_line_editing;
static int *serial_displayed_len;
static int syscall_console_count;

static char line_buffers_fallback[1][LINE_BUF_SIZE];
static char *line_buffers_fallback_ptr[1];
static int line_len_fallback[1];
static int line_cursor_fallback[1];
static int line_ready_fallback[1];
static int line_last_cr_fallback[1];
static int history_head_fallback[1];
static int history_count_fallback[1];
static int history_browse_fallback[1];
static int history_saved_len_fallback[1];
static int esc_state_fallback[1];
static char esc_buf_fallback[1][8];
static int esc_len_fallback[1];
static int in_line_editing_fallback[1];
static int serial_displayed_len_fallback[1];

static int syscall_core_console_count(void) {
    int count;

    count = syscall_console_count;
    if (count < 1) count = 1;
    if (count > NUM_CONSOLES) count = NUM_CONSOLES;
    return count;
}

static int syscall_core_clamp_console(int con_id) {
    int count;

    count = syscall_core_console_count();
    if (con_id < 0 || con_id >= count) con_id = console_get_current();
    if (con_id < 0 || con_id >= count) con_id = 0;
    return con_id;
}

static int syscall_core_valid_console(int con_id) {
    return con_id >= 0 && con_id < syscall_core_console_count();
}

static int history_tables_ready(void) {
    uint64_t count;

    if (history) return 1;
    count = (uint64_t)syscall_core_console_count();
    history = (char ***)kmalloc(count * sizeof(*history));
    if (!history) return 0;
    memset(history, 0, count * sizeof(*history));
    return 1;
}

static int history_console_ready(int con_id) {
    if (!syscall_core_valid_console(con_id)) return 0;
    if (!history_tables_ready()) return 0;
    if (!history[con_id]) {
        history[con_id] = (char **)kmalloc(HISTORY_COUNT * sizeof(*history[con_id]));
        if (!history[con_id]) return 0;
        memset(history[con_id], 0, HISTORY_COUNT * sizeof(*history[con_id]));
    }
    return 1;
}

static int line_buffer_ready(int con_id) {
    if (!syscall_core_valid_console(con_id)) return 0;
    if (!line_buffers) return 0;
    if (!line_buffers[con_id]) {
        line_buffers[con_id] = (char *)kmalloc(LINE_BUF_SIZE);
        if (!line_buffers[con_id]) return 0;
        memset(line_buffers[con_id], 0, LINE_BUF_SIZE);
    }
    return 1;
}

void syscall_core_flush_tty_input(int con_id) {
    if (!syscall_core_valid_console(con_id)) return;
    if (!line_buffers || !line_len || !line_cursor || !line_ready) return;
    line_len[con_id] = 0;
    line_cursor[con_id] = 0;
    line_ready[con_id] = 0;
    if (line_last_cr) line_last_cr[con_id] = 0;
    if (esc_state) esc_state[con_id] = 0;
    if (esc_len) esc_len[con_id] = 0;
    if (in_line_editing) in_line_editing[con_id] = 0;
    if (serial_displayed_len) serial_displayed_len[con_id] = 0;
    if (line_buffers[con_id]) memset(line_buffers[con_id], 0, LINE_BUF_SIZE);
}

static int syscall_core_user_range_mapped(uint64_t addr, uint64_t len) {
    uint64_t end;
    uint64_t p;
    uint64_t pd;
    uint64_t phys;
    uint64_t *new_user_pages;

    if (len == 0) return 1;
    if (!current_task) return 0;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return 0;
    if (addr + len < addr || addr + len > KERNEL_VMA) return 0;
    pd = current_task->cr3 ? current_task->cr3 : current_task->pml4_phys;
    if (!pd) return 0;

    end = addr + len - 1;
    p = addr & ~0xFFFULL;
    while (p <= end) {
        if (vmm_get_phys_in_pml4(pd, p) == 0) {
            if (!task_handle_file_page_fault(current_task, p)) {
                if ((p >= USER_STACK_FLOOR && p < USER_STACK_TOP) ||
                        (p >= current_task->user_brk && p < 0x40000000u) ||
                        (p >= 0x1000u && p < current_task->user_brk)) {
                    phys = pfa_alloc();
                    if (!phys) return 0;
                    pmm_zero_page_phys(phys);
                    vmm_map_page_in_pml4(pd, p, phys, 0x7);
                    if (vmm_get_phys_in_pml4(pd, p) == 0) {
                        pfa_free(phys);
                        return 0;
                    }
                    new_user_pages = (uint64_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint64_t));
                    if (!new_user_pages) {
                        vmm_unmap_page_in_pml4(pd, p);
                        pfa_free(phys);
                        return 0;
                    }
                    current_task->user_pages = new_user_pages;
                    current_task->user_pages[current_task->user_pages_count] = phys;
                    current_task->user_pages_count++;
                } else {
                    return 0;
                }
            }
            if (vmm_get_phys_in_pml4(pd, p) == 0) return 0;
        }
        if (p + 0x1000ULL <= p) return 0;
        p += 0x1000ULL;
    }
    return 1;
}

static void serial_write_move_back(int n) {
    char esc[16];
    int digits;
    int temp;
    int esc_len_local;
    if (n <= 0) return;
    esc[0] = '\033';
    esc[1] = '[';
    digits = 0;
    temp = n;
    do { digits++; temp /= 10; } while (temp > 0);
    esc_len_local = 2 + digits + 1;
    temp = n;
    {
        int d = 2 + digits - 1;
        do { esc[d--] = '0' + (temp % 10); temp /= 10; } while (temp > 0);
    }
    esc[2 + digits] = 'D';
    serial_write_direct(esc, (size_t)esc_len_local);
}

static void tty_echo_char(int con_id, char c) {
    framebuffer_t *fb = fb_get();
    if (fb && (fb->font || fb->cols) && console_is_initialized()) {
        if (con_id == 0 && in_line_editing[0]) {
            console_write_to_fb_only(con_id, &c, 1);
        } else {
            console_write_to(con_id, &c, 1);
        }
    } else {
        terminal_putchar(c);
    }
}

static void tty_echo_str(int con_id, const char *s, int len) {
    framebuffer_t *fb = fb_get();
    if (fb && (fb->font || fb->cols) && console_is_initialized()) {
        if (con_id == 0 && in_line_editing[0]) {
            console_write_to_fb_only(con_id, s, (size_t)len);
        } else {
            console_write_to(con_id, s, (size_t)len);
        }
    } else {
        int i;
        for (i = 0; i < len; i++) terminal_putchar(s[i]);
    }
}

static void serial_redraw_line(int con_id) {
    if (!syscall_core_valid_console(con_id)) return;
    if (con_id != 0) return;
    serial_write_move_back(serial_displayed_len[con_id]);
    serial_write_direct("\033[K", 3);
    if (line_len[con_id] > 0) {
        serial_write_direct(line_buffers[con_id], (size_t)line_len[con_id]);
    }
    serial_displayed_len[con_id] = line_len[con_id];
    if (line_cursor[con_id] < line_len[con_id]) {
        serial_write_move_back(line_len[con_id] - line_cursor[con_id]);
    }
}

static void line_redraw_from_cursor(int con_id, int echo) {
    int i;
    int trailing;
    if (!syscall_core_valid_console(con_id)) return;
    if (!echo) return;
    for (i = line_cursor[con_id]; i < line_len[con_id]; i++) {
        tty_echo_char(con_id, line_buffers[con_id][i]);
    }
    trailing = line_len[con_id] - line_cursor[con_id];
    tty_echo_char(con_id, ' ');
    for (i = 0; i <= trailing; i++) {
        tty_echo_str(con_id, "\033[D", 3);
    }
}

static void history_add(int con_id, const char *line, int len) {
    char *entry;
    char *old_entry;
    int slot;
    int copy_len;

    if (!syscall_core_valid_console(con_id)) return;
    if (len <= 0) return;
    if (len == 1 && line[0] == '\n') return;
    if (!history_console_ready(con_id)) return;
    copy_len = len;
    if (copy_len > 0 && line[copy_len - 1] == '\n') copy_len--;
    if (copy_len <= 0) return;
    if (copy_len >= HISTORY_LINE_SIZE) copy_len = HISTORY_LINE_SIZE - 1;
    entry = (char *)kmalloc((uint64_t)copy_len + 1);
    if (!entry) return;
    memcpy(entry, line, copy_len);
    entry[copy_len] = '\0';
    slot = history_head[con_id];
    old_entry = history[con_id][slot];
    history[con_id][slot] = entry;
    if (old_entry) kfree(old_entry);
    history_head[con_id] = (slot + 1) % HISTORY_COUNT;
    if (history_count[con_id] < HISTORY_COUNT) history_count[con_id]++;
}

static void history_replace_line(int con_id, const char *new_line, int new_len, int echo) {
    int i;

    if (!syscall_core_valid_console(con_id)) return;
    for (i = line_cursor[con_id]; i > 0; i--) {
        if (echo) tty_echo_str(con_id, "\033[D", 3);
    }
    for (i = 0; i < line_len[con_id]; i++) {
        if (echo) tty_echo_char(con_id, ' ');
    }
    for (i = 0; i < line_len[con_id]; i++) {
        if (echo) tty_echo_str(con_id, "\033[D", 3);
    }
    if (new_len >= LINE_BUF_SIZE) new_len = LINE_BUF_SIZE - 2;
    memcpy(line_buffers[con_id], new_line, new_len);
    line_len[con_id] = new_len;
    line_cursor[con_id] = new_len;
    if (echo) tty_echo_str(con_id, new_line, new_len);
}

static int history_save_line(int con_id, const char *line, int len) {
    char *saved;
    uint64_t count;

    if (!syscall_core_valid_console(con_id)) return 0;
    count = (uint64_t)syscall_core_console_count();
    if (!history_saved) {
        history_saved = (char **)kmalloc(count * sizeof(*history_saved));
        if (!history_saved) return 0;
        memset(history_saved, 0, count * sizeof(*history_saved));
    }
    saved = (char *)kmalloc((uint64_t)len + 1);
    if (!saved) return 0;
    if (len > 0) memcpy(saved, line, len);
    saved[len] = '\0';
    if (history_saved[con_id]) kfree(history_saved[con_id]);
    history_saved[con_id] = saved;
    history_saved_len[con_id] = len;
    return 1;
}

static int syscall_core_vfs_name_is(const char name[VFS_MAX_NAME], const char *lit) {
    size_t n;

    n = strlen(lit);
    if (n >= VFS_MAX_NAME) return 0;
    if (memcmp(name, lit, n) != 0) return 0;
    return name[n] == '\0';
}

static int syscall_core_vfs_name_is_tty(const char name[VFS_MAX_NAME]) {
    int i;

    if (syscall_core_vfs_name_is(name, "tty")) return 1;
    if (name[0] != 't' || name[1] != 't' || name[2] != 'y') return 0;
    i = 3;
    if (name[i] < '0' || name[i] > '9') return 0;
    while (name[i] >= '0' && name[i] <= '9') i++;
    return name[i] == '\0';
}

static int syscall_core_vfs_name_is_stdio_alias(const char name[VFS_MAX_NAME]) {
    if (syscall_core_vfs_name_is(name, "stdout")) return 1;
    if (syscall_core_vfs_name_is(name, "stderr")) return 1;
    return 0;
}

static int syscall_core_fd_is_console_output(int fd, task_fd_t *tfd) {
    vfs_node_t *node;
    uint64_t type;

    if (fd != 1 && fd != 2) return 0;
    if (!tfd) return 1;
    if (tfd->type == FD_TYPE_STDOUT || tfd->type == FD_TYPE_STDERR) return 1;
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return 0;
    node = (vfs_node_t *)tfd->node;
    type = VFS_GET_TYPE(node->flags);
    if (type == VFS_CHARDEVICE &&
            (syscall_core_vfs_name_is_tty(node->name) ||
             syscall_core_vfs_name_is(node->name, "console"))) {
        return 1;
    }
    if (type == VFS_SYMLINK && syscall_core_vfs_name_is_stdio_alias(node->name)) return 1;
    return 0;
}

static int sys_exit(int code, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    asm volatile("cli");
    asm volatile("sti");
    task_exit_deferred((uint64_t)code);
    schedule();
    for (;;) asm volatile ("hlt");
    return 0;
}

static int syscall_core_console_write_user(const char *buf, int len) {
    uint64_t buf_addr;
    uint64_t active_cr3;
    uint64_t expected_cr3;
    uint64_t remaining;
    uint64_t total;
    uint64_t chunk;
    uint64_t done;
    uint64_t i;
    uint8_t stack_buf[SYS_RW_STACK_BUF];
    int con_id;
    int term_chunk;
    framebuffer_t *fb;

    buf_addr = (uint64_t)buf;
    active_cr3 = read_cr3() & ~0xFFFULL;
    expected_cr3 = current_task ? current_task->pml4_phys & ~0xFFFULL : 0;
    if (expected_cr3 && active_cr3 != expected_cr3) {
        kernel_panic_custom("SYSCALL ADDRESS SPACE",
                            "pid=%d active=0x%lX expected=0x%lX",
                            current_task->pid, active_cr3, expected_cr3);
    }
    remaining = (uint64_t)len;
    total = 0;
    con_id = current_task ? current_task->console_id : 0;
    con_id = syscall_core_clamp_console(con_id);
    fb = fb_get();
    while (remaining > 0) {
        chunk = remaining;
        if (chunk > SYS_RW_STACK_BUF) chunk = SYS_RW_STACK_BUF;
        memcpy(stack_buf, (const void *)(buf_addr + total), chunk);
        if (fb && (fb->font || fb->cols) && console_is_initialized()) {
            console_write_to(con_id, (const char *)stack_buf, (size_t)chunk);
        } else {
            done = 0;
            while (done < chunk) {
                term_chunk = (int)(chunk - done);
                if (term_chunk > 64) term_chunk = 64;
                asm volatile("cli");
                for (i = 0; i < (uint64_t)term_chunk; i++) {
                    terminal_putchar((char)stack_buf[done + i]);
                }
                asm volatile("sti");
                done += (uint64_t)term_chunk;
            }
        }
        total += chunk;
        remaining -= chunk;
    }
    return len;
}

static int pipe_resize_buffer(pipe_t *pipe, uint64_t required) {
    uint8_t *new_buffer;
    uint64_t i;

    if (!pipe) return -ENOMEM;
    if (required > PIPE_BUF_SIZE) required = PIPE_BUF_SIZE;
    if (required == pipe->buf_size) return 0;
    if (required == 0) return 0;
    new_buffer = (uint8_t *)kmalloc(required);
    if (!new_buffer) return -ENOMEM;
    for (i = 0; i < pipe->count; i++) {
        new_buffer[i] = pipe->buffer[(pipe->read_pos + i) % pipe->buf_size];
    }
    kfree(pipe->buffer);
    pipe->buffer = new_buffer;
    pipe->buf_size = required;
    pipe->read_pos = 0;
    pipe->write_pos = pipe->count;
    return 0;
}

static int sys_write(int fd, const char *buf, int len) {
    uint64_t buf_addr;
    uint64_t work_size;
    uint64_t remaining;
    uint64_t total;
    uint64_t chunk;
    uint64_t done;
    uint64_t space;
    uint64_t i;
    uint64_t bytes;
    uint8_t stack_buf[SYS_RW_STACK_BUF];
    uint8_t *kbuf;
    int heap_buf;
    int result;
    int console_output;
    uint64_t pipe_flags;
    task_fd_t *tfd;
    vfs_node_t *node;
    pipe_t *p;

    if (len == 0) return 0;
    if (!buf) return -EFAULT;
    if (len < 0) return -EINVAL;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -EFAULT;
    if (!syscall_core_user_range_mapped(buf_addr, (uint64_t)len)) return -EFAULT;
    if (is_epoll_special_fd(fd))
        return event_descriptor_write(fd, buf, len);

    tfd = NULL;
    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use) {
        tfd = &current_task->fds[fd];
    }
    console_output = syscall_core_fd_is_console_output(fd, tfd);

    if (console_output) {
        return syscall_core_console_write_user(buf, len);
    }

    work_size = (uint64_t)len;
    if (work_size > SYS_RW_HEAP_LIMIT) work_size = SYS_RW_HEAP_LIMIT;
    heap_buf = 0;
    if (work_size <= SYS_RW_STACK_BUF) {
        kbuf = stack_buf;
    } else {
        kbuf = (uint8_t *)kmalloc(work_size);
        if (!kbuf) return -ENOMEM;
        heap_buf = 1;
    }

    total = 0;
    remaining = (uint64_t)len;

    if (is_socket_fd(fd)) {
        while (remaining > 0) {
            chunk = remaining;
            if (chunk > work_size) chunk = work_size;
            memcpy(kbuf, (const void *)(buf_addr + total), chunk);
            result = socket_write(fd, kbuf, (int)chunk);
            if (result <= 0) {
                if (heap_buf) kfree(kbuf);
                if (total > 0) return (int)total;
                if (result == 0) return -EIO;
                return result;
            }
            total += (uint64_t)result;
            remaining -= (uint64_t)result;
            if ((uint64_t)result < chunk) break;
        }
        if (heap_buf) kfree(kbuf);
        return (int)total;
    }

    if (tfd) {
        if (tfd->type == FD_TYPE_PIPE_W) {
            p = (pipe_t *)tfd->private_data;
            if (!p) {
                if (heap_buf) kfree(kbuf);
                return -EBADF;
            }
            while (remaining > 0) {
                chunk = remaining;
                if (chunk > work_size) chunk = work_size;
                memcpy(kbuf, (const void *)(buf_addr + total), chunk);
                done = 0;
                while (done < chunk) {
                    pipe_flags = pipe_lock_irqsave(p);
                    if (p->readers <= 0) {
                        pipe_unlock_irqrestore(p, pipe_flags);
                        if (heap_buf) kfree(kbuf);
                        if (total > 0) return (int)total;
                        return -EPIPE;
                    }
                    bytes = p->count + chunk - done;
                    if (bytes > PIPE_BUF_SIZE) bytes = PIPE_BUF_SIZE;
                    if (bytes > p->buf_size &&
                        pipe_resize_buffer(p, bytes) < 0 && p->buf_size == 0) {
                        pipe_unlock_irqrestore(p, pipe_flags);
                        if (heap_buf) kfree(kbuf);
                        if (total > 0 || done > 0) return (int)(total + done);
                        return -ENOMEM;
                    }
                    if (!p->buffer || p->buf_size == 0) {
                        pipe_unlock_irqrestore(p, pipe_flags);
                        if (heap_buf) kfree(kbuf);
                        if (total > 0 || done > 0) return (int)(total + done);
                        return -EIO;
                    }
                    while (p->count == p->buf_size) {
                        pipe_unlock_irqrestore(p, pipe_flags);
                        if (tfd->flags & VFS_O_NONBLOCK) {
                            if (heap_buf) kfree(kbuf);
                            if (total > 0 || done > 0) return (int)(total + done);
                            return -EAGAIN;
                        }
                        waitq_add(&p->write_waitq, current_task);
                        block_current();
                        if (syscall_core_interrupted()) {
                            if (heap_buf) kfree(kbuf);
                            if (total > 0) return (int)total;
                            return -EINTR;
                        }
                        pipe_flags = pipe_lock_irqsave(p);
                        if (p->readers <= 0) {
                            pipe_unlock_irqrestore(p, pipe_flags);
                            if (heap_buf) kfree(kbuf);
                            if (total > 0 || done > 0) return (int)(total + done);
                            return -EPIPE;
                        }
                    }
                    space = p->buf_size - p->count;
                    bytes = chunk - done;
                    if (bytes > space) bytes = space;
                    for (i = 0; i < bytes; i++) {
                        p->buffer[p->write_pos] = kbuf[done + i];
                        p->write_pos = (p->write_pos + 1) % p->buf_size;
                    }
                    p->count += bytes;
                    done += bytes;
                    pipe_unlock_irqrestore(p, pipe_flags);
                    waitq_wake_all(&p->read_waitq);
                }
                total += chunk;
                remaining -= chunk;
            }
            if (heap_buf) kfree(kbuf);
            return (int)total;
        }
        if (tfd->type == FD_TYPE_FILE && tfd->node) {
            node = (vfs_node_t *)tfd->node;
            if (tfd->flags & VFS_O_APPEND) {
                task_fd_position_set(tfd, node->length);
            }
            while (remaining > 0) {
                chunk = remaining;
                if (chunk > work_size) chunk = work_size;
                memcpy(kbuf, (const void *)(buf_addr + total), chunk);
                bytes = vfs_write(node, task_fd_position_get(tfd) + total, chunk, kbuf);
                if (bytes == 0) {
                    if (heap_buf) kfree(kbuf);
                    if (total > 0) return (int)total;
                    return -EIO;
                }
                total += bytes;
                remaining -= bytes;
                if (bytes < chunk) break;
            }
            task_fd_position_add(tfd, total);
            if (heap_buf) kfree(kbuf);
            return (int)total;
        }
        if (tfd->type != FD_TYPE_STDOUT && tfd->type != FD_TYPE_STDERR) {
            if (heap_buf) kfree(kbuf);
            return -EBADF;
        }
    }

    if (heap_buf) kfree(kbuf);
    return -EBADF;
}

static int sys_read(int fd, char *buf, int len) {
    uint64_t buf_addr;
    uint64_t work_size;
    uint64_t remaining;
    uint64_t total;
    uint64_t chunk;
    uint64_t avail;
    uint64_t to_read;
    uint64_t i;
    uint64_t bytes;
    uint8_t stack_buf[SYS_RW_STACK_BUF];
    uint8_t *kbuf;
    int heap_buf;
    int result;
    task_fd_t *tfd;
    vfs_node_t *node;
    pipe_t *p;
    uint64_t pipe_flags;

    if (len == 0) return 0;
    if (!buf) return -EFAULT;
    if (len < 0) return -EINVAL;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -EFAULT;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -EFAULT;
    if (!syscall_core_user_range_mapped(buf_addr, (uint64_t)len)) return -EFAULT;
    if (is_epoll_special_fd(fd))
        return event_descriptor_read(fd, buf, len);

    work_size = (uint64_t)len;
    if (work_size > SYS_RW_HEAP_LIMIT) work_size = SYS_RW_HEAP_LIMIT;
    heap_buf = 0;
    if (work_size <= SYS_RW_STACK_BUF) {
        kbuf = stack_buf;
    } else {
        kbuf = (uint8_t *)kmalloc(work_size);
        if (!kbuf) return -ENOMEM;
        heap_buf = 1;
    }
    total = 0;
    remaining = (uint64_t)len;

    if (is_socket_fd(fd)) {
        while (remaining > 0) {
            chunk = remaining;
            if (chunk > work_size) chunk = work_size;
            result = socket_read(fd, kbuf, (int)chunk);
            if (result <= 0) {
                if (heap_buf) kfree(kbuf);
                if (total > 0) return (int)total;
                return result;
            }
            memcpy((void *)(buf_addr + total), kbuf, (uint64_t)result);
            total += (uint64_t)result;
            remaining -= (uint64_t)result;
            if ((uint64_t)result < chunk) break;
        }
        if (heap_buf) kfree(kbuf);
        return (int)total;
    }

    tfd = NULL;
    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use) {
        tfd = &current_task->fds[fd];
        if (tfd->type == FD_TYPE_PIPE_R) {
            p = (pipe_t *)tfd->private_data;
            if (!p) {
                if (heap_buf) kfree(kbuf);
                return -EBADF;
            }
            pipe_flags = pipe_lock_irqsave(p);
            while (p->count == 0) {
                if (p->writers <= 0) {
                    pipe_unlock_irqrestore(p, pipe_flags);
                    if (heap_buf) kfree(kbuf);
                    return 0;
                }
                if (tfd->flags & VFS_O_NONBLOCK) {
                    pipe_unlock_irqrestore(p, pipe_flags);
                    if (heap_buf) kfree(kbuf);
                    return -EAGAIN;
                }
                pipe_unlock_irqrestore(p, pipe_flags);
                waitq_add(&p->read_waitq, current_task);
                block_current();
                if (syscall_core_interrupted()) {
                    if (heap_buf) kfree(kbuf);
                    return -EINTR;
                }
                pipe_flags = pipe_lock_irqsave(p);
            }
            if (!p->buffer || p->buf_size == 0) {
                p->count = 0;
                p->read_pos = 0;
                p->write_pos = 0;
                pipe_unlock_irqrestore(p, pipe_flags);
                if (heap_buf) kfree(kbuf);
                return -EIO;
            }
            avail = p->count;
            to_read = (uint64_t)len;
            if (to_read > avail) to_read = avail;
            if (to_read > work_size) to_read = work_size;
            for (i = 0; i < to_read; i++) {
                kbuf[i] = p->buffer[p->read_pos];
                p->read_pos = (p->read_pos + 1) % p->buf_size;
            }
            p->count -= to_read;
            pipe_unlock_irqrestore(p, pipe_flags);
            waitq_wake_all(&p->write_waitq);
            memcpy((void *)buf_addr, kbuf, to_read);
            if (heap_buf) kfree(kbuf);
            return (int)to_read;
        }
        if (tfd->type == FD_TYPE_FILE && tfd->node) {
            node = (vfs_node_t *)tfd->node;
            while (remaining > 0) {
                chunk = remaining;
                if (chunk > work_size) chunk = work_size;
                bytes = vfs_read(node, task_fd_position_get(tfd) + total, chunk, kbuf);
                if (bytes > chunk) bytes = chunk;
                if (bytes == 0) break;
                memcpy((void *)(buf_addr + total), kbuf, bytes);
                total += bytes;
                remaining -= bytes;
                if (bytes < chunk) break;
            }
            task_fd_position_add(tfd, total);
            if (heap_buf) kfree(kbuf);
            return (int)total;
        }
        if (tfd->type != FD_TYPE_STDIN) {
            if (heap_buf) kfree(kbuf);
            return -EBADF;
        }
    }

    if (heap_buf) kfree(kbuf);

    if (fd == 0) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        struct kernel_termios *t;
        int canonical;
        int echo;
        con_id = syscall_core_clamp_console(con_id);
        
        t = &tty_termios[con_id];
        canonical = (t->c_lflag & ICANON) != 0;
        echo = (t->c_lflag & ECHO) != 0;
        
        if (canonical) {
            if (!line_buffer_ready(con_id)) {
                return -ENOMEM;
            }
            if (line_ready[con_id]) {
                int to_copy = line_len[con_id];
                if (to_copy > len) to_copy = len;
                memcpy((void*)buf_addr, line_buffers[con_id], to_copy);
                
                if (to_copy < line_len[con_id]) {
                    memmove(line_buffers[con_id], line_buffers[con_id] + to_copy, 
                            line_len[con_id] - to_copy);
                    line_len[con_id] -= to_copy;
                } else {
                    line_len[con_id] = 0;
                    line_cursor[con_id] = 0;
                    line_ready[con_id] = 0;
                }
                return to_copy;
            }
            
            history_browse[con_id] = -1;
            in_line_editing[con_id] = 1;
            serial_displayed_len[con_id] = 0;
            
            while (!line_ready[con_id]) {
                int key;
                char c;
                int idx;
                int i;
                int raw_cr;
                
                while (!keyboard_has_data_for(con_id)) {
                    wait_queue_t *wq = keyboard_get_waitq_for(con_id);
                    if (!wq) return -1;
                    waitq_add(wq, current_task);
                    if (keyboard_has_data_for(con_id)) {
                        waitq_remove(wq, current_task);
                        break;
                    }
                    block_current();
                    if (syscall_core_interrupted()) {
                        if (heap_buf) kfree(kbuf);
                        return -EINTR;
                    }
                }
                
                key = keyboard_getchar_nb_for(con_id);
                if (key < 0) continue;
                
                c = (char)key;
                if (t->c_iflag & ISTRIP) c &= 0x7F;
                raw_cr = (c == '\r');
                if ((t->c_iflag & IGNCR) && c == '\r') {
                    line_last_cr[con_id] = 0;
                    continue;
                }
                if ((t->c_iflag & ICRNL) && c == '\r') {
                    c = '\n';
                } else if ((t->c_iflag & INLCR) && c == '\n') {
                    c = '\r';
                }
                if (line_last_cr[con_id] && !raw_cr && c == '\n') {
                    line_last_cr[con_id] = 0;
                    continue;
                }
                line_last_cr[con_id] = raw_cr && c == '\n';
                
                if (esc_state[con_id] == 1) {
                    if (c == '[') {
                        esc_state[con_id] = 2;
                        esc_len[con_id] = 0;
                    } else {
                        esc_state[con_id] = 0;
                    }
                    continue;
                }
                
                if (esc_state[con_id] == 2) {
                    if (c >= '0' && c <= '9') {
                        if (esc_len[con_id] < 6) {
                            esc_buf[con_id][esc_len[con_id]++] = c;
                        }
                        continue;
                    }
                    esc_state[con_id] = 0;
                    
                    if (c == 'A') {
                        if (history_count[con_id] == 0) continue;
                        if (!history_console_ready(con_id)) continue;
                        if (history_browse[con_id] < 0) {
                            if (!history_save_line(con_id, line_buffers[con_id], line_len[con_id])) continue;
                            history_browse[con_id] = 0;
                        } else if (history_browse[con_id] < history_count[con_id] - 1) {
                            history_browse[con_id]++;
                        } else {
                            continue;
                        }
                        idx = (history_head[con_id] - 1 - history_browse[con_id] + HISTORY_COUNT) % HISTORY_COUNT;
                        history_replace_line(con_id, history[con_id][idx], strlen(history[con_id][idx]), echo);
                        serial_redraw_line(con_id);
                        continue;
                    }
                    
                    if (c == 'B') {
                        if (history_browse[con_id] < 0) continue;
                        if (!history_console_ready(con_id)) continue;
                        if (!history_saved || !history_saved[con_id]) continue;
                        history_browse[con_id]--;
                        if (history_browse[con_id] < 0) {
                            history_replace_line(con_id, history_saved[con_id], history_saved_len[con_id], echo);
                        } else {
                            idx = (history_head[con_id] - 1 - history_browse[con_id] + HISTORY_COUNT) % HISTORY_COUNT;
                            history_replace_line(con_id, history[con_id][idx], strlen(history[con_id][idx]), echo);
                        }
                        serial_redraw_line(con_id);
                        continue;
                    }
                    
                    if (c == 'C') {
                        if (line_cursor[con_id] < line_len[con_id]) {
                            line_cursor[con_id]++;
                            if (echo) tty_echo_str(con_id, "\033[C", 3);
                        }
                        continue;
                    }
                    
                    if (c == 'D') {
                        if (line_cursor[con_id] > 0) {
                            line_cursor[con_id]--;
                            if (echo) tty_echo_str(con_id, "\033[D", 3);
                        }
                        continue;
                    }
                    
                    if (c == 'H') {
                        while (line_cursor[con_id] > 0) {
                            line_cursor[con_id]--;
                            if (echo) tty_echo_str(con_id, "\033[D", 3);
                        }
                        continue;
                    }
                    
                    if (c == 'F') {
                        while (line_cursor[con_id] < line_len[con_id]) {
                            line_cursor[con_id]++;
                            if (echo) tty_echo_str(con_id, "\033[C", 3);
                        }
                        continue;
                    }
                    
                    if (c == '~' && esc_len[con_id] == 1 && esc_buf[con_id][0] == '3') {
                        if (line_cursor[con_id] < line_len[con_id]) {
                            memmove(&line_buffers[con_id][line_cursor[con_id]],
                                    &line_buffers[con_id][line_cursor[con_id] + 1],
                                    line_len[con_id] - line_cursor[con_id] - 1);
                            line_len[con_id]--;
                            line_redraw_from_cursor(con_id, echo);
                            serial_redraw_line(con_id);
                        }
                        continue;
                    }
                    
                    continue;
                }
                
                if (c == '\033') {
                    esc_state[con_id] = 1;
                    esc_len[con_id] = 0;
                    continue;
                }
                
                if (c == '\b' || c == 127) {
                    if (line_cursor[con_id] > 0) {
                        memmove(&line_buffers[con_id][line_cursor[con_id] - 1],
                                &line_buffers[con_id][line_cursor[con_id]],
                                line_len[con_id] - line_cursor[con_id]);
                        line_cursor[con_id]--;
                        line_len[con_id]--;
                        if (echo) {
                            tty_echo_str(con_id, "\033[D", 3);
                            line_redraw_from_cursor(con_id, echo);
                            serial_redraw_line(con_id);
                        }
                    }
                } else if (c == '\n' || c == '\r') {
                    in_line_editing[con_id] = 0;
                    if (echo) {
                        history_add(con_id, line_buffers[con_id], line_len[con_id]);
                    }
                    if (line_len[con_id] < LINE_BUF_SIZE) {
                        line_buffers[con_id][line_len[con_id]++] = '\n';
                    }
                    line_cursor[con_id] = line_len[con_id];
                    line_ready[con_id] = 1;
                    if (echo) {
                        if (con_id == 0) {
                            serial_write_direct("\n", 1);
                            console_write_to_fb_only(con_id, "\n", 1);
                        } else {
                            tty_echo_char(con_id, '\n');
                        }
                    }
                } else if (c == 0x03) {
                    line_len[con_id] = 0;
                    line_cursor[con_id] = 0;
                    line_ready[con_id] = 0;
                    if (t->c_lflag & ISIG) {
                        int fg = tty_pgrp[con_id];
                        if (fg > 0) {
                            pid_t pids[64];
                            int npids;
                            int si;
                            npids = collect_pids_in_pgrp((pid_t)fg, pids, 64);
                            for (si = 0; si < npids; si++) {
                                task_t *tgt = task_find(pids[si]);
                                if (tgt) deliver_signal_to_task(tgt, 2);
                            }
                        }
                    }
                    in_line_editing[con_id] = 0;
                    return -EINTR;
                } else if (c == 4) {
                    in_line_editing[con_id] = 0;
                    if (line_len[con_id] == 0) {
                        return 0;
                    }
                    line_ready[con_id] = 1;
                } else if (c == 1) {
                    while (line_cursor[con_id] > 0) {
                        line_cursor[con_id]--;
                        if (echo) tty_echo_str(con_id, "\033[D", 3);
                    }
                } else if (c == 5) {
                    while (line_cursor[con_id] < line_len[con_id]) {
                        line_cursor[con_id]++;
                        if (echo) tty_echo_str(con_id, "\033[C", 3);
                    }
                } else if (c == 21) {
                    for (i = line_cursor[con_id]; i > 0; i--) {
                        if (echo) tty_echo_str(con_id, "\033[D", 3);
                    }
                    for (i = 0; i < line_len[con_id]; i++) {
                        if (echo) tty_echo_char(con_id, ' ');
                    }
                    for (i = 0; i < line_len[con_id]; i++) {
                        if (echo) tty_echo_str(con_id, "\033[D", 3);
                    }
                    line_len[con_id] = 0;
                    line_cursor[con_id] = 0;
                    serial_redraw_line(con_id);
                } else if (c >= 32 && c < 127) {
                    if (line_len[con_id] < LINE_BUF_SIZE - 2) {
                        if (line_cursor[con_id] < line_len[con_id]) {
                            memmove(&line_buffers[con_id][line_cursor[con_id] + 1],
                                    &line_buffers[con_id][line_cursor[con_id]],
                                    line_len[con_id] - line_cursor[con_id]);
                        }
                        line_buffers[con_id][line_cursor[con_id]] = c;
                        line_len[con_id]++;
                        line_cursor[con_id]++;
                        if (echo) {
                            if (line_cursor[con_id] == line_len[con_id]) {
                                tty_echo_char(con_id, c);
                            } else {
                                tty_echo_char(con_id, c);
                                line_redraw_from_cursor(con_id, echo);
                            }
                            serial_redraw_line(con_id);
                        }
                    }
                }
            }
            
            {
                int to_copy = line_len[con_id];
                if (to_copy > len) to_copy = len;
                memcpy((void*)buf_addr, line_buffers[con_id], to_copy);
                
                if (to_copy < line_len[con_id]) {
                    memmove(line_buffers[con_id], line_buffers[con_id] + to_copy, 
                            line_len[con_id] - to_copy);
                    line_len[con_id] -= to_copy;
                } else {
                    line_len[con_id] = 0;
                    line_cursor[con_id] = 0;
                    line_ready[con_id] = 0;
                }
                return to_copy;
            }
        } else {
            int total = 0;
            int vmin = t->c_cc[VMIN];
            if (vmin <= 0) vmin = 1;
            if (vmin > len) vmin = len;
            while (total < vmin) {
                while (!keyboard_has_data_for(con_id)) {
                    wait_queue_t *wq = keyboard_get_waitq_for(con_id);
                    if (!wq) return total > 0 ? total : -1;
                    waitq_add(wq, current_task);
                    if (keyboard_has_data_for(con_id)) {
                        waitq_remove(wq, current_task);
                        break;
                    }
                    block_current();
                    if (syscall_core_interrupted()) {
                        if (heap_buf) kfree(kbuf);
                        return total > 0 ? total : -EINTR;
                    }
                }
                {
                    int key = keyboard_getchar_nb_for(con_id);
                    if (key < 0) break;
                    {
                        char c = (char)key;
                        if ((t->c_lflag & ISIG) && c == 0x03) {
                            int fg = tty_pgrp[con_id];
                            if (fg > 0) {
                                pid_t pids[64];
                                int npids;
                                int si;
                                npids = collect_pids_in_pgrp((pid_t)fg, pids, 64);
                                for (si = 0; si < npids; si++) {
                                    task_t *tgt = task_find(pids[si]);
                                    if (tgt) deliver_signal_to_task(tgt, 2);
                                }
                            }
                            return total > 0 ? total : -EINTR;
                        }
                        if (t->c_iflag & ISTRIP) c &= 0x7F;
                        if ((t->c_iflag & IGNCR) && c == '\r') continue;
                        if ((t->c_iflag & ICRNL) && c == '\r') c = '\n';
                        else if ((t->c_iflag & INLCR) && c == '\n') c = '\r';
                        ((char *)buf_addr)[total] = c;
                        total++;
                    }
                }
            }
            return total;
        }
    }
    
    return initrd_read(fd, (void *)buf_addr, (uint64_t)len);
}

int syscall_core_read_for_readv(int fd, char *buf, int len) {
    return sys_read(fd, buf, len);
}

static int sys_read_nb(int fd, char *buf, int len) {
    uint64_t buf_addr;
    uint64_t bytes;
    task_fd_t *tfd;
    vfs_node_t *node;
    struct kernel_termios *t;
    uint8_t stack_buf[SYS_RW_STACK_BUF];
    int con_id;
    int key;
    char c;

    if (!buf || len <= 0) return -1;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -1;
    if (!syscall_core_user_range_mapped(buf_addr, (uint64_t)len)) return -EFAULT;

    tfd = NULL;
    if (current_task && current_task->fds && fd >= 0 &&
        fd < current_task->fds_capacity && current_task->fds[fd].in_use)
        tfd = &current_task->fds[fd];
    if (tfd && tfd->type == FD_TYPE_FILE && tfd->node) {
        node = (vfs_node_t *)tfd->node;
        if (strcmp(node->name, "event0") == 0 ||
            strcmp(node->name, "event1") == 0) {
            if (len > SYS_RW_STACK_BUF) len = SYS_RW_STACK_BUF;
            bytes = evdev_read_nonblocking(node, (uint64_t)len, stack_buf);
            if (bytes > 0) memcpy((void *)buf_addr, stack_buf, bytes);
            return (int)bytes;
        }
    }

    if (tfd && tfd->type == FD_TYPE_STDIN) {
        con_id = current_task ? current_task->console_id : console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;
        
        if (!keyboard_has_data_for(con_id)) {
            return 0;
        }

        t = &tty_termios[con_id];
        key = keyboard_getchar_nb_for(con_id);
        if (key >= 0) {
            c = (char)key;
            if (t->c_iflag & ISTRIP) c &= 0x7F;
            if ((t->c_iflag & IGNCR) && c == '\r') return 0;
            if ((t->c_iflag & ICRNL) && c == '\r') c = '\n';
            else if ((t->c_iflag & INLCR) && c == '\n') c = '\r';
            memcpy((void*)buf_addr, &c, 1);
            return 1;
        }
        return 0;
    }
    
    return -1;
}

static int vfs_name_is(const char name[VFS_MAX_NAME], const char *lit) {
    return syscall_core_vfs_name_is(name, lit);
}

static int sys_isatty(int fd, const char *unused, int unused2) {
    int t;
    (void)unused; (void)unused2;
    if (!current_task) return 0;
    if (fd < 0 || !current_task->fds || fd >= current_task->fds_capacity) return 0;
    if (!current_task->fds[fd].in_use) return 0;
    t = current_task->fds[fd].type;
    if (t == FD_TYPE_STDIN || t == FD_TYPE_STDOUT || t == FD_TYPE_STDERR) return 1;
    if (t == FD_TYPE_FILE && current_task->fds[fd].node) {
        uint64_t a;
        vfs_node_t *node = (vfs_node_t *)current_task->fds[fd].node;
        a = (uint64_t)node;
        if ((a & 0xFFFF0000u) == 0xFEFE0000u) return 0;
        if (a >= KERNEL_VMA &&
            vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), a) != 0 &&
            vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), a + (uint64_t)sizeof(vfs_node_t) - 1) != 0) {
            if (VFS_GET_TYPE(node->flags) == VFS_CHARDEVICE && (syscall_core_vfs_name_is_tty(node->name) || vfs_name_is(node->name, "console"))) return 1;
        }
    }
    return 0;
}

struct iovec {
    void *iov_base;
    uint64_t iov_len;
};

static int sys_writev(int fd, const char *iov_ptr, int iovcnt) {
    uint64_t iov_addr;
    uint64_t iov_end;
    struct iovec *iov;
    struct iovec stack_iov[16];
    uint64_t base;
    uint64_t len;
    int total;
    int written;
    int i;
    int heap_iov;

    if (!iov_ptr || iovcnt <= 0) return -EINVAL;
    if (iovcnt > 1024) return -EINVAL;
    
    iov_addr = (uint64_t)iov_ptr;
    if (iov_addr >= KERNEL_VMA || iov_addr < 0x1000) return -EFAULT;
    iov_end = iov_addr + (uint64_t)iovcnt * sizeof(struct iovec);
    if (iov_end < iov_addr || iov_end >= KERNEL_VMA) return -EFAULT;
    if (!syscall_core_user_range_mapped(iov_addr, (uint64_t)iovcnt * sizeof(struct iovec))) return -EFAULT;

    heap_iov = 0;
    if (iovcnt <= 16) {
        iov = stack_iov;
    } else {
        iov = (struct iovec *)kmalloc((uint64_t)iovcnt * sizeof(struct iovec));
        if (!iov) return -ENOMEM;
        heap_iov = 1;
    }
    memcpy(iov, (const void *)iov_addr, (uint64_t)iovcnt * sizeof(struct iovec));

    total = 0;
    
    for (i = 0; i < iovcnt; i++) {
        base = (uint64_t)iov[i].iov_base;
        len = iov[i].iov_len;

        if (len == 0) continue;
        if (len > 0x7FFFFFFFULL) {
            if (heap_iov) kfree(iov);
            return total > 0 ? total : -EINVAL;
        }
        if (base >= KERNEL_VMA || base < 0x1000) {
            if (heap_iov) kfree(iov);
            return total > 0 ? total : -EFAULT;
        }
        if (base + len < base || base + len >= KERNEL_VMA) {
            if (heap_iov) kfree(iov);
            return total > 0 ? total : -EFAULT;
        }
        if (!syscall_core_user_range_mapped(base, len)) {
            if (heap_iov) kfree(iov);
            return total > 0 ? total : -EFAULT;
        }
        
        written = sys_write(fd, (const char *)base, (int)len);
        if (written < 0) {
            if (heap_iov) kfree(iov);
            return total > 0 ? total : written;
        }
        if (written == 0) {
            if (heap_iov) kfree(iov);
            return total > 0 ? total : -EIO;
        }
        total += written;
        if ((uint64_t)written < len) {
            if (heap_iov) kfree(iov);
            return total;
        }
    }
    
    if (heap_iov) kfree(iov);
    return total;
}

static int sys_lseek(int fd, const char *offset_ptr, int whence) {
    int32_t offset = (int32_t)(uintptr_t)offset_ptr;
    if (!current_task) return -ESRCH;
    if (fd < 0 || !current_task->fds || fd >= current_task->fds_capacity || !current_task->fds[fd].in_use) return -EBADF;
    task_fd_t *tfd = &current_task->fds[fd];
    if (tfd->type != FD_TYPE_FILE || !tfd->node) return -ESPIPE;
    vfs_node_t *node = (vfs_node_t *)tfd->node;
    int32_t new_off;
    if (whence == VFS_SEEK_SET) new_off = offset;
    else if (whence == VFS_SEEK_CUR) new_off = (int32_t)task_fd_position_get(tfd) + offset;
    else if (whence == VFS_SEEK_END) new_off = (int32_t)node->length + offset;
    else return -EINVAL;
    if (new_off < 0) return -EINVAL;
    task_fd_position_set(tfd, (uint64_t)new_off);
    return (int)task_fd_position_get(tfd);
}

void syscalls_core_init(void) {
    int i;
    int count;
    uint64_t count_bytes;
    uint64_t state_bytes;
    uint64_t off;
    uint8_t *state;

    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_WRITE] = sys_write;
    syscall_table[SYSCALL_READ] = sys_read;
    syscall_table[SYSCALL_READ_NB] = sys_read_nb;
    syscall_table[SYSCALL_ISATTY] = sys_isatty;
    syscall_table[SYSCALL_WRITEV] = sys_writev;
    syscall_table[SYSCALL_LSEEK] = sys_lseek;

    count = cmdline_get_consoles();
    if (count < 1) count = 1;
    if (count > NUM_CONSOLES) count = NUM_CONSOLES;
    syscall_console_count = count;

    count_bytes = (uint64_t)count * sizeof(int);
    state_bytes = (uint64_t)count * sizeof(*line_buffers) +
                  count_bytes * 12 +
                  (uint64_t)count * sizeof(*esc_buf);
    state = (uint8_t *)kmalloc(state_bytes);

    if (state) {
        off = 0;
        line_buffers = (void *)(state + off);
        off += (uint64_t)count * sizeof(*line_buffers);
        off = (off + 7ULL) & ~7ULL;
        line_len = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        line_cursor = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        line_ready = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        line_last_cr = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        history_head = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        history_count = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        history_browse = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        history_saved_len = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        esc_state = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        esc_buf = (void *)(state + off);
        off += (uint64_t)count * sizeof(*esc_buf);
        off = (off + 7ULL) & ~7ULL;
        esc_len = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        in_line_editing = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        serial_displayed_len = (void *)(state + off);
        off += count_bytes;
        off = (off + 7ULL) & ~7ULL;
        history = NULL;
        history_saved = NULL;
    } else {
        line_buffers_fallback_ptr[0] = line_buffers_fallback[0];
        line_buffers = line_buffers_fallback_ptr;
        line_len = line_len_fallback;
        line_cursor = line_cursor_fallback;
        line_ready = line_ready_fallback;
        line_last_cr = line_last_cr_fallback;
        history_head = history_head_fallback;
        history_count = history_count_fallback;
        history_browse = history_browse_fallback;
        history_saved_len = history_saved_len_fallback;
        esc_state = esc_state_fallback;
        esc_buf = esc_buf_fallback;
        esc_len = esc_len_fallback;
        in_line_editing = in_line_editing_fallback;
        serial_displayed_len = serial_displayed_len_fallback;
        syscall_console_count = 1;
        count = 1;
        count_bytes = sizeof(int);
        history = NULL;
        history_saved = NULL;
    }
    
    memset(line_buffers, 0, (uint64_t)count * sizeof(*line_buffers));
    memset(line_len, 0, count_bytes);
    memset(line_cursor, 0, count_bytes);
    memset(line_ready, 0, count_bytes);
    memset(line_last_cr, 0, count_bytes);
    memset(history_head, 0, count_bytes);
    memset(history_count, 0, count_bytes);
    memset(history_browse, 0, count_bytes);
    memset(history_saved_len, 0, count_bytes);
    memset(esc_state, 0, count_bytes);
    memset(esc_buf, 0, (uint64_t)count * sizeof(*esc_buf));
    memset(esc_len, 0, count_bytes);
    memset(in_line_editing, 0, count_bytes);
    memset(serial_displayed_len, 0, count_bytes);

    if (line_buffers == line_buffers_fallback_ptr) {
        line_buffers[0] = line_buffers_fallback[0];
    }

    for (i = 0; i < count; i++) {
        line_len[i] = 0;
        line_cursor[i] = 0;
        line_ready[i] = 0;
        line_last_cr[i] = 0;
        history_head[i] = 0;
        history_count[i] = 0;
        history_browse[i] = -1;
        history_saved_len[i] = 0;
        esc_state[i] = 0;
        esc_len[i] = 0;
    }
}

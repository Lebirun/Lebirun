#include "syscall_defs.h"
#include <lebirun/creds.h>
#include <lebirun/debug.h>
#include <lebirun/mem_map.h>
#include <lebirun/cmdline.h>

extern mutex_t print_lock;
extern void serial_write_direct(const char *buf, size_t len);
extern int task_has_sigint_ignored(void);
extern int is_socket_fd(int fd);
extern int socket_write(int fd, const void *buf, int len);
extern int socket_read(int fd, void *buf, int len);

#define LINE_BUF_SIZE 256
static char (*line_buffers)[LINE_BUF_SIZE];
static int *line_len;
static int *line_cursor;
static int *line_ready;

#define HISTORY_COUNT 8
#define HISTORY_LINE_SIZE 128
static char (*history)[HISTORY_COUNT][HISTORY_LINE_SIZE];
static int (*history_len)[HISTORY_COUNT];
static int *history_head;
static int *history_count;
static int *history_browse;
static char (*history_saved)[HISTORY_LINE_SIZE];
static int *history_saved_len;

static int *esc_state;
static char (*esc_buf)[8];
static int *esc_len;

static int *in_line_editing;
static int *serial_displayed_len;
static int syscall_console_count;

static char line_buffers_fallback[1][LINE_BUF_SIZE];
static int line_len_fallback[1];
static int line_cursor_fallback[1];
static int line_ready_fallback[1];
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
    int slot;
    int copy_len;

    if (!history || !history_len) return;
    if (!syscall_core_valid_console(con_id)) return;
    if (len <= 0) return;
    if (len == 1 && line[0] == '\n') return;
    copy_len = len;
    if (copy_len > 0 && line[copy_len - 1] == '\n') copy_len--;
    if (copy_len <= 0) return;
    if (copy_len >= HISTORY_LINE_SIZE) copy_len = HISTORY_LINE_SIZE - 1;
    slot = history_head[con_id];
    memcpy(history[con_id][slot], line, copy_len);
    history[con_id][slot][copy_len] = '\0';
    history_len[con_id][slot] = copy_len;
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

static pid_t tty_get_my_pgrp(void) {
    pid_t my_pgrp = creds_get_pgid(0);
    if (my_pgrp == 0 && current_task) my_pgrp = current_task->pgid;
    if (my_pgrp == 0 && current_task) my_pgrp = current_task->pid;
    if (my_pgrp == 0) my_pgrp = 1;
    return my_pgrp;
}

static int sys_exit(int code, const char *unused1, int unused2) {
    (void)unused1;
    (void)unused2;
    asm volatile("cli");
    DEBUG_SYSCALL("sys_exit: user task exiting with code %d\n", code);
    asm volatile("sti");
    task_exit_deferred((uint64_t)code);
    schedule();
    for (;;) asm volatile ("hlt");
    return 0;
}

static int sys_write(int fd, const char *buf, int len) {
    uint64_t buf_addr;
    if (!buf || len < 0) return -1;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -1;

    if (is_socket_fd(fd)) {
        return socket_write(fd, (const void *)buf_addr, len);
    }

    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use) {
        task_fd_t *tfd = &current_task->fds[fd];
        if (tfd->type == FD_TYPE_PIPE_W) {
            int written;
            const uint8_t *src;
            pipe_t *p = (pipe_t *)tfd->private_data;
            if (!p) return -EBADF;
            written = 0;
            src = (const uint8_t *)buf_addr;
            while (written < len) {
                uint64_t space;
                uint64_t chunk;
                if (p->readers <= 0) return -EPIPE;
                while (p->count == p->buf_size) {
                    waitq_add(&p->write_waitq, current_task);
                    block_current();
                }
                space = p->buf_size - p->count;
                chunk = (uint64_t)(len - written);
                if (chunk > space) chunk = space;
                for (uint64_t i = 0; i < chunk; i++) {
                    p->buffer[p->write_pos] = src[written + (int)i];
                    p->write_pos = (p->write_pos + 1) % p->buf_size;
                }
                p->count += chunk;
                written += (int)chunk;
                waitq_wake_all(&p->read_waitq);
            }
            return written;
        }
        if (tfd->type == FD_TYPE_FILE && tfd->node) {
            uint64_t bytes;
            vfs_node_t *node = (vfs_node_t *)tfd->node;
            bytes = vfs_write(node, tfd->offset, (uint64_t)len, (uint8_t *)buf_addr);
            tfd->offset += bytes;
            return (int)bytes;
        }
        if (tfd->type != FD_TYPE_STDOUT && tfd->type != FD_TYPE_STDERR) {
            return -EBADF;
        }
    }

    if (fd == 1 || fd == 2) {
        int con_id;
        framebuffer_t *fb = fb_get();
        con_id = current_task ? current_task->console_id : 0;
        if (fb && (fb->font || fb->cols) && console_is_initialized()) {
            console_write_to(con_id, (const char *)buf_addr, (size_t)len);
        } else {
            int written = 0;
            while (written < len) {
                int chunk = len - written;
                if (chunk > 64) chunk = 64;
                asm volatile("cli");
                for (int i = 0; i < chunk; i++) {
                    terminal_putchar(((const char *)buf_addr)[written + i]);
                }
                asm volatile("sti");
                written += chunk;
            }
        }
        return len;
    }
    
    return -EBADF;
}

static int sys_read(int fd, char *buf, int len) {
    uint64_t buf_addr;
    if (len == 0) return 0;
    if (!buf || len < 0) return -1;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -1;

    if (is_socket_fd(fd)) {
        return socket_read(fd, (void *)buf_addr, len);
    }

    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use) {
        task_fd_t *tfd = &current_task->fds[fd];
        if (tfd->type == FD_TYPE_PIPE_R) {
            uint64_t avail;
            uint64_t to_read;
            uint8_t *dst;
            pipe_t *p = (pipe_t *)tfd->private_data;
            if (!p) return -EBADF;
            while (p->count == 0) {
                if (p->writers <= 0) return 0;
                waitq_add(&p->read_waitq, current_task);
                block_current();
            }
            avail = p->count;
            to_read = (uint64_t)len;
            if (to_read > avail) to_read = avail;
            dst = (uint8_t *)buf_addr;
            for (uint64_t i = 0; i < to_read; i++) {
                dst[i] = p->buffer[p->read_pos];
                p->read_pos = (p->read_pos + 1) % p->buf_size;
            }
            p->count -= to_read;
            waitq_wake_all(&p->write_waitq);
            return (int)to_read;
        }
        if (tfd->type == FD_TYPE_FILE && tfd->node) {
            vfs_node_t *node = (vfs_node_t *)tfd->node;
            uint64_t file_off = tfd->offset;
            uint64_t bytes;

            bytes = vfs_read(node, file_off, (uint64_t)len, (uint8_t *)buf_addr);
            tfd->offset = file_off + bytes;
            return (int)bytes;
        }
        if (tfd->type != FD_TYPE_STDIN) {
            return -EBADF;
        }
    }

    if (fd == 0) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        int fg_pgrp;
        struct kernel_termios *t;
        int canonical;
        int echo;
        con_id = syscall_core_clamp_console(con_id);

        fg_pgrp = tty_pgrp[con_id];
        if (fg_pgrp != 0) {
            pid_t my_pgrp = tty_get_my_pgrp();
            if (my_pgrp != (pid_t)fg_pgrp) {
                tty_pgrp[con_id] = (int)my_pgrp;
            }
        }
        
        t = &tty_termios[con_id];
        canonical = (t->c_lflag & ICANON) != 0;
        echo = (t->c_lflag & ECHO) != 0;
        
        if (canonical) {
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
                
                while (!keyboard_has_data_for(con_id)) {
                    wait_queue_t *wq = keyboard_get_waitq_for(con_id);
                    if (!wq) return -1;
                    waitq_add(wq, current_task);
                    if (keyboard_has_data_for(con_id)) {
                        waitq_remove(wq, current_task);
                        break;
                    }
                    block_current();
                }
                
                key = keyboard_getchar_nb_for(con_id);
                if (key < 0) continue;
                
                c = (char)key;
                
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
                        if (!history || !history_len || !history_saved) continue;
                        if (history_count[con_id] == 0) continue;
                        if (history_browse[con_id] < 0) {
                            history_browse[con_id] = 0;
                            memcpy(history_saved[con_id], line_buffers[con_id], line_len[con_id]);
                            history_saved_len[con_id] = line_len[con_id];
                        } else if (history_browse[con_id] < history_count[con_id] - 1) {
                            history_browse[con_id]++;
                        } else {
                            continue;
                        }
                        idx = (history_head[con_id] - 1 - history_browse[con_id] + HISTORY_COUNT) % HISTORY_COUNT;
                        history_replace_line(con_id, history[con_id][idx], history_len[con_id][idx], echo);
                        serial_redraw_line(con_id);
                        continue;
                    }
                    
                    if (c == 'B') {
                        if (!history || !history_len || !history_saved) continue;
                        if (history_browse[con_id] < 0) continue;
                        history_browse[con_id]--;
                        if (history_browse[con_id] < 0) {
                            history_replace_line(con_id, history_saved[con_id], history_saved_len[con_id], echo);
                        } else {
                            idx = (history_head[con_id] - 1 - history_browse[con_id] + HISTORY_COUNT) % HISTORY_COUNT;
                            history_replace_line(con_id, history[con_id][idx], history_len[con_id][idx], echo);
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
                    if (task_has_sigint_ignored()) {
                        in_line_editing[con_id] = 1;
                        serial_displayed_len[con_id] = 0;
                        continue;
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

static int sys_read_nb(int fd, char *buf, int len) {
    uint64_t buf_addr;
    if (!buf || len <= 0) return -1;
    buf_addr = (uint64_t)buf;
    if (buf_addr >= KERNEL_VMA || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint64_t)len < buf_addr || buf_addr + (uint64_t)len >= KERNEL_VMA) return -1;

    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use && current_task->fds[fd].type == FD_TYPE_STDIN) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        struct kernel_termios *t;
        int fg_pgrp;
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;

        fg_pgrp = tty_pgrp[con_id];
        if (fg_pgrp != 0) {
            pid_t my_pgrp = tty_get_my_pgrp();
            if (my_pgrp != (pid_t)fg_pgrp) {
                tty_pgrp[con_id] = (int)my_pgrp;
            }
        }
        
        if (!keyboard_has_data_for(con_id)) {
            return 0;
        }

        t = &tty_termios[con_id];
        {
            int key = keyboard_getchar_nb_for(con_id);
            if (key >= 0) {
                char c = (char)key;
                if (t->c_iflag & ISTRIP) c &= 0x7F;
                if ((t->c_iflag & IGNCR) && c == '\r') return 0;
                if ((t->c_iflag & ICRNL) && c == '\r') c = '\n';
                else if ((t->c_iflag & INLCR) && c == '\n') c = '\r';
                memcpy((void*)buf_addr, &c, 1);
                return 1;
            }
        }
        return 0;
    }
    
    return -1;
}

static int vfs_name_is(const char name[VFS_MAX_NAME], const char *lit) {
    size_t n = strlen(lit);
    if (n >= VFS_MAX_NAME) return 0;
    if (memcmp(name, lit, n) != 0) return 0;
    return name[n] == '\0';
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
            if ((node->flags & VFS_CHARDEVICE) && (vfs_name_is(node->name, "tty") || vfs_name_is(node->name, "console"))) return 1;
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
    int total;
    int i;

    if (!iov_ptr || iovcnt <= 0) return -1;
    if (iovcnt > 1024) return -EINVAL;
    
    iov_addr = (uint64_t)iov_ptr;
    if (iov_addr >= KERNEL_VMA || iov_addr < 0x1000) return -1;
    iov_end = iov_addr + (uint64_t)iovcnt * sizeof(struct iovec);
    if (iov_end < iov_addr || iov_end >= KERNEL_VMA) return -1;
    
    iov = (struct iovec *)iov_addr;
    total = 0;
    
    for (i = 0; i < iovcnt; i++) {
        uint64_t base = (uint64_t)iov[i].iov_base;
        uint64_t len = iov[i].iov_len;
        int written;
        
        if (base >= KERNEL_VMA || base < 0x1000) continue;
        if (base + len < base || base + len >= KERNEL_VMA) continue;
        
        written = sys_write(fd, (const char *)base, (int)len);
        if (written > 0) total += written;
    }
    
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
    else if (whence == VFS_SEEK_CUR) new_off = (int32_t)tfd->offset + offset;
    else if (whence == VFS_SEEK_END) new_off = (int32_t)node->length + offset;
    else return -EINVAL;
    if (new_off < 0) return -EINVAL;
    tfd->offset = (uint64_t)new_off;
    return (int)tfd->offset;
}

void syscalls_core_init(void) {
    int i;
    int count;
    uint64_t count_bytes;
    uint64_t history_slots;

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
    history_slots = (uint64_t)count + 1;

    line_buffers = kmalloc((uint64_t)count * sizeof(*line_buffers));
    line_len = kmalloc(count_bytes);
    line_cursor = kmalloc(count_bytes);
    line_ready = kmalloc(count_bytes);
    history_head = kmalloc(count_bytes);
    history_count = kmalloc(count_bytes);
    history_browse = kmalloc(count_bytes);
    history_saved_len = kmalloc(count_bytes);
    esc_state = kmalloc(count_bytes);
    esc_buf = kmalloc((uint64_t)count * sizeof(*esc_buf));
    esc_len = kmalloc(count_bytes);
    in_line_editing = kmalloc(count_bytes);
    serial_displayed_len = kmalloc(count_bytes);

    if (!line_buffers || !line_len || !line_cursor || !line_ready ||
        !history_head || !history_count || !history_browse ||
        !history_saved_len || !esc_state || !esc_buf || !esc_len ||
        !in_line_editing || !serial_displayed_len) {
        if (line_buffers) kfree(line_buffers);
        if (line_len) kfree(line_len);
        if (line_cursor) kfree(line_cursor);
        if (line_ready) kfree(line_ready);
        if (history_head) kfree(history_head);
        if (history_count) kfree(history_count);
        if (history_browse) kfree(history_browse);
        if (history_saved_len) kfree(history_saved_len);
        if (esc_state) kfree(esc_state);
        if (esc_buf) kfree(esc_buf);
        if (esc_len) kfree(esc_len);
        if (in_line_editing) kfree(in_line_editing);
        if (serial_displayed_len) kfree(serial_displayed_len);
        line_buffers = line_buffers_fallback;
        line_len = line_len_fallback;
        line_cursor = line_cursor_fallback;
        line_ready = line_ready_fallback;
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
    }
    
    memset(line_buffers, 0, (uint64_t)count * sizeof(*line_buffers));
    memset(line_len, 0, count_bytes);
    memset(line_cursor, 0, count_bytes);
    memset(line_ready, 0, count_bytes);
    memset(history_head, 0, count_bytes);
    memset(history_count, 0, count_bytes);
    memset(history_browse, 0, count_bytes);
    memset(history_saved_len, 0, count_bytes);
    memset(esc_state, 0, count_bytes);
    memset(esc_buf, 0, (uint64_t)count * sizeof(*esc_buf));
    memset(esc_len, 0, count_bytes);
    memset(in_line_editing, 0, count_bytes);
    memset(serial_displayed_len, 0, count_bytes);

    for (i = 0; i < count; i++) {
        line_len[i] = 0;
        line_cursor[i] = 0;
        line_ready[i] = 0;
        history_head[i] = 0;
        history_count[i] = 0;
        history_browse[i] = -1;
        history_saved_len[i] = 0;
        esc_state[i] = 0;
        esc_len[i] = 0;
    }

    history = kmalloc(history_slots * sizeof(*history));
    if (history) memset(history, 0, history_slots * sizeof(*history));
    history_len = kmalloc(history_slots * sizeof(*history_len));
    if (history_len) memset(history_len, 0, history_slots * sizeof(*history_len));
    history_saved = kmalloc(history_slots * sizeof(*history_saved));
    if (history_saved) memset(history_saved, 0, history_slots * sizeof(*history_saved));
}

#include "syscall_defs.h"
#include <kernel/creds.h>
#include <kernel/debug.h>

extern mutex_t print_lock;

#define LINE_BUF_SIZE 256
static char line_buffers[NUM_CONSOLES][LINE_BUF_SIZE];
static int line_len[NUM_CONSOLES];
static int line_cursor[NUM_CONSOLES];
static int line_ready[NUM_CONSOLES];

#define HISTORY_COUNT 16
#define HISTORY_LINE_SIZE 128
static char history[NUM_CONSOLES][HISTORY_COUNT][HISTORY_LINE_SIZE];
static int history_len[NUM_CONSOLES][HISTORY_COUNT];
static int history_head[NUM_CONSOLES];
static int history_count[NUM_CONSOLES];
static int history_browse[NUM_CONSOLES];
static char history_saved[NUM_CONSOLES][HISTORY_LINE_SIZE];
static int history_saved_len[NUM_CONSOLES];

static int esc_state[NUM_CONSOLES];
static char esc_buf[NUM_CONSOLES][8];
static int esc_len[NUM_CONSOLES];

static void tty_echo_char(int con_id, char c) {
    framebuffer_t *fb = fb_get();
    if (fb && fb->font && console_is_initialized()) {
        console_write_to(con_id, &c, 1);
    } else {
        terminal_putchar(c);
    }
}

static void tty_echo_str(int con_id, const char *s, int len) {
    framebuffer_t *fb = fb_get();
    if (fb && fb->font && console_is_initialized()) {
        console_write_to(con_id, s, (size_t)len);
    } else {
        int i;
        for (i = 0; i < len; i++) terminal_putchar(s[i]);
    }
}

static void line_redraw_from_cursor(int con_id, int echo) {
    int i;
    int trailing;
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
    task_exit_deferred((uint32_t)code);
    schedule();
    for (;;) asm volatile ("hlt");
    return 0;
}

static int sys_write(int fd, const char *buf, int len) {
    if (!buf || len < 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len < buf_addr || buf_addr + (uint32_t)len >= 0xC0000000) return -1;

    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use) {
        task_fd_t *tfd = &current_task->fds[fd];
        if (tfd->type == FD_TYPE_PIPE_W) {
            pipe_t *p = (pipe_t *)tfd->private_data;
            if (!p) return -EBADF;
            int written = 0;
            const uint8_t *src = (const uint8_t *)buf_addr;
            while (written < len) {
                if (p->readers <= 0) return -EPIPE;
                while (p->count == PIPE_BUF_SIZE) {
                    waitq_add(&p->write_waitq, current_task);
                    block_current();
                }
                uint32_t space = PIPE_BUF_SIZE - p->count;
                uint32_t chunk = (uint32_t)(len - written);
                if (chunk > space) chunk = space;
                for (uint32_t i = 0; i < chunk; i++) {
                    p->buffer[p->write_pos] = src[written + (int)i];
                    p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
                }
                p->count += chunk;
                written += (int)chunk;
                waitq_wake_all(&p->read_waitq);
            }
            return written;
        }
        if (tfd->type == FD_TYPE_FILE && tfd->node) {
            vfs_node_t *node = (vfs_node_t *)tfd->node;
            uint32_t bytes = vfs_write(node, tfd->offset, (uint32_t)len, (uint8_t *)buf_addr);
            tfd->offset += bytes;
            return (int)bytes;
        }
        if (tfd->type != FD_TYPE_STDOUT && tfd->type != FD_TYPE_STDERR) {
            return -EBADF;
        }
    }

    if (fd == 1 || fd == 2) {
        framebuffer_t *fb = fb_get();
        int con_id = current_task ? current_task->console_id : 0;
        if (fb && fb->font && console_is_initialized()) {
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
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len < buf_addr || buf_addr + (uint32_t)len >= 0xC0000000) return -1;

    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use) {
        task_fd_t *tfd = &current_task->fds[fd];
        if (tfd->type == FD_TYPE_PIPE_R) {
            pipe_t *p = (pipe_t *)tfd->private_data;
            if (!p) return -EBADF;
            while (p->count == 0) {
                if (p->writers <= 0) return 0;
                waitq_add(&p->read_waitq, current_task);
                block_current();
            }
            uint32_t avail = p->count;
            uint32_t to_read = (uint32_t)len;
            if (to_read > avail) to_read = avail;
            uint8_t *dst = (uint8_t *)buf_addr;
            for (uint32_t i = 0; i < to_read; i++) {
                dst[i] = p->buffer[p->read_pos];
                p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
            }
            p->count -= to_read;
            waitq_wake_all(&p->write_waitq);
            return (int)to_read;
        }
        if (tfd->type == FD_TYPE_FILE && tfd->node) {
            vfs_node_t *node = (vfs_node_t *)tfd->node;
            uint32_t bytes = vfs_read(node, tfd->offset, (uint32_t)len, (uint8_t *)buf_addr);
            tfd->offset += bytes;
            return (int)bytes;
        }
        if (tfd->type != FD_TYPE_STDIN) {
            return -EBADF;
        }
    }

    if (fd == 0) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;

        int fg_pgrp = tty_pgrp[con_id];
        if (fg_pgrp != 0) {
            pid_t my_pgrp = tty_get_my_pgrp();
            if (my_pgrp != (pid_t)fg_pgrp) {
                tty_pgrp[con_id] = (int)my_pgrp;
            }
        }
        
        struct kernel_termios *t = &tty_termios[con_id];
        int canonical = (t->c_lflag & ICANON) != 0;
        int echo = (t->c_lflag & ECHO) != 0;
        
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
                        continue;
                    }
                    
                    if (c == 'B') {
                        if (history_browse[con_id] < 0) continue;
                        history_browse[con_id]--;
                        if (history_browse[con_id] < 0) {
                            history_replace_line(con_id, history_saved[con_id], history_saved_len[con_id], echo);
                        } else {
                            idx = (history_head[con_id] - 1 - history_browse[con_id] + HISTORY_COUNT) % HISTORY_COUNT;
                            history_replace_line(con_id, history[con_id][idx], history_len[con_id][idx], echo);
                        }
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
                        }
                    }
                } else if (c == '\n' || c == '\r') {
                    history_add(con_id, line_buffers[con_id], line_len[con_id]);
                    if (line_len[con_id] < LINE_BUF_SIZE) {
                        line_buffers[con_id][line_len[con_id]++] = '\n';
                    }
                    line_cursor[con_id] = line_len[con_id];
                    line_ready[con_id] = 1;
                    if (echo) {
                        tty_echo_char(con_id, '\n');
                    }
                } else if (c == 0x03) {
                    line_len[con_id] = 0;
                    line_cursor[con_id] = 0;
                    line_ready[con_id] = 0;
                    if (echo) {
                        tty_echo_char(con_id, '^');
                        tty_echo_char(con_id, 'C');
                        tty_echo_char(con_id, '\n');
                    }
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
                    return -EINTR;
                } else if (c == 4) {
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
    
    return initrd_read(fd, (void *)buf_addr, (uint32_t)len);
}

static int sys_read_nb(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + (uint32_t)len < buf_addr || buf_addr + (uint32_t)len >= 0xC0000000) return -1;

    if (current_task && current_task->fds && fd >= 0 && fd < current_task->fds_capacity && current_task->fds[fd].in_use && current_task->fds[fd].type == FD_TYPE_STDIN) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        struct kernel_termios *t;
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;

        int fg_pgrp = tty_pgrp[con_id];
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
    (void)unused; (void)unused2;
    if (!current_task) return 0;
    if (fd < 0 || !current_task->fds || fd >= current_task->fds_capacity) return 0;
    if (!current_task->fds[fd].in_use) return 0;
    int t = current_task->fds[fd].type;
    if (t == FD_TYPE_STDIN || t == FD_TYPE_STDOUT || t == FD_TYPE_STDERR) return 1;
    if (t == FD_TYPE_FILE && current_task->fds[fd].node) {
        vfs_node_t *node = (vfs_node_t *)current_task->fds[fd].node;
        uint32_t a = (uint32_t)node;
        if ((a & 0xFFFF0000u) == 0xFEFE0000u) return 0;
        if (a >= 0xC0000000 &&
            vmm_get_phys_in_pd(vmm_get_kernel_cr3(), a) != 0 &&
            vmm_get_phys_in_pd(vmm_get_kernel_cr3(), a + (uint32_t)sizeof(vfs_node_t) - 1) != 0) {
            if ((node->flags & VFS_CHARDEVICE) && (vfs_name_is(node->name, "tty") || vfs_name_is(node->name, "console"))) return 1;
        }
    }
    return 0;
}

struct iovec {
    void *iov_base;
    uint32_t iov_len;
};

static int sys_writev(int fd, const char *iov_ptr, int iovcnt) {
    uint32_t iov_addr;
    uint32_t iov_end;
    struct iovec *iov;
    int total;
    int i;

    if (!iov_ptr || iovcnt <= 0) return -1;
    if (iovcnt > 1024) return -EINVAL;
    
    iov_addr = (uint32_t)iov_ptr;
    if (iov_addr >= 0xC0000000 || iov_addr < 0x1000) return -1;
    iov_end = iov_addr + (uint32_t)iovcnt * sizeof(struct iovec);
    if (iov_end < iov_addr || iov_end >= 0xC0000000) return -1;
    
    iov = (struct iovec *)iov_addr;
    total = 0;
    
    for (i = 0; i < iovcnt; i++) {
        uint32_t base = (uint32_t)iov[i].iov_base;
        uint32_t len = iov[i].iov_len;
        int written;
        
        if (base >= 0xC0000000 || base < 0x1000) continue;
        if (base + len < base || base + len >= 0xC0000000) continue;
        
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
    tfd->offset = (uint32_t)new_off;
    return (int)tfd->offset;
}

void syscalls_core_init(void) {
    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_WRITE] = sys_write;
    syscall_table[SYSCALL_READ] = sys_read;
    syscall_table[SYSCALL_READ_NB] = sys_read_nb;
    syscall_table[SYSCALL_ISATTY] = sys_isatty;
    syscall_table[SYSCALL_WRITEV] = sys_writev;
    syscall_table[SYSCALL_LSEEK] = sys_lseek;
    
    for (int i = 0; i < NUM_CONSOLES; i++) {
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
}

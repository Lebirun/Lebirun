#include "syscall_defs.h"
#include <kernel/creds.h>
#include <kernel/debug.h>

extern mutex_t print_lock;

#define LINE_BUF_SIZE 128
static char line_buffers[NUM_CONSOLES][LINE_BUF_SIZE];
static int line_pos[NUM_CONSOLES];
static int line_ready[NUM_CONSOLES];

static void tty_echo_char(int con_id, char c) {
    framebuffer_t *fb = fb_get();
    if (fb && fb->font && console_is_initialized()) {
        console_write_to(con_id, &c, 1);
    } else {
        terminal_putchar(c);
    }
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
    if (buf_addr + (uint32_t)len >= 0xC0000000) return -1;

    if (current_task && fd >= 0 && fd < TASK_MAX_FDS && current_task->fds[fd].in_use) {
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
        
        int written = 0;
        while (written < len) {
            int chunk = len - written;
            if (chunk > 64) chunk = 64;
            
            if (fb && fb->font && console_is_initialized()) {
                console_write_to(con_id, (const char *)(buf_addr + written), (size_t)chunk);
            } else {
                asm volatile("cli");
                for (int i = 0; i < chunk; i++) {
                    terminal_putchar(((const char *)buf_addr)[written + i]);
                }
                asm volatile("sti");
            }
            
            written += chunk;
        }
        return len;
    }
    
    return -EBADF;
}

static int sys_read(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (current_task && fd >= 0 && fd < TASK_MAX_FDS && current_task->fds[fd].in_use) {
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
                if (current_task) {
                    current_task->state = TASK_BLOCKED;
                    schedule();
                }
                return -EINTR;
            }
        }
        
        struct kernel_termios *t = &tty_termios[con_id];
        int canonical = (t->c_lflag & ICANON) != 0;
        int echo = (t->c_lflag & ECHO) != 0;
        
        if (canonical) {
            if (line_ready[con_id]) {
                int to_copy = line_pos[con_id];
                if (to_copy > len) to_copy = len;
                memcpy((void*)buf_addr, line_buffers[con_id], to_copy);
                
                if (to_copy < line_pos[con_id]) {
                    memmove(line_buffers[con_id], line_buffers[con_id] + to_copy, 
                            line_pos[con_id] - to_copy);
                    line_pos[con_id] -= to_copy;
                } else {
                    line_pos[con_id] = 0;
                    line_ready[con_id] = 0;
                }
                return to_copy;
            }
            
            while (!line_ready[con_id]) {
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
                
                int key = keyboard_getchar_nb_for(con_id);
                if (key < 0) continue;
                
                char c = (char)key;
                
                if (c == '\b' || c == 127) {
                    if (line_pos[con_id] > 0) {
                        line_pos[con_id]--;
                        if (echo) {
                            tty_echo_char(con_id, '\b');
                            tty_echo_char(con_id, ' ');
                            tty_echo_char(con_id, '\b');
                        }
                    }
                } else if (c == '\n' || c == '\r') {
                    if (line_pos[con_id] < LINE_BUF_SIZE) {
                        line_buffers[con_id][line_pos[con_id]++] = '\n';
                    }
                    line_ready[con_id] = 1;
                    if (echo) {
                        tty_echo_char(con_id, '\n');
                    }
                } else if (c == 0x03) {
                    line_pos[con_id] = 0;
                    line_ready[con_id] = 0;
                    if (echo) {
                        tty_echo_char(con_id, '^');
                        tty_echo_char(con_id, 'C');
                        tty_echo_char(con_id, '\n');
                    }
                    return 0;
                } else if (c == 4) {
                    if (line_pos[con_id] == 0) {
                        return 0;
                    }
                    line_ready[con_id] = 1;
                } else if (c >= 32 && c < 127) {
                    if (line_pos[con_id] < LINE_BUF_SIZE - 1) {
                        line_buffers[con_id][line_pos[con_id]++] = c;
                        if (echo) {
                            tty_echo_char(con_id, c);
                        }
                    }
                }
            }
            
            int to_copy = line_pos[con_id];
            if (to_copy > len) to_copy = len;
            memcpy((void*)buf_addr, line_buffers[con_id], to_copy);
            
            if (to_copy < line_pos[con_id]) {
                memmove(line_buffers[con_id], line_buffers[con_id] + to_copy, 
                        line_pos[con_id] - to_copy);
                line_pos[con_id] -= to_copy;
            } else {
                line_pos[con_id] = 0;
                line_ready[con_id] = 0;
            }
            return to_copy;
        } else {
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

            int key = keyboard_getchar_nb_for(con_id);
            if (key >= 0) {
                char c = (char)key;
                memcpy((void*)buf_addr, &c, 1);
                return 1;
            }
            return 0;
        }
    }
    
    return initrd_read(fd, (void *)buf_addr, (uint32_t)len);
}

static int sys_read_nb(int fd, char *buf, int len) {
    if (!buf || len <= 0) return -1;
    uint32_t buf_addr = (uint32_t)buf;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    if (buf_addr + len >= 0xC0000000) return -1;

    if (current_task && fd >= 0 && fd < TASK_MAX_FDS && current_task->fds[fd].in_use && current_task->fds[fd].type == FD_TYPE_STDIN) {
        int con_id = current_task ? current_task->console_id : console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = console_get_current();
        if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;

        int fg_pgrp = tty_pgrp[con_id];
        if (fg_pgrp != 0) {
            pid_t my_pgrp = tty_get_my_pgrp();
            if (my_pgrp != (pid_t)fg_pgrp) {
                return -EAGAIN;
            }
        }
        
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

static int vfs_name_is(const char name[VFS_MAX_NAME], const char *lit) {
    size_t n = strlen(lit);
    if (n >= VFS_MAX_NAME) return 0;
    if (memcmp(name, lit, n) != 0) return 0;
    return name[n] == '\0';
}

static int sys_isatty(int fd, const char *unused, int unused2) {
    (void)unused; (void)unused2;
    if (!current_task) return 0;
    if (fd < 0 || fd >= TASK_MAX_FDS) return 0;
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
    if (!iov_ptr || iovcnt <= 0) return -1;
    
    uint32_t iov_addr = (uint32_t)iov_ptr;
    if (iov_addr >= 0xC0000000 || iov_addr < 0x1000) return -1;
    
    struct iovec *iov = (struct iovec *)iov_addr;
    int total = 0;
    
    for (int i = 0; i < iovcnt; i++) {
        uint32_t base = (uint32_t)iov[i].iov_base;
        uint32_t len = iov[i].iov_len;
        
        if (base >= 0xC0000000 || base < 0x1000) continue;
        if (base + len >= 0xC0000000) continue;
        
        int written = sys_write(fd, (const char *)base, (int)len);
        if (written > 0) total += written;
    }
    
    return total;
}

static int sys_lseek(int fd, const char *offset_ptr, int whence) {
    int32_t offset = (int32_t)(uintptr_t)offset_ptr;
    if (!current_task) return -ESRCH;
    if (fd < 0 || fd >= TASK_MAX_FDS || !current_task->fds[fd].in_use) return -EBADF;
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
        line_pos[i] = 0;
        line_ready[i] = 0;
    }
}

#include <kernel/pty.h>
#include <kernel/tty.h>
#include <kernel/task.h>
#include <kernel/mutex.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MAX_PTYS 4
#define PTY_BUF_SIZE 128

typedef struct {
    int in_use;
    int master_fd;
    int slave_fd;
    uint8_t master_buf[PTY_BUF_SIZE];
    uint32_t master_head;
    uint32_t master_tail;
    uint8_t slave_buf[PTY_BUF_SIZE];
    uint32_t slave_head;
    uint32_t slave_tail;
    struct termios termios;
    struct winsize winsize;
    pid_t session;
    pid_t pgrp;
    int master_closed;
    int slave_closed;
    mutex_t lock;
} pty_t;

static pty_t ptys[MAX_PTYS];
static int pty_base_master = 200;
static int pty_base_slave = 300;
static mutex_t pty_lock;

static void init_default_termios(struct termios *t) {
    memset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = B38400 | CS8 | CREAD | HUPCL;
    t->c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
    t->c_cc[VINTR] = 3;
    t->c_cc[VQUIT] = 28;
    t->c_cc[VERASE] = 127;
    t->c_cc[VKILL] = 21;
    t->c_cc[VEOF] = 4;
    t->c_cc[VTIME] = 0;
    t->c_cc[VMIN] = 1;
    t->c_cc[VSTART] = 17;
    t->c_cc[VSTOP] = 19;
    t->c_cc[VSUSP] = 26;
    t->c_cc[VEOL] = 0;
    t->c_cc[VREPRINT] = 18;
    t->c_cc[VDISCARD] = 15;
    t->c_cc[VWERASE] = 23;
    t->c_cc[VLNEXT] = 22;
    t->c_cc[VEOL2] = 0;
}

static int alloc_pty(void) {
    int i;
    for (i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].in_use) {
            memset(&ptys[i], 0, sizeof(pty_t));
            ptys[i].in_use = 1;
            ptys[i].master_fd = pty_base_master + i;
            ptys[i].slave_fd = pty_base_slave + i;
            init_default_termios(&ptys[i].termios);
            ptys[i].winsize.ws_row = 24;
            ptys[i].winsize.ws_col = 80;
            mutex_init(&ptys[i].lock);
            return i;
        }
    }
    return -1;
}

static pty_t *get_pty_by_master(int fd) {
    int idx = fd - pty_base_master;
    if (idx < 0 || idx >= MAX_PTYS) return NULL;
    if (!ptys[idx].in_use) return NULL;
    return &ptys[idx];
}

static pty_t *get_pty_by_slave(int fd) {
    int idx = fd - pty_base_slave;
    if (idx < 0 || idx >= MAX_PTYS) return NULL;
    if (!ptys[idx].in_use) return NULL;
    return &ptys[idx];
}

int pty_open_master(void) {
    mutex_lock(&pty_lock);
    int idx = alloc_pty();
    if (idx < 0) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    int fd = ptys[idx].master_fd;
    mutex_unlock(&pty_lock);
    return fd;
}

int pty_open_slave(int master_fd) {
    pty_t *pty = get_pty_by_master(master_fd);
    if (!pty) return -1;
    return pty->slave_fd;
}

int pty_grant(int master_fd) {
    pty_t *pty = get_pty_by_master(master_fd);
    if (!pty) return -1;
    return 0;
}

int pty_unlock(int master_fd) {
    pty_t *pty = get_pty_by_master(master_fd);
    if (!pty) return -1;
    return 0;
}

char *pty_name(int master_fd) {
    pty_t *pty = get_pty_by_master(master_fd);
    if (!pty) return NULL;
    static char name[32];
    int idx = master_fd - pty_base_master;
    strcpy(name, "/dev/pts/");
    int len = 9;
    if (idx == 0) {
        name[len++] = '0';
    } else {
        char tmp[8];
        int i = 0;
        int t = idx;
        while (t > 0) {
            tmp[i++] = '0' + (t % 10);
            t /= 10;
        }
        while (i > 0) {
            name[len++] = tmp[--i];
        }
    }
    name[len] = '\0';
    return name;
}

static size_t buf_used(uint32_t head, uint32_t tail) {
    return tail - head;
}

static size_t buf_free(uint32_t head, uint32_t tail, size_t size) {
    return size - buf_used(head, tail);
}

ssize_t pty_master_read(int fd, void *buf, size_t count) {
    pty_t *pty;
    size_t available;
    size_t to_read;
    uint8_t *dst;
    size_t i;

    pty = get_pty_by_master(fd);
    if (!pty) return -1;
    
    mutex_lock(&pty->lock);
    
    available = buf_used(pty->slave_head, pty->slave_tail);
    if (available == 0) {
        mutex_unlock(&pty->lock);
        if (pty->slave_closed) return 0;
        return -11;
    }
    
    to_read = (count < available) ? count : available;
    dst = (uint8_t *)buf;
    
    for (i = 0; i < to_read; i++) {
        dst[i] = pty->slave_buf[pty->slave_head % PTY_BUF_SIZE];
        pty->slave_head++;
    }
    
    mutex_unlock(&pty->lock);
    return to_read;
}

ssize_t pty_master_write(int fd, const void *buf, size_t count) {
    pty_t *pty;
    size_t space;
    size_t to_write;
    const uint8_t *src;
    size_t i;
    uint8_t c;

    pty = get_pty_by_master(fd);
    if (!pty) return -1;
    
    if (pty->slave_closed) return -32;
    
    mutex_lock(&pty->lock);
    
    space = buf_free(pty->master_head, pty->master_tail, PTY_BUF_SIZE);
    to_write = (count < space) ? count : space;
    src = (const uint8_t *)buf;
    
    for (i = 0; i < to_write; i++) {
        c = src[i];
        
        if (pty->termios.c_lflag & ISIG) {
            if (c == pty->termios.c_cc[VINTR]) {
                mutex_unlock(&pty->lock);
                return to_write;
            }
            if (c == pty->termios.c_cc[VQUIT]) {
                mutex_unlock(&pty->lock);
                return to_write;
            }
            if (c == pty->termios.c_cc[VSUSP]) {
                mutex_unlock(&pty->lock);
                return to_write;
            }
        }
        
        pty->master_buf[pty->master_tail % PTY_BUF_SIZE] = c;
        pty->master_tail++;
    }
    
    mutex_unlock(&pty->lock);
    return to_write;
}

ssize_t pty_slave_read(int fd, void *buf, size_t count) {
    pty_t *pty;
    size_t available;
    uint8_t *dst;
    size_t read_count;
    int found_line;
    size_t line_end;
    size_t i;
    uint8_t c;
    size_t to_read;
    cc_t vmin;

    pty = get_pty_by_slave(fd);
    if (!pty) return -1;
    
    if (pty->master_closed) return 0;
    
    mutex_lock(&pty->lock);
    
    available = buf_used(pty->master_head, pty->master_tail);
    if (available == 0) {
        mutex_unlock(&pty->lock);
        return -11;
    }
    
    dst = (uint8_t *)buf;
    read_count = 0;
    
    if (pty->termios.c_lflag & ICANON) {
        found_line = 0;
        line_end = 0;
        
        for (i = 0; i < available && i < PTY_BUF_SIZE; i++) {
            c = pty->master_buf[(pty->master_head + i) % PTY_BUF_SIZE];
            if (c == '\n' || c == pty->termios.c_cc[VEOF] || c == pty->termios.c_cc[VEOL]) {
                found_line = 1;
                line_end = i + 1;
                break;
            }
        }
        
        if (!found_line) {
            mutex_unlock(&pty->lock);
            return -11;
        }
        
        to_read = (count < line_end) ? count : line_end;
        for (i = 0; i < to_read; i++) {
            dst[i] = pty->master_buf[pty->master_head % PTY_BUF_SIZE];
            pty->master_head++;
        }
        read_count = to_read;
    } else {
        vmin = pty->termios.c_cc[VMIN];
        
        if (vmin > 0 && available < vmin) {
            mutex_unlock(&pty->lock);
            return -11;
        }
        
        to_read = (count < available) ? count : available;
        for (i = 0; i < to_read; i++) {
            dst[i] = pty->master_buf[pty->master_head % PTY_BUF_SIZE];
            pty->master_head++;
        }
        read_count = to_read;
    }
    
    mutex_unlock(&pty->lock);
    return read_count;
}

ssize_t pty_slave_write(int fd, const void *buf, size_t count) {
    pty_t *pty;
    size_t space;
    size_t written;
    const uint8_t *src;
    size_t i;
    uint8_t c;

    pty = get_pty_by_slave(fd);
    if (!pty) return -1;
    
    if (pty->master_closed) return -32;
    
    mutex_lock(&pty->lock);
    
    space = buf_free(pty->slave_head, pty->slave_tail, PTY_BUF_SIZE);
    written = 0;
    src = (const uint8_t *)buf;
    
    for (i = 0; i < count && written < space; i++) {
        c = src[i];
        
        if (pty->termios.c_oflag & OPOST) {
            if (c == '\n' && (pty->termios.c_oflag & ONLCR)) {
                if (space - written >= 2) {
                    pty->slave_buf[pty->slave_tail % PTY_BUF_SIZE] = '\r';
                    pty->slave_tail++;
                    pty->slave_buf[pty->slave_tail % PTY_BUF_SIZE] = '\n';
                    pty->slave_tail++;
                    written += 2;
                } else {
                    break;
                }
            } else {
                pty->slave_buf[pty->slave_tail % PTY_BUF_SIZE] = c;
                pty->slave_tail++;
                written++;
            }
        } else {
            pty->slave_buf[pty->slave_tail % PTY_BUF_SIZE] = c;
            pty->slave_tail++;
            written++;
        }
    }
    
    mutex_unlock(&pty->lock);
    return count;
}

int pty_ioctl(int fd, unsigned long request, void *arg) {
    pty_t *pty = get_pty_by_master(fd);
    if (!pty) pty = get_pty_by_slave(fd);
    if (!pty) return -1;
    
    switch (request) {
        case TCGETS:
            if (arg) memcpy(arg, &pty->termios, sizeof(struct termios));
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            if (arg) memcpy(&pty->termios, arg, sizeof(struct termios));
            return 0;
        case TIOCGWINSZ:
            if (arg) memcpy(arg, &pty->winsize, sizeof(struct winsize));
            return 0;
        case TIOCSWINSZ:
            if (arg) memcpy(&pty->winsize, arg, sizeof(struct winsize));
            return 0;
        case TIOCGPGRP:
            if (arg) *(pid_t *)arg = pty->pgrp;
            return 0;
        case TIOCSPGRP:
            if (arg) pty->pgrp = *(pid_t *)arg;
            return 0;
        case TIOCGSID:
            if (arg) *(pid_t *)arg = pty->session;
            return 0;
        case TIOCSCTTY:
            pty->session = current_task->pid;
            pty->pgrp = current_task->pid;
            return 0;
        case TIOCNOTTY:
            pty->session = 0;
            pty->pgrp = 0;
            return 0;
        default:
            return -22;
    }
}

int pty_close_master(int fd) {
    pty_t *pty = get_pty_by_master(fd);
    if (!pty) return -1;
    
    mutex_lock(&pty->lock);
    pty->master_closed = 1;
    
    if (pty->slave_closed) {
        pty->in_use = 0;
    }
    
    mutex_unlock(&pty->lock);
    return 0;
}

int pty_close_slave(int fd) {
    pty_t *pty = get_pty_by_slave(fd);
    if (!pty) return -1;
    
    mutex_lock(&pty->lock);
    pty->slave_closed = 1;
    
    if (pty->master_closed) {
        pty->in_use = 0;
    }
    
    mutex_unlock(&pty->lock);
    return 0;
}

int is_pty_master(int fd) {
    return get_pty_by_master(fd) != NULL;
}

int is_pty_slave(int fd) {
    return get_pty_by_slave(fd) != NULL;
}

void pty_init(void) {
    memset(ptys, 0, sizeof(ptys));
    mutex_init(&pty_lock);
}

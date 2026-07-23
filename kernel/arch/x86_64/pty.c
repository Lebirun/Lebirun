#include <lebirun/pty.h>
#include <lebirun/tty.h>
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/mem_map.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PTY_INIT_COUNT 1
#define PTY_BUF_SIZE 4096

typedef struct {
    int in_use;
    uint16_t master_capacity;
    uint16_t slave_capacity;
    uint8_t *master_buf;
    uint64_t master_head;
    uint64_t master_tail;
    uint8_t *slave_buf;
    uint64_t slave_head;
    uint64_t slave_tail;
    struct termios termios;
    struct winsize winsize;
    pid_t session;
    pid_t pgrp;
    int input_cr_pending;
    int master_closed;
    int slave_closed;
    mutex_t lock;
} pty_t;

static pty_t *ptys = NULL;
static int pty_capacity = 0;
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

static int pty_grow(void) {
    int new_cap;
    int i;
    pty_t *new_arr;

    new_cap = pty_capacity ? pty_capacity * 2 : PTY_INIT_COUNT;
    new_arr = (pty_t *)krealloc(ptys, new_cap * sizeof(pty_t));
    if (!new_arr) return -1;
    for (i = pty_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(pty_t));
    }
    ptys = new_arr;
    pty_capacity = new_cap;
    return 0;
}

static int alloc_pty(void) {
    int i;

    for (i = 0; i < pty_capacity; i++) {
        if (!ptys[i].in_use) goto found;
    }
    if (pty_grow() < 0) return -1;
    i = pty_capacity / 2;
found:
    memset(&ptys[i], 0, sizeof(pty_t));
    ptys[i].in_use = 1;
    init_default_termios(&ptys[i].termios);
    ptys[i].winsize.ws_row = 24;
    ptys[i].winsize.ws_col = 80;
    mutex_init(&ptys[i].lock);
    return i;
}

static int pty_reserve_buffer(uint8_t **buffer, uint16_t *capacity,
                              uint64_t *head, uint64_t *tail,
                              size_t additional) {
    uint64_t used;
    uint64_t required;
    uint16_t new_capacity;
    uint8_t *new_buffer;
    uint64_t i;

    used = *tail - *head;
    if (used > PTY_BUF_SIZE) return -12;
    if (additional > PTY_BUF_SIZE - used) return -12;
    required = used + additional;
    if (required <= *capacity) return 0;
    new_capacity = (uint16_t)required;
    new_buffer = (uint8_t *)kmalloc(new_capacity);
    if (!new_buffer) return -12;
    for (i = 0; i < used; i++) {
        new_buffer[i] = (*buffer)[(*head + i) % *capacity];
    }
    kfree(*buffer);
    *buffer = new_buffer;
    *capacity = new_capacity;
    *head = 0;
    *tail = used;
    return 0;
}

static void pty_compact_buffer(uint8_t **buffer, uint16_t *capacity,
                               uint64_t *head, uint64_t *tail) {
    uint64_t used;
    uint8_t *new_buffer;
    uint64_t i;

    used = *tail - *head;
    if (used == *capacity) return;
    if (used == 0) {
        kfree(*buffer);
        *buffer = NULL;
        *capacity = 0;
        *head = 0;
        *tail = 0;
        return;
    }
    new_buffer = (uint8_t *)kmalloc(used);
    if (!new_buffer) return;
    for (i = 0; i < used; i++) {
        new_buffer[i] = (*buffer)[(*head + i) % *capacity];
    }
    kfree(*buffer);
    *buffer = new_buffer;
    *capacity = (uint16_t)used;
    *head = 0;
    *tail = used;
}

static int pty_ensure_master_buf(pty_t *pty, size_t additional) {
    return pty_reserve_buffer(&pty->master_buf, &pty->master_capacity,
                              &pty->master_head, &pty->master_tail,
                              additional);
}

static int pty_ensure_slave_buf(pty_t *pty, size_t additional) {
    return pty_reserve_buffer(&pty->slave_buf, &pty->slave_capacity,
                              &pty->slave_head, &pty->slave_tail,
                              additional);
}

static pty_t *get_pty_by_master(int fd) {
    int idx = fd - pty_base_master;
    if (idx < 0 || idx >= pty_capacity) return NULL;
    if (!ptys[idx].in_use) return NULL;
    return &ptys[idx];
}

static pty_t *get_pty_by_slave(int fd) {
    int idx = fd - pty_base_slave;
    if (idx < 0 || idx >= pty_capacity) return NULL;
    if (!ptys[idx].in_use) return NULL;
    return &ptys[idx];
}

int pty_open_master(void) {
    int idx;
    int fd;
    mutex_lock(&pty_lock);
    idx = alloc_pty();
    if (idx < 0) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    fd = pty_base_master + idx;
    mutex_unlock(&pty_lock);
    return fd;
}

int pty_open_slave(int master_fd) {
    pty_t *pty = get_pty_by_master(master_fd);
    if (!pty) return -1;
    return pty_base_slave + (int)(pty - ptys);
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
    static char name[32];
    int idx;
    int len;
    pty_t *pty = get_pty_by_master(master_fd);
    if (!pty) return NULL;
    idx = master_fd - pty_base_master;
    strcpy(name, "/dev/pts/");
    len = 9;
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

static size_t buf_used(uint64_t head, uint64_t tail) {
    return tail - head;
}

static size_t buf_free(uint64_t head, uint64_t tail, size_t size) {
    return size - buf_used(head, tail);
}

ssize_t pty_master_read(int fd, void *buf, size_t count) {
    size_t available;
    size_t to_read;
    uint8_t *dst;
    size_t i;
    pty_t *pty;

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
        dst[i] = pty->slave_buf[pty->slave_head % pty->slave_capacity];
        pty->slave_head++;
    }
    pty_compact_buffer(&pty->slave_buf, &pty->slave_capacity,
                       &pty->slave_head, &pty->slave_tail);
    
    mutex_unlock(&pty->lock);
    return to_read;
}

ssize_t pty_master_write(int fd, const void *buf, size_t count) {
    size_t space;
    size_t to_write;
    const uint8_t *src;
    size_t i;
    uint8_t c;
    pty_t *pty;
    int raw_cr;
    int cr_newline;
    pid_t pids[64];
    task_t *signal_task;
    int npids;
    int si;

    pty = get_pty_by_master(fd);
    if (!pty) return -1;
    
    if (pty->slave_closed) return -32;
    
    mutex_lock(&pty->lock);
    
    space = buf_free(pty->master_head, pty->master_tail, PTY_BUF_SIZE);
    to_write = (count < space) ? count : space;
    if (to_write > 0 && pty_ensure_master_buf(pty, to_write) < 0) {
        mutex_unlock(&pty->lock);
        return -12;
    }
    src = (const uint8_t *)buf;
    
    for (i = 0; i < to_write; i++) {
        c = src[i];
        if (pty->termios.c_iflag & ISTRIP) c &= 0x7F;
        raw_cr = (c == '\r');
        if ((pty->termios.c_iflag & IGNCR) && c == '\r') {
            pty->input_cr_pending = 0;
            continue;
        }
        if ((pty->termios.c_iflag & ICRNL) && c == '\r') {
            c = '\n';
        } else if ((pty->termios.c_iflag & INLCR) && c == '\n') {
            c = '\r';
        }
        if (pty->input_cr_pending && !raw_cr && c == '\n') {
            pty->input_cr_pending = 0;
            continue;
        }
        cr_newline = raw_cr && c == '\n';
        pty->input_cr_pending = cr_newline;
        
        if (pty->termios.c_lflag & ISIG) {
            if (c == pty->termios.c_cc[VINTR]) {
                if (pty->pgrp > 0) {
                    npids = collect_pids_in_pgrp(pty->pgrp, pids, 64);
                    for (si = 0; si < npids; si++) {
                        signal_task = task_find(pids[si]);
                        if (signal_task) deliver_signal_to_task(signal_task, 2);
                    }
                }
                pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                                   &pty->master_head, &pty->master_tail);
                mutex_unlock(&pty->lock);
                return to_write;
            }
            if (c == pty->termios.c_cc[VQUIT]) {
                if (pty->pgrp > 0) {
                    npids = collect_pids_in_pgrp(pty->pgrp, pids, 64);
                    for (si = 0; si < npids; si++) {
                        signal_task = task_find(pids[si]);
                        if (signal_task) deliver_signal_to_task(signal_task, 3);
                    }
                }
                pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                                   &pty->master_head, &pty->master_tail);
                mutex_unlock(&pty->lock);
                return to_write;
            }
            if (c == pty->termios.c_cc[VSUSP]) {
                if (pty->pgrp > 0) {
                    npids = collect_pids_in_pgrp(pty->pgrp, pids, 64);
                    for (si = 0; si < npids; si++) {
                        signal_task = task_find(pids[si]);
                        if (signal_task) deliver_signal_to_task(signal_task, 20);
                    }
                }
                pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                                   &pty->master_head, &pty->master_tail);
                mutex_unlock(&pty->lock);
                return to_write;
            }
        }
        
        pty->master_buf[pty->master_tail % pty->master_capacity] = c;
        pty->master_tail++;
    }
    pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                       &pty->master_head, &pty->master_tail);
    
    mutex_unlock(&pty->lock);
    return to_write;
}

ssize_t pty_slave_read(int fd, void *buf, size_t count) {
    size_t available;
    uint8_t *dst;
    size_t read_count;
    int found_line;
    size_t line_end;
    size_t i;
    uint8_t c;
    size_t to_read;
    pty_t *pty;
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
            c = pty->master_buf[(pty->master_head + i) % pty->master_capacity];
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
            dst[i] = pty->master_buf[pty->master_head % pty->master_capacity];
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
            dst[i] = pty->master_buf[pty->master_head % pty->master_capacity];
            pty->master_head++;
        }
        read_count = to_read;
    }
    pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                       &pty->master_head, &pty->master_tail);
    
    mutex_unlock(&pty->lock);
    return read_count;
}

ssize_t pty_slave_write(int fd, const void *buf, size_t count) {
    size_t space;
    size_t consumed;
    size_t output_size;
    size_t character_size;
    const uint8_t *src;
    size_t i;
    uint8_t c;
    pty_t *pty;

    pty = get_pty_by_slave(fd);
    if (!pty) return -1;
    
    if (pty->master_closed) return -32;
    
    mutex_lock(&pty->lock);
    
    space = buf_free(pty->slave_head, pty->slave_tail, PTY_BUF_SIZE);
    consumed = 0;
    output_size = 0;
    src = (const uint8_t *)buf;

    for (i = 0; i < count; i++) {
        c = src[i];
        character_size = (pty->termios.c_oflag & OPOST) &&
                         c == '\n' && (pty->termios.c_oflag & ONLCR) ? 2 : 1;
        if (character_size > space - output_size) break;
        output_size += character_size;
        consumed++;
    }
    if (output_size > 0 && pty_ensure_slave_buf(pty, output_size) < 0) {
        mutex_unlock(&pty->lock);
        return -12;
    }
    for (i = 0; i < consumed; i++) {
        c = src[i];
        if ((pty->termios.c_oflag & OPOST) && c == '\n' &&
            (pty->termios.c_oflag & ONLCR)) {
            pty->slave_buf[pty->slave_tail % pty->slave_capacity] = '\r';
            pty->slave_tail++;
            pty->slave_buf[pty->slave_tail % pty->slave_capacity] = '\n';
            pty->slave_tail++;
        } else {
            pty->slave_buf[pty->slave_tail % pty->slave_capacity] = c;
            pty->slave_tail++;
        }
    }

    mutex_unlock(&pty->lock);
    return consumed;
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
            if (pty->pgrp > 0) {
                pid_t pids[64];
                int npids;
                int si;

                npids = collect_pids_in_pgrp(pty->pgrp, pids, 64);
                for (si = 0; si < npids; si++) {
                    task_t *t = task_find(pids[si]);
                    if (t) deliver_signal_to_task(t, 28);
                }
            }
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
        kfree(pty->master_buf);
        kfree(pty->slave_buf);
        pty->master_buf = NULL;
        pty->slave_buf = NULL;
        pty->master_capacity = 0;
        pty->slave_capacity = 0;
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
        kfree(pty->master_buf);
        kfree(pty->slave_buf);
        pty->master_buf = NULL;
        pty->slave_buf = NULL;
        pty->master_capacity = 0;
        pty->slave_capacity = 0;
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

int pty_has_data_for_master(int fd) {
    size_t available;
    pty_t *pty;

    pty = get_pty_by_master(fd);
    if (!pty) return 0;
    available = buf_used(pty->slave_head, pty->slave_tail);
    if (available > 0) return 1;
    if (pty->slave_closed) return 1;
    return 0;
}

int pty_has_data_for_slave(int fd) {
    size_t available;
    pty_t *pty;

    pty = get_pty_by_slave(fd);
    if (!pty) return 0;
    available = buf_used(pty->master_head, pty->master_tail);
    if (available > 0) return 1;
    if (pty->master_closed) return 1;
    return 0;
}

void KERNEL_INIT pty_init(void) {
    pty_capacity = 0;
    ptys = NULL;
    mutex_init(&pty_lock);
}

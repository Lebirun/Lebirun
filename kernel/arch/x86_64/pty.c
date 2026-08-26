#include <lebirun/pty.h>
#include <lebirun/tty.h>
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PTY_INIT_COUNT 1
typedef struct {
    int in_use;
    int master_refs;
    int slave_refs;
    int unlocked;
    size_t master_capacity;
    size_t slave_capacity;
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
    t->c_cc[VERASE] = 8;
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
    ptys[i].master_refs = 1;
    ptys[i].slave_refs = -1;
    init_default_termios(&ptys[i].termios);
    ptys[i].winsize.ws_row = 24;
    ptys[i].winsize.ws_col = 80;
    mutex_init(&ptys[i].lock);
    return i;
}

static int pty_reserve_buffer(uint8_t **buffer, size_t *capacity,
                              uint64_t *head, uint64_t *tail,
                              size_t additional) {
    uint64_t used;
    uint64_t required;
    size_t new_capacity;
    uint8_t *new_buffer;
    uint64_t i;

    used = *tail - *head;
    if (used > SIZE_MAX || additional > SIZE_MAX - (size_t)used) return -12;
    required = used + additional;
    if (required <= *capacity) return 0;
    new_capacity = (size_t)required;
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

static void pty_compact_buffer(uint8_t **buffer, size_t *capacity,
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
    *capacity = (size_t)used;
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
    int idx;

    idx = fd - pty_base_master;
    if (idx < 0 || idx >= pty_capacity) return NULL;
    if (!ptys[idx].in_use) return NULL;
    return &ptys[idx];
}

static pty_t *get_pty_by_slave(int fd) {
    int idx;

    idx = fd - pty_base_slave;
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
    pty_t *pty;
    int fd;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(master_fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    if (!pty->unlocked) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    if (pty->slave_refs < 0)
        pty->slave_refs = 1;
    else
        pty->slave_refs++;
    fd = pty_base_slave + (int)(pty - ptys);
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return fd;
}

int pty_grant(int master_fd) {
    int found;

    mutex_lock(&pty_lock);
    found = get_pty_by_master(master_fd) != NULL;
    mutex_unlock(&pty_lock);
    return found ? 0 : -1;
}

int pty_unlock(int master_fd) {
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(master_fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    pty->unlocked = 1;
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return 0;
}

int pty_path_supported(const char *path) {
    const char *digits;

    if (!path) return 0;
    if (strcmp(path, "/dev/ptmx") == 0) return 1;
    if (strcmp(path, "/dev/tty") == 0) return 1;
    if (strcmp(path, "/dev/stdin") == 0) return 1;
    if (strcmp(path, "/dev/stdout") == 0) return 1;
    if (strcmp(path, "/dev/stderr") == 0) return 1;
    if (strncmp(path, "/dev/pts/", 9) != 0) return 0;
    digits = path + 9;
    if (!*digits) return 0;
    while (*digits) {
        if (*digits < '0' || *digits > '9') return 0;
        digits++;
    }
    return 1;
}

static int pty_open_task_alias(const char *path, int flags) {
    task_fd_t *source;
    pty_t *pty;
    pid_t session;
    int source_fd;
    int endpoint;
    int descriptor_type;
    int fd;
    int i;

    source_fd = -1;
    endpoint = -1;
    descriptor_type = FD_TYPE_PTY_SLAVE;
    if (strcmp(path, "/dev/stdin") == 0) source_fd = 0;
    if (strcmp(path, "/dev/stdout") == 0) source_fd = 1;
    if (strcmp(path, "/dev/stderr") == 0) source_fd = 2;
    if (source_fd >= 0) {
        source = task_fd_get(current_task, source_fd);
        if (!source || !FD_TYPE_IS_PTY(source->type))
            return PTY_OPEN_FALLBACK;
        endpoint = (int)(uintptr_t)source->private_data;
        descriptor_type = source->type;
        if (pty_retain_endpoint(endpoint) < 0) return -1;
    } else if (strcmp(path, "/dev/tty") == 0) {
        session = current_task->sid ? current_task->sid : current_task->pid;
        mutex_lock(&pty_lock);
        for (i = 0; i < pty_capacity; i++) {
            pty = &ptys[i];
            if (!pty->in_use || pty->session != session ||
                pty->slave_refs <= 0)
                continue;
            pty->slave_refs++;
            endpoint = pty_base_slave + i;
            break;
        }
        mutex_unlock(&pty_lock);
        if (endpoint < 0)
            return current_task->console_id >= 0 ?
                   PTY_OPEN_FALLBACK : PTY_OPEN_NOCTTY;
    } else {
        return PTY_OPEN_FALLBACK;
    }
    fd = task_fd_alloc(current_task);
    if (fd < 0) {
        if (descriptor_type == FD_TYPE_PTY_MASTER)
            pty_close_master(endpoint);
        else
            pty_close_slave(endpoint);
        return -1;
    }
    current_task->fds[fd].type = descriptor_type;
    current_task->fds[fd].flags = (uint64_t)flags;
    current_task->fds[fd].private_data = (void *)(uintptr_t)endpoint;
    return fd;
}

static int pty_has_controlling_session(pid_t session, pty_t *candidate) {
    int i;

    for (i = 0; i < pty_capacity; i++) {
        if (!ptys[i].in_use || &ptys[i] == candidate) continue;
        if (ptys[i].session == session) return 1;
    }
    return 0;
}

static void pty_acquire_controlling_terminal(int fd, int flags) {
    pty_t *pty;
    pid_t session;
    pid_t pgrp;

    if (!current_task || (flags & VFS_O_NOCTTY)) return;
    session = current_task->sid ? current_task->sid : current_task->pid;
    if (session != current_task->pid) return;
    pgrp = current_task->pgid ? current_task->pgid : current_task->pid;
    mutex_lock(&pty_lock);
    pty = get_pty_by_slave(fd);
    if (pty && pty->session == 0 &&
        !pty_has_controlling_session(session, pty)) {
        pty->session = session;
        pty->pgrp = pgrp;
    }
    mutex_unlock(&pty_lock);
}

int pty_open_path(const char *path, int flags) {
    const char *digits;
    int index;
    int digit;
    int endpoint;
    int descriptor_type;
    int fd;

    if (!current_task || !path) return -1;
    if (strcmp(path, "/dev/tty") == 0 ||
        strcmp(path, "/dev/stdin") == 0 ||
        strcmp(path, "/dev/stdout") == 0 ||
        strcmp(path, "/dev/stderr") == 0)
        return pty_open_task_alias(path, flags);
    if (strcmp(path, "/dev/ptmx") == 0) {
        endpoint = pty_open_master();
        descriptor_type = FD_TYPE_PTY_MASTER;
    } else if (strncmp(path, "/dev/pts/", 9) == 0) {
        digits = path + 9;
        index = 0;
        if (!*digits) return -1;
        while (*digits) {
            if (*digits < '0' || *digits > '9') return -1;
            digit = *digits - '0';
            if (index > (INT32_MAX - digit) / 10) return -1;
            index = index * 10 + digit;
            digits++;
        }
        endpoint = pty_open_slave(pty_base_master + index);
        descriptor_type = FD_TYPE_PTY_SLAVE;
    } else {
        return -1;
    }
    if (endpoint < 0) return -1;
    fd = task_fd_alloc(current_task);
    if (fd < 0) {
        if (descriptor_type == FD_TYPE_PTY_MASTER)
            pty_close_master(endpoint);
        else pty_close_slave(endpoint);
        return -1;
    }
    current_task->fds[fd].type = descriptor_type;
    current_task->fds[fd].flags = (uint64_t)flags;
    current_task->fds[fd].private_data = (void *)(uintptr_t)endpoint;
    if (descriptor_type == FD_TYPE_PTY_SLAVE)
        pty_acquire_controlling_terminal(endpoint, flags);
    return fd;
}

int pty_retain_endpoint(int fd) {
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (pty) {
        pty->master_refs++;
        mutex_unlock(&pty_lock);
        return 0;
    }
    pty = get_pty_by_slave(fd);
    if (pty) {
        pty->slave_refs++;
        mutex_unlock(&pty_lock);
        return 0;
    }
    mutex_unlock(&pty_lock);
    return -1;
}

int pty_task_endpoint(int fd) {
    task_fd_t *descriptor;
    int endpoint;

    if (!current_task || !current_task->fds || fd < 0 ||
        fd >= current_task->fds_capacity) return -1;
    descriptor = &current_task->fds[fd];
    if (!descriptor->in_use || !FD_TYPE_IS_PTY(descriptor->type) ||
        !descriptor->private_data) return -1;
    endpoint = (int)(uintptr_t)descriptor->private_data;
    if (descriptor->type == FD_TYPE_PTY_MASTER &&
        !is_pty_master(endpoint)) return -1;
    if (descriptor->type == FD_TYPE_PTY_SLAVE &&
        !is_pty_slave(endpoint)) return -1;
    return endpoint;
}

int pty_name(int master_fd, char *buffer, size_t size) {
    char tmp[16];
    int idx;
    int len;
    int i;
    int t;

    if (!buffer || size < 11) return -1;
    mutex_lock(&pty_lock);
    if (!get_pty_by_master(master_fd)) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    idx = master_fd - pty_base_master;
    strcpy(buffer, "/dev/pts/");
    len = 9;
    if (idx == 0) {
        buffer[len++] = '0';
    } else {
        i = 0;
        t = idx;
        while (t > 0) {
            tmp[i++] = '0' + (t % 10);
            t /= 10;
        }
        while (i > 0) {
            if ((size_t)(len + 1) >= size) {
                mutex_unlock(&pty_lock);
                return -1;
            }
            buffer[len++] = tmp[--i];
        }
    }
    buffer[len] = '\0';
    mutex_unlock(&pty_lock);
    return 0;
}

static size_t buf_used(uint64_t head, uint64_t tail) {
    return tail - head;
}

static size_t pty_slave_readable(pty_t *pty) {
    size_t available;
    size_t i;
    uint8_t c;

    available = buf_used(pty->master_head, pty->master_tail);
    if (pty->termios.c_lflag & ICANON) {
        for (i = 0; i < available; i++) {
            c = pty->master_buf[
                (pty->master_head + i) % pty->master_capacity];
            if (c == '\n' || c == pty->termios.c_cc[VEOF] ||
                c == pty->termios.c_cc[VEOL])
                return i + 1;
        }
        return 0;
    }
    if (pty->termios.c_cc[VMIN] > 0 &&
        available < pty->termios.c_cc[VMIN])
        return 0;
    return available;
}

static int pty_append_slave_output(pty_t *pty, uint8_t c) {
    size_t needed;

    needed = (pty->termios.c_oflag & OPOST) && c == '\n' &&
             (pty->termios.c_oflag & ONLCR) ? 2 : 1;
    if (pty_ensure_slave_buf(pty, needed) < 0) return -12;
    if (needed == 2) {
        pty->slave_buf[pty->slave_tail % pty->slave_capacity] = '\r';
        pty->slave_tail++;
    }
    pty->slave_buf[pty->slave_tail % pty->slave_capacity] = c;
    pty->slave_tail++;
    return 0;
}

static void pty_echo_erase(pty_t *pty) {
    if (!(pty->termios.c_lflag & ECHO)) return;
    if (pty->termios.c_lflag & ECHOE) {
        pty_append_slave_output(pty, '\b');
        pty_append_slave_output(pty, ' ');
        pty_append_slave_output(pty, '\b');
    } else {
        pty_append_slave_output(pty, pty->termios.c_cc[VERASE]);
    }
}

static void pty_echo_input(pty_t *pty, uint8_t c) {
    if ((pty->termios.c_lflag & ECHOCTL) && c != '\n' && c != '\t' &&
        (c < 32 || c == 127)) {
        pty_append_slave_output(pty, '^');
        pty_append_slave_output(pty, c == 127 ? '?' : (uint8_t)(c + '@'));
        return;
    }
    pty_append_slave_output(pty, c);
}

ssize_t pty_master_read(int fd, void *buf, size_t count) {
    size_t available;
    size_t to_read;
    uint8_t *dst;
    size_t i;
    pty_t *pty;
    int closed;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    mutex_lock(&pty->lock);
    available = buf_used(pty->slave_head, pty->slave_tail);
    if (available == 0) {
        closed = pty->slave_refs == 0;
        mutex_unlock(&pty->lock);
        mutex_unlock(&pty_lock);
        if (closed) return -5;
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
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return to_read;
}

ssize_t pty_master_write(int fd, const void *buf, size_t count) {
    size_t to_write;
    const uint8_t *src;
    size_t i;
    uint8_t c;
    uint8_t previous;
    pty_t *pty;
    int raw_cr;
    int cr_newline;
    int signal_number;
    pid_t signal_pgrp;

    signal_number = 0;
    signal_pgrp = 0;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    if (pty->slave_refs <= 0) {
        mutex_unlock(&pty_lock);
        return -32;
    }
    mutex_lock(&pty->lock);
    to_write = count;
    if (to_write > 0 && pty_ensure_master_buf(pty, to_write) < 0) {
        mutex_unlock(&pty->lock);
        mutex_unlock(&pty_lock);
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
                signal_number = 2;
                signal_pgrp = pty->pgrp;
                pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                                   &pty->master_head, &pty->master_tail);
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                if (signal_pgrp > 0)
                    deliver_signal_to_pgrp(signal_pgrp, signal_number);
                descriptor_ready_notify();
                return to_write;
            }
            if (c == pty->termios.c_cc[VQUIT]) {
                signal_number = 3;
                signal_pgrp = pty->pgrp;
                pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                                   &pty->master_head, &pty->master_tail);
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                if (signal_pgrp > 0)
                    deliver_signal_to_pgrp(signal_pgrp, signal_number);
                descriptor_ready_notify();
                return to_write;
            }
            if (c == pty->termios.c_cc[VSUSP]) {
                signal_number = 20;
                signal_pgrp = pty->pgrp;
                pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                                   &pty->master_head, &pty->master_tail);
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                if (signal_pgrp > 0)
                    deliver_signal_to_pgrp(signal_pgrp, signal_number);
                descriptor_ready_notify();
                return to_write;
            }
        }

        if ((pty->termios.c_lflag & ICANON) &&
            (c == pty->termios.c_cc[VERASE] ||
             (c == '\b' && pty->termios.c_cc[VERASE] == 127))) {
            if (pty->master_tail > pty->master_head) {
                previous = pty->master_buf[
                    (pty->master_tail - 1) % pty->master_capacity];
                if (previous != '\n' &&
                    previous != pty->termios.c_cc[VEOL]) {
                    pty->master_tail--;
                    pty_echo_erase(pty);
                }
            }
            continue;
        }
        if ((pty->termios.c_lflag & ICANON) &&
            c == pty->termios.c_cc[VKILL]) {
            while (pty->master_tail > pty->master_head) {
                previous = pty->master_buf[
                    (pty->master_tail - 1) % pty->master_capacity];
                if (previous == '\n' ||
                    previous == pty->termios.c_cc[VEOL]) break;
                pty->master_tail--;
                pty_echo_erase(pty);
            }
            if ((pty->termios.c_lflag & ECHOK) &&
                !(pty->termios.c_lflag & ECHOE))
                pty_append_slave_output(pty, '\n');
            continue;
        }
        
        pty->master_buf[pty->master_tail % pty->master_capacity] = c;
        pty->master_tail++;
        if ((pty->termios.c_lflag & ECHO) ||
            (c == '\n' && (pty->termios.c_lflag & ECHONL)))
            pty_echo_input(pty, c);
    }
    pty_compact_buffer(&pty->master_buf, &pty->master_capacity,
                       &pty->master_head, &pty->master_tail);
    
    mutex_unlock(&pty->lock);
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
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

    mutex_lock(&pty_lock);
    pty = get_pty_by_slave(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    if (pty->master_refs == 0) {
        mutex_unlock(&pty_lock);
        return 0;
    }
    mutex_lock(&pty->lock);
    available = buf_used(pty->master_head, pty->master_tail);
    if (available == 0) {
        mutex_unlock(&pty->lock);
        mutex_unlock(&pty_lock);
        return -11;
    }
    
    dst = (uint8_t *)buf;
    read_count = 0;
    
    if (pty->termios.c_lflag & ICANON) {
        found_line = 0;
        line_end = 0;
        
        for (i = 0; i < available; i++) {
            c = pty->master_buf[(pty->master_head + i) % pty->master_capacity];
            if (c == '\n' || c == pty->termios.c_cc[VEOF] || c == pty->termios.c_cc[VEOL]) {
                found_line = 1;
                line_end = i + 1;
                break;
            }
        }
        
        if (!found_line) {
            mutex_unlock(&pty->lock);
            mutex_unlock(&pty_lock);
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
            mutex_unlock(&pty_lock);
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
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return read_count;
}

ssize_t pty_slave_write(int fd, const void *buf, size_t count) {
    size_t consumed;
    size_t output_size;
    size_t character_size;
    const uint8_t *src;
    size_t i;
    uint8_t c;
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_slave(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    if (pty->master_refs == 0) {
        mutex_unlock(&pty_lock);
        return -32;
    }
    mutex_lock(&pty->lock);
    
    consumed = 0;
    output_size = 0;
    src = (const uint8_t *)buf;

    for (i = 0; i < count; i++) {
        c = src[i];
        character_size = (pty->termios.c_oflag & OPOST) &&
                         c == '\n' && (pty->termios.c_oflag & ONLCR) ? 2 : 1;
        if (character_size > SIZE_MAX - output_size) break;
        output_size += character_size;
        consumed++;
    }
    if (output_size > 0 && pty_ensure_slave_buf(pty, output_size) < 0) {
        mutex_unlock(&pty->lock);
        mutex_unlock(&pty_lock);
        return -12;
    }
    for (i = 0; i < consumed; i++) {
        c = src[i];
        pty_append_slave_output(pty, c);
    }

    mutex_unlock(&pty->lock);
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return consumed;
}

int pty_ioctl(int fd, unsigned long request, void *arg) {
    pty_t *pty;
    int index;
    int lock_value;
    int changed;
    int queue;
    pid_t signal_pgrp;
    size_t available;
    size_t readable;

    changed = 0;
    signal_pgrp = 0;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (!pty) pty = get_pty_by_slave(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    mutex_lock(&pty->lock);
    switch (request) {
        case TCGETS:
            if (arg) memcpy(arg, &pty->termios, sizeof(struct termios));
            break;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            if (arg && memcmp(&pty->termios, arg,
                              sizeof(struct termios)) != 0) {
                memcpy(&pty->termios, arg, sizeof(struct termios));
                changed = 1;
            }
            break;
        case TIOCGWINSZ:
            if (arg) memcpy(arg, &pty->winsize, sizeof(struct winsize));
            break;
        case TIOCSWINSZ:
            if (arg && memcmp(&pty->winsize, arg,
                              sizeof(struct winsize)) != 0) {
                memcpy(&pty->winsize, arg, sizeof(struct winsize));
                changed = 1;
                if (pty->pgrp > 0)
                    signal_pgrp = pty->pgrp;
            }
            break;
        case TIOCGPGRP:
            if (arg) *(pid_t *)arg = pty->pgrp;
            break;
        case FIONREAD:
            if (!arg) {
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                return -22;
            }
            if (get_pty_by_master(fd)) {
                available = buf_used(pty->slave_head, pty->slave_tail);
                readable = available;
            } else {
                readable = pty_slave_readable(pty);
            }
            if (readable > INT32_MAX) readable = INT32_MAX;
            *(int *)arg = (int)readable;
            break;
        case TIOCSPGRP:
            if (arg && pty->pgrp != *(pid_t *)arg) {
                pty->pgrp = *(pid_t *)arg;
                changed = 1;
            }
            break;
        case TIOCGSID:
            if (arg) *(pid_t *)arg = pty->session;
            break;
        case TIOCSCTTY:
            if (pty->session != (current_task->sid ? current_task->sid :
                                current_task->pid) ||
                pty->pgrp != (current_task->pgid ? current_task->pgid :
                              current_task->pid)) {
                pty->session = current_task->sid ? current_task->sid :
                               current_task->pid;
                pty->pgrp = current_task->pgid ? current_task->pgid :
                            current_task->pid;
                changed = 1;
            }
            break;
        case TIOCNOTTY:
            if (pty->session != 0 || pty->pgrp != 0) {
                pty->session = 0;
                pty->pgrp = 0;
                changed = 1;
            }
            break;
        case TCSBRK:
            break;
        case TCXONC:
            if ((int)(uintptr_t)arg < TCOOFF ||
                (int)(uintptr_t)arg > TCION) {
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                return -22;
            }
            break;
        case TCFLSH:
            queue = (int)(uintptr_t)arg;
            if (queue < TCIFLUSH || queue > TCIOFLUSH) {
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                return -22;
            }
            if (queue == TCIFLUSH || queue == TCIOFLUSH) {
                pty->master_head = pty->master_tail;
                pty->input_cr_pending = 0;
                pty_compact_buffer(&pty->master_buf,
                                   &pty->master_capacity,
                                   &pty->master_head,
                                   &pty->master_tail);
                changed = 1;
            }
            if (queue == TCOFLUSH || queue == TCIOFLUSH) {
                pty->slave_head = pty->slave_tail;
                pty_compact_buffer(&pty->slave_buf,
                                   &pty->slave_capacity,
                                   &pty->slave_head,
                                   &pty->slave_tail);
                changed = 1;
            }
            break;
        case TIOCGPTN:
            if (!arg || !get_pty_by_master(fd)) {
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                return -22;
            }
            index = fd - pty_base_master;
            *(int *)arg = index;
            break;
        case TIOCSPTLCK:
            if (!arg || !get_pty_by_master(fd)) {
                mutex_unlock(&pty->lock);
                mutex_unlock(&pty_lock);
                return -22;
            }
            lock_value = *(int *)arg;
            if (pty->unlocked != (lock_value ? 0 : 1)) {
                pty->unlocked = lock_value ? 0 : 1;
                changed = 1;
            }
            break;
        default:
            mutex_unlock(&pty->lock);
            mutex_unlock(&pty_lock);
            return -22;
    }
    mutex_unlock(&pty->lock);
    mutex_unlock(&pty_lock);
    if (signal_pgrp > 0)
        deliver_signal_to_pgrp(signal_pgrp, 28);
    if (changed)
        descriptor_ready_notify();
    return 0;
}

static void pty_release(pty_t *pty) {
    if (!pty || pty->master_refs != 0 || pty->slave_refs > 0) return;
    pty->in_use = 0;
    kfree(pty->master_buf);
    kfree(pty->slave_buf);
    pty->master_buf = NULL;
    pty->slave_buf = NULL;
    pty->master_capacity = 0;
    pty->slave_capacity = 0;
}

static void pty_reclaim_storage(void) {
    int new_capacity;
    pty_t *new_ptys;

    new_capacity = pty_capacity;
    while (new_capacity > 0 && !ptys[new_capacity - 1].in_use)
        new_capacity--;
    if (new_capacity == pty_capacity) return;
    if (new_capacity == 0) {
        kfree(ptys);
        ptys = NULL;
        pty_capacity = 0;
        return;
    }
    new_ptys = (pty_t *)krealloc(
        ptys, (size_t)new_capacity * sizeof(pty_t));
    if (!new_ptys) return;
    ptys = new_ptys;
    pty_capacity = new_capacity;
}

int pty_close_master(int fd) {
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    mutex_lock(&pty->lock);
    if (pty->master_refs <= 0) {
        mutex_unlock(&pty->lock);
        mutex_unlock(&pty_lock);
        return -1;
    }
    pty->master_refs--;
    pty_release(pty);
    mutex_unlock(&pty->lock);
    pty_reclaim_storage();
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return 0;
}

int pty_close_slave(int fd) {
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_slave(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return -1;
    }
    mutex_lock(&pty->lock);
    if (pty->slave_refs <= 0) {
        mutex_unlock(&pty->lock);
        mutex_unlock(&pty_lock);
        return -1;
    }
    pty->slave_refs--;
    pty_release(pty);
    mutex_unlock(&pty->lock);
    pty_reclaim_storage();
    mutex_unlock(&pty_lock);
    descriptor_ready_notify();
    return 0;
}

int is_pty_master(int fd) {
    int found;

    mutex_lock(&pty_lock);
    found = get_pty_by_master(fd) != NULL;
    mutex_unlock(&pty_lock);
    return found;
}

int is_pty_slave(int fd) {
    int found;

    mutex_lock(&pty_lock);
    found = get_pty_by_slave(fd) != NULL;
    mutex_unlock(&pty_lock);
    return found;
}

int pty_has_data_for_master(int fd) {
    size_t available;
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return 0;
    }
    available = buf_used(pty->slave_head, pty->slave_tail);
    available = available > 0 || pty->slave_refs == 0;
    mutex_unlock(&pty_lock);
    return available ? 1 : 0;
}

int pty_has_data_for_slave(int fd) {
    size_t available;
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_slave(fd);
    if (!pty) {
        mutex_unlock(&pty_lock);
        return 0;
    }
    available = pty_slave_readable(pty);
    available = available > 0 || pty->master_refs == 0;
    mutex_unlock(&pty_lock);
    return available ? 1 : 0;
}

int pty_can_write(int fd) {
    pty_t *pty;
    int writable;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (pty) {
        writable = pty->slave_refs > 0;
        mutex_unlock(&pty_lock);
        return writable;
    }
    pty = get_pty_by_slave(fd);
    if (pty) {
        writable = pty->master_refs > 0;
        mutex_unlock(&pty_lock);
        return writable;
    }
    mutex_unlock(&pty_lock);
    return 0;
}

int pty_poll_events(int fd) {
    int events;
    pty_t *pty;

    mutex_lock(&pty_lock);
    pty = get_pty_by_master(fd);
    if (pty) {
        events = (buf_used(pty->slave_head, pty->slave_tail) > 0 ||
                  pty->slave_refs == 0) ? 0x01 : 0;
        if (pty->slave_refs > 0) events |= 0x04;
        if (pty->slave_refs == 0) events |= 0x10;
        mutex_unlock(&pty_lock);
        return events;
    }
    pty = get_pty_by_slave(fd);
    if (pty) {
        events = (pty_slave_readable(pty) > 0 ||
                  pty->master_refs == 0) ? 0x01 : 0;
        if (pty->master_refs > 0) events |= 0x04;
        if (pty->master_refs == 0) events |= 0x10;
        mutex_unlock(&pty_lock);
        return events;
    }
    mutex_unlock(&pty_lock);
    return -1;
}

void KERNEL_INIT pty_init(void) {
    pty_capacity = 0;
    ptys = NULL;
    mutex_init(&pty_lock);
}

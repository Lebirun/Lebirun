#include <lebirun/io.h>
#include <lebirun/tty.h>
#include <lebirun/keyboard.h>
#include <lebirun/task.h>
#include <lebirun/console.h>
#include <lebirun/cmdline.h>
#include <lebirun/mem_map.h>
#include <string.h>

#define KEYBOARD_BUFFER_INITIAL 8

typedef struct {
    char *buffer;
    unsigned int capacity;
    volatile unsigned int head;
    volatile unsigned int tail;
    wait_queue_t waitq;
    volatile int sigint_pending;
} kbd_console_t;

static kbd_console_t *kbd_consoles;
static int kbd_num_consoles;

static bool left_shift_pressed = false;
static bool right_shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;
static bool e0_prefix = false;

static keyboard_observer_t kbd_observer = NULL;

extern void serial_write_direct(const char *buf, size_t len);

int keyboard_get_modifier_state(void) {
    int state = 0;
    if (ctrl_pressed) state |= 1;
    if (alt_pressed) state |= 2;
    if (left_shift_pressed || right_shift_pressed) state |= 4;
    return state;
}

void keyboard_register_observer(keyboard_observer_t observer) {
    kbd_observer = observer;
}

void keyboard_unregister_observer(void) {
    kbd_observer = NULL;
}

#define SCANCODE_CTRL  0x1D
#define SCANCODE_ALT   0x38
#define SCANCODE_CAPS  0x3A

#define SCANCODE_LSHIFT 0x2A
#define SCANCODE_RSHIFT 0x36
#define SCANCODE_C      0x2E

static const char qwerty_lowercase[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char qwerty_uppercase[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static inline bool shift_is_down(void) {
    return left_shift_pressed || right_shift_pressed;
}

static inline bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline char apply_caps_shift(char c, bool shift) {
    if (c >= 'a' && c <= 'z' && caps_lock != shift)
        return (char)(c - 'a' + 'A');
    return c;
}

static int buffer_grow(kbd_console_t *console) {
    char *grown;
    unsigned int old_capacity;
    unsigned int new_capacity;
    unsigned int count;
    unsigned int i;

    old_capacity = console->capacity;
    if (old_capacity == 0) {
        new_capacity = KEYBOARD_BUFFER_INITIAL;
    } else {
        if (old_capacity > UINT32_MAX / 2) return 0;
        new_capacity = old_capacity * 2;
    }
    grown = (char *)kmalloc(new_capacity);
    if (!grown) return 0;
    count = 0;
    if (console->buffer && old_capacity != 0) {
        i = console->tail;
        while (i != console->head) {
            grown[count++] = console->buffer[i];
            i = (i + 1) % old_capacity;
        }
    }
    kfree(console->buffer);
    console->buffer = grown;
    console->capacity = new_capacity;
    console->tail = 0;
    console->head = count;
    return 1;
}

static void buffer_put(char c) {
    kbd_console_t *console;
    int cur;
    unsigned int next;

    if (!kbd_consoles) return;
    cur = console_is_initialized() ? console_get_current() : 0;
    if (cur < 0 || cur >= kbd_num_consoles) return;
    console = &kbd_consoles[cur];
    if (!console->buffer && !buffer_grow(console)) return;
    next = (console->head + 1) % console->capacity;
    if (next == console->tail) {
        if (!buffer_grow(console)) return;
        next = (console->head + 1) % console->capacity;
    }
    console->buffer[console->head] = c;
    console->head = next;
}

static void buffer_put_seq(const char *seq, int len) {
    int i;
    for (i = 0; i < len; i++) {
        buffer_put(seq[i]);
    }
}

int keyboard_has_data(void) {
    int cur = console_is_initialized() ? console_get_current() : 0;
    return keyboard_has_data_for(cur);
}

int keyboard_getchar_nb(void) {
    int cur = console_is_initialized() ? console_get_current() : 0;
    return keyboard_getchar_nb_for(cur);
}

int keyboard_has_data_for(int console_id) {
    if (!kbd_consoles || console_id < 0 ||
        console_id >= kbd_num_consoles) return 0;
    return kbd_consoles[console_id].head != kbd_consoles[console_id].tail;
}

int keyboard_getchar_nb_for(int console_id) {
    int c;

    if (!kbd_consoles || console_id < 0 ||
        console_id >= kbd_num_consoles) return -1;
    if (kbd_consoles[console_id].head == kbd_consoles[console_id].tail) return -1;
    c = (unsigned char)kbd_consoles[console_id].buffer[kbd_consoles[console_id].tail];
    kbd_consoles[console_id].tail =
        (kbd_consoles[console_id].tail + 1) %
            kbd_consoles[console_id].capacity;
    return c;
}

void keyboard_flush_for(int console_id) {
    if (!kbd_consoles || console_id < 0 ||
        console_id >= kbd_num_consoles) return;
    kbd_consoles[console_id].head = 0;
    kbd_consoles[console_id].tail = 0;
    kbd_consoles[console_id].sigint_pending = 0;
}

wait_queue_t* keyboard_get_waitq(void) {
    int cur = console_is_initialized() ? console_get_current() : 0;
    if (!kbd_consoles || cur < 0 || cur >= kbd_num_consoles) return NULL;
    return &kbd_consoles[cur].waitq;
}

wait_queue_t* keyboard_get_waitq_for(int console_id) {
    if (!kbd_consoles || console_id < 0 ||
        console_id >= kbd_num_consoles) return NULL;
    return &kbd_consoles[console_id].waitq;
}

int getchar(void) {
    while (!keyboard_has_data()) asm volatile("hlt");
    return keyboard_getchar_nb();
}

void keyboard_handler(registers_t* regs) {
    static const char *f6_seqs[] = {
        "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~"
    };
    struct keyboard_event kev;
    uint8_t scancode;
    uint8_t code;
    int was_e0;
    int console_num;
    int cur;
    bool is_release;
    bool shift;
    char seq[4];
    char cc;
    char c;

    (void)regs;
    scancode = inb(0x60);

    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    was_e0 = e0_prefix;
    e0_prefix = false;

    is_release = (scancode & 0x80) != 0;
    code = scancode & 0x7F;

    if (kbd_observer) {
        kev.scancode = code;
        kev.is_release = is_release ? 1 : 0;
        kev.is_extended = was_e0 ? 1 : 0;
        kev.ctrl_held = ctrl_pressed ? 1 : 0;
        kev.alt_held = alt_pressed ? 1 : 0;
        kev.shift_held = shift_is_down() ? 1 : 0;
        kbd_observer(kev);
    }

    if (was_e0) {
        if (is_release) {
            if (code == SCANCODE_CTRL) ctrl_pressed = false;
            else if (code == SCANCODE_ALT) alt_pressed = false;
            return;
        }
        if (code == SCANCODE_CTRL) { ctrl_pressed = true; return; }
        if (code == SCANCODE_ALT) { alt_pressed = true; return; }
        if (console_is_initialized() &&
            console_get_graphics_mode(console_get_current())) return;
        if (code == 0x48) { buffer_put_seq("\033[A", 3); goto wake; }
        if (code == 0x50) { buffer_put_seq("\033[B", 3); goto wake; }
        if (code == 0x4D) { buffer_put_seq("\033[C", 3); goto wake; }
        if (code == 0x4B) { buffer_put_seq("\033[D", 3); goto wake; }
        if (code == 0x47) { buffer_put_seq("\033[H", 3); goto wake; }
        if (code == 0x4F) { buffer_put_seq("\033[F", 3); goto wake; }
        if (code == 0x53) { buffer_put_seq("\033[3~", 4); goto wake; }
        return;
    }

    if (is_release) {
        if (code == SCANCODE_LSHIFT) left_shift_pressed = false;
        else if (code == SCANCODE_RSHIFT) right_shift_pressed = false;
        else if (code == SCANCODE_CTRL) ctrl_pressed = false;
        else if (code == SCANCODE_ALT) alt_pressed = false;
        return;
    }

    if (code == SCANCODE_LSHIFT) { left_shift_pressed = true; return; }
    if (code == SCANCODE_RSHIFT) { right_shift_pressed = true; return; }
    if (code == SCANCODE_CTRL) { ctrl_pressed = true; return; }
    if (code == SCANCODE_ALT) { alt_pressed = true; return; }
    if (code == SCANCODE_CAPS) { caps_lock = !caps_lock; return; }

    if (ctrl_pressed && alt_pressed &&
        (!console_is_initialized() ||
         !console_get_graphics_mode(console_get_current()))) {
        console_num = -1;
        if (code >= SCANCODE_F1 && code <= SCANCODE_F10)
            console_num = code - SCANCODE_F1;
        else if (code == SCANCODE_F11) console_num = 10;
        else if (code == SCANCODE_F12) console_num = 11;
        if (console_num >= 0 && console_num < console_get_count()) {
            console_switch_via_interrupt(console_num);
            return;
        }
    }

    if (console_is_initialized() &&
        console_get_graphics_mode(console_get_current())) return;

    if (code >= 0x3B && code <= 0x3F && !ctrl_pressed && !alt_pressed) {
        seq[0] = '\033';
        seq[1] = '[';
        seq[2] = '[';
        seq[3] = 'A' + (code - 0x3B);
        buffer_put_seq(seq, 4);
        goto wake;
    }
    if (code >= 0x40 && code <= 0x44 && !ctrl_pressed && !alt_pressed) {
        buffer_put_seq(f6_seqs[code - 0x40], 5);
        goto wake;
    }
    if (code == 0x57 && !ctrl_pressed && !alt_pressed) {
        buffer_put_seq("\033[23~", 5);
        goto wake;
    }
    if (code == 0x58 && !ctrl_pressed && !alt_pressed) {
        buffer_put_seq("\033[24~", 5);
        goto wake;
    }

    if (ctrl_pressed && code == SCANCODE_C) {
        cur = console_is_initialized() ? console_get_current() : 0;
        if (cur >= 0 && cur < kbd_num_consoles)
            kbd_consoles[cur].sigint_pending = 1;
        if (cur == 0) {
            serial_write_direct("^C\n", 3);
        }
        console_write_to_fb_only(cur, "^C\n", 3);
        buffer_put(0x03);
        goto wake;
    }

    if (ctrl_pressed) {
        cc = qwerty_lowercase[code];
        if (cc >= 'a' && cc <= 'z') {
            buffer_put((char)(cc - 'a' + 1));
            goto wake;
        }
    }

    shift = shift_is_down();
    c = qwerty_lowercase[code];
    if (!is_alpha(c) && shift) {
        c = qwerty_uppercase[code];
    }
    c = apply_caps_shift(c, shift);
    if (c != 0) {
        buffer_put(c);
        goto wake;
    }
    return;

wake:
    cur = console_is_initialized() ? console_get_current() : 0;
    if (cur >= 0 && cur < kbd_num_consoles)
        waitq_wake_all(&kbd_consoles[cur].waitq);
    descriptor_ready_notify();
}

void KERNEL_INIT keyboard_init(void) {
    int i;
    uint8_t cmd;
    uint8_t master_mask;

    while (inb(0x64) & 0x01) {
        inb(0x60);
    }

    outb(0x64, 0x20); 
    cmd = inb(0x60);
    cmd |= 0x01;      
    cmd &= ~0x10;      
    outb(0x64, 0x60);  
    outb(0x60, cmd);

    outb(0x60, 0xF4);

    while (inb(0x64) & 0x01) {
        inb(0x60);
    }

    kbd_num_consoles = console_get_count();
    if (kbd_num_consoles <= 0) kbd_num_consoles = 1;
    kbd_consoles = kmalloc(kbd_num_consoles * sizeof(kbd_console_t));
    if (kbd_consoles) {
        memset(kbd_consoles, 0, kbd_num_consoles * sizeof(kbd_console_t));
        for (i = 0; i < kbd_num_consoles; i++) {
            waitq_init(&kbd_consoles[i].waitq);
        }
    } else kbd_num_consoles = 0;

    master_mask = inb(0x21);
    master_mask &= ~(1 << 1);
    outb(0x21, master_mask);
}

static pid_t keyboard_next_sigint_target(int console_id, pid_t foreground_pgid,
                                         pid_t after)
{
    task_t *task;
    pid_t task_pgid;
    pid_t join_pid;
    pid_t next;
    uintptr_t address;

    next = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task) {
        address = (uintptr_t)task;
        if (address < KERNEL_VMA)
            break;
        if ((address & 0xFFFF0000u) == 0xFEFE0000u)
            break;

        task_pgid = task->pgid ? task->pgid : task->pid;
        if (task_pgid == foreground_pgid && task->is_user &&
                task->state != TASK_DEAD && task->pid > after &&
                (next == 0 || task->pid < next))
            next = task->pid;

        if (task->is_user && task->state == TASK_BLOCKED &&
                (task->waiting_for_any_child || task->join_target) &&
                task->join_target && task->console_id == console_id) {
            join_pid = task->join_target->pid;
            if (join_pid > after && (next == 0 || join_pid < next))
                next = join_pid;
        }

        task = task->all_next;
    }
    unlock_scheduler();
    return next;
}

void keyboard_process_sigint(void)
{
    extern struct termios *tty_termios;
    extern int *tty_pgrp;
    extern int tty_count;
    extern int deliver_signal_to_task(task_t *target, int sig);
    extern void syscall_core_flush_tty_input(int con_id);
    int i;
    int fg;
    task_t *target;
    pid_t last_pid;
    pid_t next_pid;

    if (!kbd_consoles || !tty_termios || !tty_pgrp) return;

    for (i = 0; i < kbd_num_consoles; i++) {
        if (i >= tty_count)
            break;
        if (!kbd_consoles[i].sigint_pending)
            continue;
        kbd_consoles[i].sigint_pending = 0;

        if (!(tty_termios[i].c_lflag & ISIG))
            continue;

        if (!(tty_termios[i].c_lflag & NOFLSH)) {
            keyboard_flush_for(i);
            syscall_core_flush_tty_input(i);
        }

        fg = tty_pgrp[i];
        if (fg <= 0)
            continue;

        last_pid = 0;
        for (;;) {
            next_pid = keyboard_next_sigint_target(i, (pid_t)fg, last_pid);
            if (next_pid <= 0)
                break;
            last_pid = next_pid;
            target = task_find(next_pid);
            if (target && target->is_user && target->state != TASK_DEAD) {
                deliver_signal_to_task(target, 2);
            }
        }
    }
}

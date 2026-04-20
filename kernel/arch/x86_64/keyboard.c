#include <lebirun/io.h>
#include <lebirun/tty.h>
#include <lebirun/debug.h>
#include <lebirun/keyboard.h>
#include <lebirun/task.h>
#include <lebirun/console.h>
#include <lebirun/cmdline.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <string.h>

#define BUFFER_SIZE 128

typedef struct {
    char buffer[BUFFER_SIZE];
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

#define SCANCODE_F1  0x3B
#define SCANCODE_F2  0x3C
#define SCANCODE_F3  0x3D
#define SCANCODE_F4  0x3E
#define SCANCODE_F5  0x3F
#define SCANCODE_F6  0x40
#define SCANCODE_F7  0x41
#define SCANCODE_F8  0x42
#define SCANCODE_F9  0x43
#define SCANCODE_F10 0x44
#define SCANCODE_F11 0x57
#define SCANCODE_F12 0x58

#define SC2_F1  0x05
#define SC2_F2  0x06
#define SC2_F3  0x04
#define SC2_F4  0x0C
#define SC2_F5  0x03
#define SC2_F6  0x0B
#define SC2_F7  0x83
#define SC2_F8  0x0A
#define SC2_F9  0x01

#define SCANCODE_CTRL  0x1D
#define SCANCODE_ALT   0x38
#define SCANCODE_CAPS  0x3A

#define SCANCODE_LSHIFT 0x2A
#define SCANCODE_RSHIFT 0x36
#define SCANCODE_C      0x2E

#define SC2_LSHIFT     0x12
#define SC2_RSHIFT     0x59
#define SC2_CTRL       0x14
#define SC2_ALT        0x11
#define SC2_CAPS       0x58
#define SC2_C  0x21

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
    if (!is_alpha(c)) return c;
    bool upper = (c >= 'A' && c <= 'Z');
    bool want_upper = upper;
    if (caps_lock) want_upper = !want_upper;
    if (shift) want_upper = !want_upper;
    if (want_upper) {
        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
        return c;
    }
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static void buffer_put(char c) {
    int cur;
    unsigned int next;

    if (!kbd_consoles) return;
    cur = console_is_initialized() ? console_get_current() : 0;
    if (cur < 0 || cur >= kbd_num_consoles) return;
    next = (kbd_consoles[cur].head + 1) % BUFFER_SIZE;
    if (next == kbd_consoles[cur].tail) return;
    kbd_consoles[cur].buffer[kbd_consoles[cur].head] = c;
    kbd_consoles[cur].head = next;
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
    if (console_id < 0 || console_id >= kbd_num_consoles) return 0;
    return kbd_consoles[console_id].head != kbd_consoles[console_id].tail;
}

int keyboard_getchar_nb_for(int console_id) {
    int c;

    if (console_id < 0 || console_id >= kbd_num_consoles) return -1;
    if (kbd_consoles[console_id].head == kbd_consoles[console_id].tail) return -1;
    c = (unsigned char)kbd_consoles[console_id].buffer[kbd_consoles[console_id].tail];
    kbd_consoles[console_id].tail = (kbd_consoles[console_id].tail + 1) % BUFFER_SIZE;
    return c;
}

wait_queue_t* keyboard_get_waitq(void) {
    int cur = console_is_initialized() ? console_get_current() : 0;
    if (cur < 0 || cur >= kbd_num_consoles) return NULL;
    return &kbd_consoles[cur].waitq;
}

wait_queue_t* keyboard_get_waitq_for(int console_id) {
    if (console_id < 0 || console_id >= kbd_num_consoles) return NULL;
    return &kbd_consoles[console_id].waitq;
}

int getchar(void) {
    while (!keyboard_has_data()) asm volatile("hlt");
    return keyboard_getchar_nb();
}

void keyboard_handler(registers_t* regs) {
    (void)regs;

    uint8_t scancode = inb(0x60);
    int was_e0;

    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    was_e0 = e0_prefix;
    e0_prefix = false;

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (kbd_observer) {
        struct keyboard_event kev;
        kev.scancode = code;
        kev.is_release = is_release ? 1 : 0;
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

    if (ctrl_pressed && alt_pressed) {
        int console_num = -1;
        if (code == SCANCODE_F1) console_num = 0;
        else if (code == SCANCODE_F2) console_num = 1;
        else if (code == SCANCODE_F3) console_num = 2;
        else if (code == SCANCODE_F4) console_num = 3;
        else if (code == SCANCODE_F5) console_num = 4;
        else if (code == SCANCODE_F6) console_num = 5;
        else if (code == SCANCODE_F7) console_num = 6;
        else if (code == SCANCODE_F8) console_num = 7;
        else if (code == SCANCODE_F9) console_num = 8;
        else if (code == SCANCODE_F10) console_num = 9;
        else if (code == SCANCODE_F11) console_num = 10;
        else if (code == SCANCODE_F12) console_num = 11;
        if (console_num >= 0 && console_num < cmdline_get_consoles()) {
            console_switch_via_interrupt(console_num);
            return;
        }
    }

    if (code >= 0x3B && code <= 0x3F && !ctrl_pressed && !alt_pressed) {
        char seq[4];
        seq[0] = '\033';
        seq[1] = '[';
        seq[2] = '[';
        seq[3] = 'A' + (code - 0x3B);
        buffer_put_seq(seq, 4);
        goto wake;
    }
    if (code >= 0x40 && code <= 0x44 && !ctrl_pressed && !alt_pressed) {
        static const char *f6_seqs[] = {
            "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~"
        };
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
        int cur = console_is_initialized() ? console_get_current() : 0;
        if (cur >= 0 && cur < kbd_num_consoles)
            kbd_consoles[cur].sigint_pending = 1;
        if (cur == 0) {
            extern void serial_write_direct(const char *buf, size_t len);
            serial_write_direct("^C\n", 3);
        }
        console_write_to_fb_only(cur, "^C\n", 3);
        buffer_put(0x03);
        goto wake;
    }

    if (ctrl_pressed) {
        char cc = qwerty_lowercase[code];
        if (cc >= 'a' && cc <= 'z') {
            buffer_put((char)(cc - 'a' + 1));
            goto wake;
        }
    }

    bool shift = shift_is_down();
    char c = qwerty_lowercase[code];
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
    {
        int cur = console_is_initialized() ? console_get_current() : 0;
        if (cur >= 0 && cur < kbd_num_consoles)
            waitq_wake_all(&kbd_consoles[cur].waitq);
    }
}

void keyboard_init(void) {
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

    kbd_num_consoles = cmdline_get_consoles();
    if (kbd_num_consoles <= 0) kbd_num_consoles = 1;
    if (kbd_num_consoles > NUM_CONSOLES) kbd_num_consoles = NUM_CONSOLES;
    kbd_consoles = kmalloc(kbd_num_consoles * sizeof(kbd_console_t));
    if (kbd_consoles) {
        memset(kbd_consoles, 0, kbd_num_consoles * sizeof(kbd_console_t));
        for (i = 0; i < kbd_num_consoles; i++) {
            waitq_init(&kbd_consoles[i].waitq);
        }
    }

    master_mask = inb(0x21);
    master_mask &= ~(1 << 1);
    outb(0x21, master_mask);
}

void keyboard_process_sigint(void)
{
    extern struct termios tty_termios[];
    extern int tty_pgrp[];
    extern int deliver_signal_to_task(task_t *target, int sig);
    extern task_t *all_tasks_head;
    int i;
    int fg;
    task_t *t;
    int guard;
    pid_t t_pgid;

    if (!kbd_consoles) return;

    for (i = 0; i < kbd_num_consoles; i++) {
        if (!kbd_consoles[i].sigint_pending)
            continue;
        kbd_consoles[i].sigint_pending = 0;

        if (!(tty_termios[i].c_lflag & ISIG))
            continue;

        fg = tty_pgrp[i];
        if (fg <= 0)
            continue;

        t = all_tasks_head;
        guard = 0;
        while (t && guard < 4096) {
            uintptr_t a = (uintptr_t)t;
            if (a < KERNEL_VMA)
                break;
            if ((a & 0xFFFF0000u) == 0xFEFE0000u)
                break;
            t_pgid = t->pgid ? t->pgid : t->pid;
            if (t_pgid == (pid_t)fg) {
                deliver_signal_to_task(t, 2);
            }
            t = t->all_next;
            guard++;
        }
    }
}

#include <kernel/io.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/keyboard.h>
#include <kernel/task.h>
#include <kernel/console.h>

#define BUFFER_SIZE 256
static char key_buffers[NUM_CONSOLES][BUFFER_SIZE];
static unsigned int head[NUM_CONSOLES];
static unsigned int tail[NUM_CONSOLES];

static wait_queue_t keyboard_waiters[NUM_CONSOLES];

static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool e0_prefix = false;

#define SCANCODE_F1  0x3B
#define SCANCODE_F2  0x3C
#define SCANCODE_F3  0x3D
#define SCANCODE_F4  0x3E
#define SCANCODE_F5  0x3F
#define SCANCODE_F6  0x40
#define SCANCODE_F7  0x41
#define SCANCODE_F8  0x42
#define SCANCODE_F9  0x43

#define SCANCODE_CTRL  0x1D
#define SCANCODE_ALT   0x38

static const char qwerty_lowercase[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char qwerty_uppercase[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static void buffer_put(char c) {
    int cur = console_is_initialized() ? console_get_current() : 0;
    key_buffers[cur][head[cur]] = c;
    head[cur] = (head[cur] + 1) % BUFFER_SIZE;
    if (head[cur] == tail[cur]) tail[cur] = (tail[cur] + 1) % BUFFER_SIZE;
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
    if (console_id < 0 || console_id >= NUM_CONSOLES) return 0;
    return head[console_id] != tail[console_id];
}

int keyboard_getchar_nb_for(int console_id) {
    if (console_id < 0 || console_id >= NUM_CONSOLES) return -1;
    if (head[console_id] == tail[console_id]) return -1;
    int c = (unsigned char)key_buffers[console_id][tail[console_id]];
    tail[console_id] = (tail[console_id] + 1) % BUFFER_SIZE;
    return c;
}

wait_queue_t* keyboard_get_waitq(void) {
    int cur = console_is_initialized() ? console_get_current() : 0;
    return &keyboard_waiters[cur];
}

wait_queue_t* keyboard_get_waitq_for(int console_id) {
    if (console_id < 0 || console_id >= NUM_CONSOLES) return NULL;
    return &keyboard_waiters[console_id];
}

int getchar(void) {
    while (!keyboard_has_data()) asm volatile("hlt");
    return keyboard_getchar_nb();
}

void keyboard_handler(registers_t* regs) {
    (void)regs;

    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    if (e0_prefix) {
        e0_prefix = false;
        return;
    }

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (is_release) {
        if (code == 0x2A || code == 0x36) {
            shift_pressed = false;
        } else if (code == SCANCODE_CTRL) {
            ctrl_pressed = false;
        } else if (code == SCANCODE_ALT) {
            alt_pressed = false;
        }
        return;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = true;
        return;
    }
    if (code == SCANCODE_CTRL) {
        ctrl_pressed = true;
        return;
    }
    if (code == SCANCODE_ALT) {
        alt_pressed = true;
        return;
    }

    if (ctrl_pressed && alt_pressed) {
        if (code >= SCANCODE_F1 && code <= SCANCODE_F9) {
            int console_num = code - SCANCODE_F1;
            console_switch(console_num);
            return;
        }
    }

    char c = shift_pressed ? qwerty_uppercase[code] : qwerty_lowercase[code];
    if (c != 0) {
        buffer_put(c);
        int cur = console_is_initialized() ? console_get_current() : 0;
        waitq_wake_all(&keyboard_waiters[cur]);
    }
}

void keyboard_init(void) {
    while (inb(0x64) & 0x01) {
        inb(0x60);
    }

    outb(0x64, 0x20); 
    uint8_t cmd = inb(0x60);
    cmd |= 0x01;      
    cmd &= ~0x10;      
    outb(0x64, 0x60);  
    outb(0x60, cmd);

    outb(0x60, 0xF4);

    while (inb(0x64) & 0x01) {
        inb(0x60);
    }

    for (int i = 0; i < NUM_CONSOLES; i++) {
        head[i] = tail[i] = 0;
        waitq_init(&keyboard_waiters[i]);
    }

    uint8_t master_mask = inb(0x21);
    master_mask &= ~(1 << 1);
    outb(0x21, master_mask);

    terminal_writestring("Keyboard initialized.\n");
}
#include <kernel/io.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/keyboard.h>
#include <kernel/task.h>

#define BUFFER_SIZE 256
static char key_buffer[BUFFER_SIZE];
static unsigned int head = 0;
static unsigned int tail = 0;

static wait_queue_t keyboard_waiters;

static bool shift_pressed = false;
static bool e0_prefix = false;

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
    key_buffer[head] = c;
    head = (head + 1) % BUFFER_SIZE;
    if (head == tail) tail = (tail + 1) % BUFFER_SIZE;
}

int keyboard_has_data(void) {
    return head != tail;
}

int keyboard_getchar_nb(void) {
    if (head == tail) return -1;
    int c = (unsigned char)key_buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return c;
}

wait_queue_t* keyboard_get_waitq(void) {
    return &keyboard_waiters;
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
        }
        return;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = true;
        return;
    }

    char c = shift_pressed ? qwerty_uppercase[code] : qwerty_lowercase[code];
    if (c != 0) {
        buffer_put(c);
        waitq_wake_all(&keyboard_waiters);
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

    waitq_init(&keyboard_waiters);

    uint8_t master_mask = inb(0x21);
    master_mask &= ~(1 << 1);
    outb(0x21, master_mask);

    terminal_writestring("Keyboard initialized.\n");
}
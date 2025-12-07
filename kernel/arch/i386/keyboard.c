#include "io.h"
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/keyboard.h>

#define BUFFER_SIZE 256
static char key_buffer[BUFFER_SIZE];
static unsigned int head = 0;
static unsigned int tail = 0;

static bool shift_pressed = false;

static const char qwerty_lowercase[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '{', '}', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ':', '"', '~', 0, '|', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char qwerty_uppercase[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void buffer_put(char c) {
    key_buffer[head] = c;
    head = (head + 1) % BUFFER_SIZE;
    if (head == tail) tail = (tail + 1) % BUFFER_SIZE;
}

char getchar(void) {
    while (head == tail) asm volatile("hlt");
    char c = key_buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return c;
}

void keyboard_handler(registers_t* regs) {
    (void)regs;
    static bool shift_pressed = false;

    uint8_t scancode = inb(0x60);

    // terminal_writestring("Raw: 0x"); print_hex(scancode); ... \n

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (is_release) {
        // terminal_writestring("--- RELEASE HANDLED ---\n");
        if (code == 0x2A || code == 0x36) {
            shift_pressed = false;
            // terminal_writestring("SHIFT RELEASED!\n");
        } else {
            // terminal_writestring("Unknown release ignored\n");
        }
        return;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = true;
        // terminal_writestring("SHIFT PRESSED!\n");
        return;
    }

    char c = shift_pressed ? qwerty_uppercase[code] : qwerty_lowercase[code];
    if (c != 0) {
        terminal_putchar(c);
    }
    // terminal_writestring(" (0x"); print_hex(scancode); terminal_writestring(")\n");
}

void keyboard_init(void) {
    outb(0x64, 0x20); 
    uint8_t cmd = inb(0x60);
    cmd |= 0x01;
    outb(0x64, 0x60);
    outb(0x60, cmd);
    terminal_writestring("Keyboard controller IRQ enabled.\n");

    outb(0x60, 0xFF); 
    terminal_writestring("Kbd reset sent (no ACK wait).\n");
}
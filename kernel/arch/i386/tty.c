#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/framebuffer.h>
#include <kernel/psf.h>
#include <kernel/console.h>
#include <kernel/io.h>
#include <kernel/common.h>

#include "vga.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xC00B8000;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;
static bool use_framebuffer = false;
static psf_font_t loaded_font;

#define EARLY_BOOT_BUFFER_SIZE 16384
static char early_boot_buffer[EARLY_BOOT_BUFFER_SIZE];
static size_t early_boot_index = 0;
static bool early_boot_capture = true;

static void terminal_updatecursor(void) {
    if (use_framebuffer) {
        fb_update_cursor();
        return;
    }
    uint16_t pos = terminal_row * VGA_WIDTH + terminal_column;
    outb(0x3D4, 14);
    outb(0x3D5, (pos >> 8) & 0xFF);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
}

void terminal_initialize(void) {
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
	terminal_buffer = VGA_MEMORY;
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_color);
		}
	}
	terminal_updatecursor();
}

void terminal_init_fb(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp, uint8_t type) {
    if (fb_init(addr, width, height, pitch, bpp, type) == 0) {
        use_framebuffer = true;
        fb_set_colors(0xFFAAAAAA, 0xFF000000);
    }
}

void terminal_replay_early_boot(void) {
    size_t i;
    early_boot_capture = false;
    for (i = 0; i < early_boot_index; i++) {
        char c;
        c = early_boot_buffer[i];
        if (use_framebuffer) {
            fb_write_char(c);
        }
    }
    if (use_framebuffer) {
        fb_update_cursor();
    }
}

void terminal_setcolor(uint8_t color) {
	terminal_color = color;
}

void terminal_putentryat(unsigned char c, uint8_t color, size_t x, size_t y) {
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}
void terminal_scroll(void) {
    if (use_framebuffer) {
        fb_scroll();
        return;
    }
	const size_t bytes = VGA_WIDTH * (VGA_HEIGHT - 1) * sizeof(uint16_t);
	memmove(terminal_buffer, terminal_buffer + VGA_WIDTH, bytes);
	const size_t last_row = VGA_HEIGHT - 1;
	for (size_t x = 0; x < VGA_WIDTH; x++) {
		terminal_putentryat(' ', terminal_color, x, last_row);
	}
}

void terminal_putchar(char c) {
    if (early_boot_capture && early_boot_index < EARLY_BOOT_BUFFER_SIZE - 1) {
        early_boot_buffer[early_boot_index++] = c;
    }

    if (use_framebuffer && console_is_initialized()) {
        console_putchar_to(0, c);
        return;
    }

    serial_putchar(c);

    if (use_framebuffer) {
        fb_write_char(c);
        return;
    }
    unsigned char uc = c;

    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
        terminal_updatecursor();
        return;
    }

    if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
        }
        terminal_updatecursor();
        return;
    }

    terminal_putentryat(uc, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
    }
    terminal_updatecursor();
}

void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++)
		terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
	terminal_write(data, strlen(data));
}

int terminal_load_psf_font(const void *data, size_t size) {
    if (!use_framebuffer) {
        return -1;
    }
    if (psf_load(data, size, &loaded_font) == 0) {
        fb_set_font(&loaded_font);
        fb_clear();
        return 0;
    }
    return -1;
}
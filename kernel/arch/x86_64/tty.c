#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <lebirun/tty.h>
#include <lebirun/framebuffer.h>
#include <lebirun/psf.h>
#include <lebirun/console.h>
#include <lebirun/io.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>

#include "vga.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
static uint16_t* const VGA_MEMORY = (uint16_t*) (0xB8000 + KERNEL_VMA);

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;
static bool use_framebuffer = false;
static psf_font_t loaded_font;

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

void terminal_init_fb(uint64_t addr, uint64_t width, uint64_t height, uint64_t pitch, uint8_t bpp, uint8_t type) {
    if (fb_init(addr, width, height, pitch, bpp, type) == 0) {
        use_framebuffer = true;
        fb_set_colors(0xFFAAAAAA, 0xFF000000);
    }
}

void terminal_replay_early_boot(void) {
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
    if (console_is_initialized()) {
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
       if (console_is_initialized()) {
		console_write_to(0, data, size);
		return;
	}
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
        return 0;
    }
    return -1;
}

void terminal_compact_font(uint64_t max_glyphs) {
    uint64_t keep;
    uint64_t copy_size;
    uint8_t *compact;

    if (!loaded_font.glyphs || !loaded_font.bytesperglyph) return;
    keep = loaded_font.numglyph;
    if (keep <= max_glyphs) return;
    keep = max_glyphs;
    copy_size = keep * loaded_font.bytesperglyph;
    compact = (uint8_t *)kmalloc(copy_size);
    if (!compact) return;
    memcpy(compact, loaded_font.glyphs, copy_size);
    loaded_font.glyphs = compact;
    loaded_font.numglyph = keep;
    loaded_font.unicode_table = 0;
    loaded_font.unicode_table_size = 0;
    loaded_font.owns_data = 1;
}

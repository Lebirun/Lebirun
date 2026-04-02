#include <kernel/framebuffer.h>
#include <kernel/mem_map.h>
#include <kernel/drivers/fb/bga.h>
#include <kernel/drivers/fb/vga_modes.h>
#include <kernel/console.h>
#include <kernel/io.h>
#include <string.h>

static framebuffer_t fb;
static uint64_t cursor_prev_x = 0;
static uint64_t cursor_prev_y = 0;
static int cursor_visible = 1;
static int cursor_drawn = 0;
static int cursor_hidden_by_app = 0;
static int is_tty_mode = 0;
static uint64_t original_fb_width = 0;
static uint64_t original_fb_height = 0;
static uint64_t original_fb_pitch = 0;
static uint64_t original_fb_size = 0;
static uint64_t hw_width = 0;
static uint64_t hw_height = 0;
static uint64_t hw_pitch = 0;
static uint64_t fb_vram_bytes = 0;
static uint64_t mapped_pages_count = 0;

static uint64_t *vram_addr = 0;

static int fb_graphical = 0;
static uint16_t *vga_text_mem = (uint16_t *)(0xB8000 + KERNEL_VMA);
static uint8_t vga_cur_attr = 0x07;

static const uint8_t ansi_to_vga[16] = {
    0, 4, 2, 6, 1, 5, 3, 7,
    8, 12, 10, 14, 9, 13, 11, 15
};

static uint8_t rgb_to_vga_index(uint64_t rgb) {
    int i;
    static const uint64_t normal[8] = {
        0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAA5500,
        0xFF0000AA, 0xFFAA00AA, 0xFF00AAAA, 0xFFAAAAAA
    };
    static const uint64_t intense[8] = {
        0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
        0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF
    };
    for (i = 0; i < 8; i++) {
        if (rgb == normal[i]) return ansi_to_vga[i];
        if (rgb == intense[i]) return ansi_to_vga[i + 8];
    }
    return 7;
}

static void vga_text_update_cursor(uint64_t x, uint64_t y) {
    uint16_t pos;
    pos = (uint16_t)(y * 80 + x);
    outb(0x3D4, 14);
    outb(0x3D5, (pos >> 8) & 0xFF);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

void fb_flush(void) {
}

static uint64_t decrease_width_step(uint64_t value, uint64_t step) {
    uint64_t delta;
    if (step == 0 || value <= step) {
        return 0;
    }
    delta = value % step;
    if (delta == 0) {
        delta = step;
    }
    if (delta > value) {
        return 0;
    }
    return value - delta;
}

static uint64_t decrease_step(uint64_t value, uint64_t step) {
    uint64_t delta;
    if (step == 0 || value <= step) {
        return 0;
    }
    delta = value % step;
    if (delta == 0) {
        delta = step;
    }
    if (delta > value) {
        return 0;
    }
    return value - delta;
}

static inline void tlb_flush_page(uint64_t addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline void tlb_flush_all(void) {
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    asm volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
}

static inline void memory_barrier(void) {
    asm volatile("mfence" ::: "memory");
}

#define MAX_COLS 160
static uint64_t screen_buffer_rows = 0;
static char (*screen_buffer)[MAX_COLS];
static uint32_t (*screen_fg_buf)[MAX_COLS];
static uint32_t (*screen_bg_buf)[MAX_COLS];

static void fb_grow_screen_buffer(uint64_t needed_rows) {
    uint64_t new_rows;
    char (*new_buf)[MAX_COLS];
    uint32_t (*new_fg)[MAX_COLS];
    uint32_t (*new_bg)[MAX_COLS];

    if (needed_rows <= screen_buffer_rows) return;
    new_rows = needed_rows;
    new_buf = (char (*)[MAX_COLS])kmalloc(new_rows * MAX_COLS);
    if (!new_buf) return;
    new_fg = (uint32_t (*)[MAX_COLS])kmalloc(new_rows * MAX_COLS * sizeof(uint32_t));
    if (!new_fg) { kfree(new_buf); return; }
    new_bg = (uint32_t (*)[MAX_COLS])kmalloc(new_rows * MAX_COLS * sizeof(uint32_t));
    if (!new_bg) { kfree(new_buf); kfree(new_fg); return; }
    memset(new_buf, ' ', new_rows * MAX_COLS);
    memset(new_fg, 0xFF, new_rows * MAX_COLS * sizeof(uint32_t));
    memset(new_bg, 0, new_rows * MAX_COLS * sizeof(uint32_t));
    if (screen_buffer && screen_buffer_rows > 0) {
        memcpy(new_buf, screen_buffer, screen_buffer_rows * MAX_COLS);
        kfree(screen_buffer);
    }
    if (screen_fg_buf && screen_buffer_rows > 0) {
        memcpy(new_fg, screen_fg_buf, screen_buffer_rows * MAX_COLS * sizeof(uint32_t));
        kfree(screen_fg_buf);
    }
    if (screen_bg_buf && screen_buffer_rows > 0) {
        memcpy(new_bg, screen_bg_buf, screen_buffer_rows * MAX_COLS * sizeof(uint32_t));
        kfree(screen_bg_buf);
    }
    screen_buffer = new_buf;
    screen_fg_buf = new_fg;
    screen_bg_buf = new_bg;
    screen_buffer_rows = new_rows;
}

static void fb_restore_cell(uint64_t cx, uint64_t cy) {
    uint64_t saved_fg;
    uint64_t saved_bg;
    char ch;

    ch = ' ';
    if (cy < screen_buffer_rows && cx < MAX_COLS && screen_buffer)
        ch = screen_buffer[cy][cx];

    saved_fg = fb.fg_color;
    saved_bg = fb.bg_color;

    if (cy < screen_buffer_rows && cx < MAX_COLS && screen_fg_buf && screen_bg_buf) {
        fb.fg_color = screen_fg_buf[cy][cx];
        fb.bg_color = screen_bg_buf[cy][cx];
    }

    fb_putchar(ch, cx, cy);

    fb.fg_color = saved_fg;
    fb.bg_color = saved_bg;
}

static void fb_draw_cursor_block(uint64_t cx, uint64_t cy, uint64_t color) {
    uint64_t px;
    uint64_t py;
    uint64_t row;
    uint64_t col;
    if (!fb.font) return;
    px = cx * fb.font->width;
    py = cy * fb.font->height + fb.font->height - 2;
    for (row = 0; row < 2; row++) {
        for (col = 0; col < fb.font->width; col++) {
            if (px + col < fb.width && py + row < fb.height) {
                fb_putpixel(px + col, py + row, color);
            }
        }
    }
}

static uint8_t default_font_data[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7E,0x81,0xA5,0x81,0x81,0xBD,0x99,0x81,0x81,0x7E,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7E,0xFF,0xDB,0xFF,0xFF,0xC3,0xE7,0xFF,0xFF,0x7E,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6C,0xFE,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x18,0x3C,0x3C,0xE7,0xE7,0xE7,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xE7,0xC3,0xC3,0xE7,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xC3,0x99,0xBD,0xBD,0x99,0xC3,0xFF,0xFF,0xFF,0xFF,0xFF,
    0x00,0x00,0x1E,0x0E,0x1A,0x32,0x78,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3F,0x33,0x3F,0x30,0x30,0x30,0x30,0x70,0xF0,0xE0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7F,0x63,0x7F,0x63,0x63,0x63,0x63,0x67,0xE7,0xE6,0xC0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x18,0x18,0xDB,0x3C,0xE7,0x3C,0xDB,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x80,0xC0,0xE0,0xF0,0xF8,0xFE,0xF8,0xF0,0xE0,0xC0,0x80,0x00,0x00,0x00,0x00,
    0x00,0x02,0x06,0x0E,0x1E,0x3E,0xFE,0x3E,0x1E,0x0E,0x06,0x02,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x3C,0x7E,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x66,0x66,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7F,0xDB,0xDB,0xDB,0x7B,0x1B,0x1B,0x1B,0x1B,0x1B,0x00,0x00,0x00,0x00,
    0x00,0x7C,0xC6,0x60,0x38,0x6C,0xC6,0xC6,0x6C,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0xFE,0xFE,0xFE,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x3C,0x7E,0x18,0x18,0x18,0x7E,0x3C,0x18,0x7E,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x18,0x0C,0xFE,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x30,0x60,0xFE,0x60,0x30,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0xC0,0xC0,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x24,0x66,0xFF,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x10,0x38,0x38,0x7C,0x7C,0xFE,0xFE,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFE,0xFE,0x7C,0x7C,0x38,0x38,0x10,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,
    0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00,
    0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00,
    0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,
    0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,
    0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,
    0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC3,0xE7,0xFF,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00,
    0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,
    0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3,0xC3,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFF,0xC3,0x86,0x0C,0x18,0x30,0x60,0xC1,0xC3,0xFF,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00,
    0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,
    0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,
    0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,
    0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,
    0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,
    0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xE6,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,
    0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00,
    0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00,
    0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,
    0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,
    0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,
    0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00,
};

static psf_font_t default_font = {
    .version = 1,
    .width = 8,
    .height = 16,
    .bytesperglyph = 16,
    .numglyph = 128,
    .glyphs = default_font_data,
    .unicode_table = 0,
    .unicode_table_size = 0
};

const uint8_t *fb_get_default_font_data(void) {
    return default_font_data;
}

static void fb_clear_region(uint64_t start_y, uint64_t end_y) {
    uint64_t bytes_per_pixel;
    uint64_t line_bytes;
    uint64_t y;
    uint8_t *row;
    uint64_t x;
    uint8_t *p;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint16_t rgb565;
    uint32_t *p32;
    uint32_t bg32;
    
    bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
    if (bytes_per_pixel == 0) return;
    if (end_y > hw_height) end_y = hw_height;
    if (start_y >= end_y) return;

    line_bytes = hw_width * bytes_per_pixel;
    if (line_bytes > hw_pitch) line_bytes = hw_pitch;

    for (y = start_y; y < end_y; y++) {
        row = (uint8_t *)fb.addr + y * hw_pitch;
        if (fb.bpp == 32) {
            if (fb.bg_color == 0) {
                memset(row, 0, line_bytes);
            } else {
                p32 = (uint32_t *)row;
                bg32 = (uint32_t)fb.bg_color;
                
                for (x = 0; x < hw_width; x++) {
                    p32[x] = bg32;
                }
            }
        } else if (fb.bg_color == 0) {
            memset(row, 0, line_bytes);
        } else {
            for (x = 0; x < hw_width; x++) {
                p = row + x * bytes_per_pixel;
                if (fb.bpp == 24) {
                    p[0] = (uint8_t)(fb.bg_color & 0xFF);
                    p[1] = (uint8_t)((fb.bg_color >> 8) & 0xFF);
                    p[2] = (uint8_t)((fb.bg_color >> 16) & 0xFF);
                } else if (fb.bpp == 16) {
                    r = (uint8_t)((fb.bg_color >> 16) & 0xFF);
                    g = (uint8_t)((fb.bg_color >> 8) & 0xFF);
                    b = (uint8_t)(fb.bg_color & 0xFF);
                    rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    *(uint16_t *)p = rgb565;
                }
            }
        }
    }
}

int fb_init(uint64_t addr, uint64_t width, uint64_t height, uint64_t pitch, uint8_t bpp, uint8_t type) {
    uint64_t fb_phys;
    uint64_t fb_size;
    uint64_t num_pages;
    uint64_t i;
    uint64_t phys;
    uint64_t virt;
    uint64_t bytes_per_pixel;

    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = bpp;
    fb.type = type;
    fb.phys_addr = addr;
    fb.refresh_rate = 60;
    
    fb_phys = (uint64_t)addr;
    fb_size = pitch * height;
    num_pages = (fb_size + 0xFFF) / 0x1000;
    
    for (i = 0; i < num_pages; i++) {
        phys = fb_phys + (i * 0x1000);
        virt = (KERNEL_VMA + 0x20000000ULL) + (i * 0x1000);
        vmm_map_page(virt, phys, 0x003);
    }
    mapped_pages_count = num_pages;
    
    tlb_flush_all();
    memory_barrier();
    
    vram_addr = (uint64_t *)(KERNEL_VMA + 0x20000000ULL);
    fb.addr = vram_addr;

    fb.font = &default_font;
    fb.fg_color = 0xFFFFFFFF;
    fb.bg_color = 0x00000000;
    fb.cursor_x = 0;
    fb.cursor_y = 0;
    fb.cols = width / fb.font->width;
    fb.rows = height / fb.font->height;
    if (fb.cols > MAX_COLS) fb.cols = MAX_COLS;
    
    original_fb_width = width;
    original_fb_height = height;
    original_fb_pitch = pitch;
    original_fb_size = pitch * height;
    fb_vram_bytes = original_fb_size;
    if (bga_is_available()) {
        uint64_t vram;
        vram = bga_get_vram_bytes();
        if (vram > fb_vram_bytes) fb_vram_bytes = vram;
    } else if (vga_is_cirrus()) {
        uint64_t vram;
        vram = vga_get_vram_bytes();
        if (vram > fb_vram_bytes) fb_vram_bytes = vram;
    }
    hw_height = height;
    hw_pitch = pitch;
    bytes_per_pixel = (uint64_t)(bpp / 8u);
    hw_width = bytes_per_pixel ? (pitch / bytes_per_pixel) : width;
    
    is_tty_mode = (type == 0);
    
    if (!screen_buffer) {
        fb_grow_screen_buffer(fb.rows > 0 ? fb.rows : 25);
    } else if (fb.rows > screen_buffer_rows) {
        fb_grow_screen_buffer(fb.rows);
    }

    fb_clear();
    
    fb_graphical = 1;
    return 0;
}

void fb_init_textmode(const uint8_t *font_glyphs, uint16_t num_chars, uint8_t font_height) {
    if (font_height == 0) font_height = 16;
    __asm__ volatile("outw %0, %1" : : "a"((uint16_t)4), "Nd"((uint16_t)0x01CE));
    __asm__ volatile("outw %0, %1" : : "a"((uint16_t)0), "Nd"((uint16_t)0x01CF));
    vga_set_text_mode(font_glyphs, num_chars, font_height);

    fb.rows = 25;
    fb.cols = 80;
    fb.font = NULL;
    fb.addr = NULL;
    fb_graphical = 0;
    fb.cursor_x = 0;
    fb.cursor_y = 0;
}

void fb_set_font(psf_font_t *font) {
    uint64_t old_cols;
    uint64_t old_rows;
    uint64_t redraw_rows;
    uint64_t redraw_cols;
    uint64_t r;
    uint64_t c;
    uint64_t clear_height;
    uint64_t bytes_per_pixel;

    if (font && font->glyphs) {
        old_cols = fb.cols;
        old_rows = fb.rows;
        fb.font = font;
        if (!font->width || !font->height) {
            fb.cols = 0;
            fb.rows = 0;
        } else {
            fb.cols = fb.width / font->width;
            fb.rows = fb.height / font->height;
        }
        if (fb.cols > MAX_COLS) fb.cols = MAX_COLS;
        if (fb.cursor_x >= fb.cols && fb.cols) fb.cursor_x = fb.cols - 1;
        if (fb.cursor_y >= fb.rows && fb.rows) fb.cursor_y = fb.rows - 1;

        if (console_is_initialized()) {
            if (old_cols != fb.cols) {
                console_rewrap_all(old_cols, fb.cols, fb.rows);
            }
            console_clamp_cursors(fb.cols, fb.rows);
            if (fb_graphical) {
                clear_height = (fb.height < hw_height) ? fb.height : hw_height;
                if (clear_height > 0) fb_clear_region(0, clear_height);
            }
            console_redraw_current();
        } else if (screen_buffer && screen_buffer_rows > 0 && fb.cols > 0 && fb.rows > 0 && fb_graphical) {
            bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
            clear_height = (fb.height < hw_height) ? fb.height : hw_height;
            if (clear_height > 0 && bytes_per_pixel > 0) fb_clear_region(0, clear_height);
            redraw_rows = fb.rows;
            if (redraw_rows > screen_buffer_rows) redraw_rows = screen_buffer_rows;
            if (redraw_rows > old_rows && old_rows > 0) redraw_rows = old_rows;
            redraw_cols = fb.cols;
            if (redraw_cols > old_cols && old_cols > 0) redraw_cols = old_cols;
            for (r = 0; r < redraw_rows; r++) {
                for (c = 0; c < redraw_cols; c++) {
                    if ((unsigned char)screen_buffer[r][c] >= 32) {
                        fb_putchar(screen_buffer[r][c], c, r);
                    }
                }
            }
        } else {
            fb_clear();
        }

        cursor_drawn = 0;
        cursor_prev_x = fb.cursor_x;
        cursor_prev_y = fb.cursor_y;
    }
}

void fb_set_colors(uint64_t fg, uint64_t bg) {
    uint8_t vga_fg;
    uint8_t vga_bg;

    fb.fg_color = fg;
    fb.bg_color = bg;
    if (!fb_graphical) {
        vga_fg = rgb_to_vga_index(fg);
        vga_bg = rgb_to_vga_index(bg);
        vga_cur_attr = (uint8_t)((vga_bg << 4) | (vga_fg & 0x0F));
    }
}

void fb_clear(void) {
    uint64_t bytes_per_pixel;
    uint64_t clear_height;
    uint64_t i;

    if (!fb_graphical) {
        volatile uint16_t *vga = (volatile uint16_t *)vga_text_mem;
        for (i = 0; i < 16384; i++) {
            vga[i] = (uint16_t)(' ' | ((uint16_t)vga_cur_attr << 8));
        }
        fb.cursor_x = 0;
        fb.cursor_y = 0;
        return;
    }

    bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
    if (bytes_per_pixel == 0) return;

    clear_height = (fb.height < hw_height) ? fb.height : hw_height;
    if (clear_height == 0) return;

    fb_clear_region(0, clear_height);
    
    fb.cursor_x = 0;
    fb.cursor_y = 0;
    
    if (screen_buffer && screen_buffer_rows > 0) memset(&screen_buffer[0][0], ' ', screen_buffer_rows * MAX_COLS);
    if (screen_fg_buf && screen_buffer_rows > 0) memset(&screen_fg_buf[0][0], 0xFF, screen_buffer_rows * MAX_COLS * sizeof(uint32_t));
    if (screen_bg_buf && screen_buffer_rows > 0) memset(&screen_bg_buf[0][0], 0, screen_buffer_rows * MAX_COLS * sizeof(uint32_t));
    
    cursor_drawn = 0;
    cursor_prev_x = 0;
    cursor_prev_y = 0;
}

void fb_putpixel(uint64_t x, uint64_t y, uint64_t color) {
    uint64_t bytes_per_pixel;
    uint64_t offset;
    uint64_t fb_size;
    uint8_t *p;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint16_t rgb565;

    if (x >= hw_width || y >= hw_height) {
        return;
    }
    bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
    if (bytes_per_pixel == 0) {
        return;
    }
    offset = y * hw_pitch + x * bytes_per_pixel;
    fb_size = hw_pitch * hw_height;
    if (offset >= fb_size || fb_size - offset < bytes_per_pixel) {
        return;
    }

    p = (uint8_t *)fb.addr + offset;
    if (fb.bpp == 32) {
        *(uint32_t *)p = (uint32_t)color;
    } else if (fb.bpp == 24) {
        p[0] = (uint8_t)(color & 0xFF);
        p[1] = (uint8_t)((color >> 8) & 0xFF);
        p[2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (fb.bpp == 16) {
        r = (uint8_t)((color >> 16) & 0xFF);
        g = (uint8_t)((color >> 8) & 0xFF);
        b = (uint8_t)(color & 0xFF);
        rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        *(uint16_t *)p = rgb565;
    }
}

void fb_putchar(char c, uint64_t cx, uint64_t cy) {
    const uint8_t *glyph;
    uint64_t px;
    uint64_t py;
    uint64_t bytes_per_pixel;
    uint64_t bytes_per_line;
    uint8_t *base;
    uint32_t fg;
    uint32_t bg;
    uint64_t row;
    uint8_t bits;
    uint32_t *p32;
    uint64_t col;
    uint64_t byte_idx;
    uint8_t bit;
    uint32_t color;
    uint8_t *p;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint16_t rgb565;
    uint8_t uc;

    if (!fb_graphical) {
        if (cx < 80 && cy < 25) {
            vga_text_mem[cy * 80 + cx] = (uint16_t)((uint8_t)c | ((uint16_t)vga_cur_attr << 8));
        }
        return;
    }

    if (!fb.font || !fb.font->glyphs) {
        return;
    }

    if (cx >= fb.cols || cy >= fb.rows) {
        return;
    }
    
    if (cy < screen_buffer_rows && cx < MAX_COLS && screen_buffer) {
        screen_buffer[cy][cx] = c;
        if (screen_fg_buf) screen_fg_buf[cy][cx] = (uint32_t)fb.fg_color;
        if (screen_bg_buf) screen_bg_buf[cy][cx] = (uint32_t)fb.bg_color;
    }
    
    uc = (uint8_t)c;
    glyph = psf_get_glyph(fb.font, uc);
    if (!glyph) {
        return;
    }
    
    px = cx * fb.font->width;
    py = cy * fb.font->height;

    bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
    if (bytes_per_pixel == 0) return;

    bytes_per_line = fb.font->bytesperglyph / fb.font->height;
    if (bytes_per_line == 0) bytes_per_line = (fb.font->width + 7) / 8;

    if (fb.bpp == 32 && fb.font->width == 8 && bytes_per_line == 1) {
        base = (uint8_t *)fb.addr;
        fg = (uint32_t)fb.fg_color;
        bg = (uint32_t)fb.bg_color;
        for (row = 0; row < fb.font->height; row++) {
            bits = glyph[row];
            p32 = (uint32_t *)(base + (py + row) * hw_pitch + px * 4u);
            p32[0] = (bits & 0x80) ? fg : bg;
            p32[1] = (bits & 0x40) ? fg : bg;
            p32[2] = (bits & 0x20) ? fg : bg;
            p32[3] = (bits & 0x10) ? fg : bg;
            p32[4] = (bits & 0x08) ? fg : bg;
            p32[5] = (bits & 0x04) ? fg : bg;
            p32[6] = (bits & 0x02) ? fg : bg;
            p32[7] = (bits & 0x01) ? fg : bg;
        }
        return;
    }

    base = (uint8_t *)fb.addr;
    for (row = 0; row < fb.font->height; row++) {
        p = base + (py + row) * hw_pitch + px * bytes_per_pixel;
        for (col = 0; col < fb.font->width; col++) {
            byte_idx = row * bytes_per_line + col / 8;
            bit = 7 - (col % 8);
            color = (glyph[byte_idx] & (1u << bit)) ? (uint32_t)fb.fg_color : (uint32_t)fb.bg_color;
            if (fb.bpp == 32) {
                *((uint32_t *)(p + col * bytes_per_pixel)) = color;
            } else if (fb.bpp == 24) {
                p[col * bytes_per_pixel] = (uint8_t)(color & 0xFF);
                p[col * bytes_per_pixel + 1] = (uint8_t)((color >> 8) & 0xFF);
                p[col * bytes_per_pixel + 2] = (uint8_t)((color >> 16) & 0xFF);
            } else if (fb.bpp == 16) {
                r = (uint8_t)((color >> 16) & 0xFF);
                g = (uint8_t)((color >> 8) & 0xFF);
                b = (uint8_t)(color & 0xFF);
                rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                *((uint16_t *)(p + col * bytes_per_pixel)) = rgb565;
            }
        }
    }
}

void fb_scroll(void) {
    uint64_t line_height;
    uint64_t copy_height;
    uint64_t bytes_per_pixel;
    uint64_t copy_bytes;
    uint64_t last_row_start;
    uint64_t clear_end;
    uint64_t scroll_rows;
    uint64_t scroll_cols;
    uint64_t col;
    uint64_t x;

    if (fb.rows == 0 || fb.cols == 0) {
        return;
    }

    if (!fb_graphical) {
        memmove(vga_text_mem, vga_text_mem + 80, 80 * 24 * sizeof(uint16_t));
        for (x = 0; x < 80; x++) {
            vga_text_mem[24 * 80 + x] = (uint16_t)(' ' | ((uint16_t)vga_cur_attr << 8));
        }
        return;
    }

    if (cursor_drawn) {
        fb_restore_cell(cursor_prev_x, cursor_prev_y);
        cursor_drawn = 0;
    }

    line_height = fb.font->height;
    copy_height = (fb.rows - 1) * line_height;

    bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
    if (bytes_per_pixel == 0) return;

    copy_bytes = copy_height * hw_pitch;
    memmove((uint8_t *)fb.addr, (uint8_t *)fb.addr + line_height * hw_pitch, copy_bytes);

    last_row_start = (fb.rows - 1) * line_height;
    clear_end = last_row_start + line_height;
    if (clear_end > hw_height) clear_end = hw_height;
    if (last_row_start < hw_height) {
        fb_clear_region(last_row_start, clear_end);
    }

    scroll_rows = (fb.rows > 1 && fb.rows <= screen_buffer_rows) ? fb.rows - 1 : 0;
    scroll_cols = (fb.cols <= MAX_COLS) ? fb.cols : MAX_COLS;

    if (scroll_rows > 0 && scroll_cols > 0 && scroll_rows < screen_buffer_rows && screen_buffer) {
        memmove(&screen_buffer[0][0], &screen_buffer[1][0],
                scroll_rows * MAX_COLS * sizeof(char));
        if (screen_fg_buf)
            memmove(&screen_fg_buf[0][0], &screen_fg_buf[1][0],
                    scroll_rows * MAX_COLS * sizeof(uint32_t));
        if (screen_bg_buf)
            memmove(&screen_bg_buf[0][0], &screen_bg_buf[1][0],
                    scroll_rows * MAX_COLS * sizeof(uint32_t));

        if (fb.rows > 0 && fb.rows - 1 < screen_buffer_rows) {
            for (col = 0; col < scroll_cols; col++) {
                screen_buffer[fb.rows - 1][col] = ' ';
                if (screen_fg_buf) screen_fg_buf[fb.rows - 1][col] = (uint32_t)fb.fg_color;
                if (screen_bg_buf) screen_bg_buf[fb.rows - 1][col] = (uint32_t)fb.bg_color;
            }
        }
    }

    if (cursor_prev_y > 0) {
        cursor_prev_y--;
    }
}

void fb_write_char(char c) {
    uint64_t tab_stop;
    uint64_t i;

    if (fb.rows == 0 || fb.cols == 0) {
        return;
    }
    if (c == '\n') {
        fb.cursor_x = 0;
        fb.cursor_y++;
        if (fb.cursor_y >= fb.rows) {
            fb_scroll();
            fb.cursor_y = fb.rows - 1;
        }
        return;
    }
    
    if (c == '\r') {
        fb.cursor_x = 0;
        return;
    }
    
    if (c == '\b') {
        if (fb.cursor_x > 0) {
            fb.cursor_x--;
            fb_putchar(' ', fb.cursor_x, fb.cursor_y);
        }
        return;
    }
    
    if (c == '\t') {
        tab_stop = 8 - (fb.cursor_x % 8);
        for (i = 0; i < tab_stop; i++) {
            fb_write_char(' ');
        }
        return;
    }
    
    fb_putchar(c, fb.cursor_x, fb.cursor_y);
    fb.cursor_x++;
    
    if (fb.cursor_x >= fb.cols) {
        fb.cursor_x = 0;
        fb.cursor_y++;
        if (fb.cursor_y >= fb.rows) {
            fb_scroll();
            fb.cursor_y = fb.rows - 1;
        }
    }
    fb_flush();
}

void fb_write_string(const char *str) {
    while (*str) {
        fb_write_char(*str++);
    }
}

void fb_update_cursor(void) {

    if (!fb_graphical) {
        vga_text_update_cursor(fb.cursor_x, fb.cursor_y);
        return;
    }

    if (cursor_drawn && (cursor_prev_x != fb.cursor_x || cursor_prev_y != fb.cursor_y)) {
        if (cursor_prev_x < fb.cols && cursor_prev_y < fb.rows) {
            fb_restore_cell(cursor_prev_x, cursor_prev_y);
        }
        cursor_drawn = 0;
    }
    if (cursor_drawn && (!cursor_visible || cursor_hidden_by_app)) {
        if (cursor_prev_x < fb.cols && cursor_prev_y < fb.rows) {
            fb_restore_cell(cursor_prev_x, cursor_prev_y);
        }
        cursor_drawn = 0;
    }
    if (cursor_visible && !cursor_hidden_by_app && !cursor_drawn && fb.cursor_x < fb.cols && fb.cursor_y < fb.rows) {
        fb_draw_cursor_block(fb.cursor_x, fb.cursor_y, fb.fg_color);
        cursor_prev_x = fb.cursor_x;
        cursor_prev_y = fb.cursor_y;
        cursor_drawn = 1;
    }
    fb_flush();
}

extern void console_tick_redraw(void);

void fb_tick(void) {
    static int tick_counter = 0;
    static int flush_counter = 0;

    tick_counter++;
    if (tick_counter >= 500) {
        tick_counter = 0;
        cursor_visible = !cursor_visible;
        fb_update_cursor();
    }
    console_tick_redraw();

    flush_counter++;
    if (flush_counter >= 16) {
        flush_counter = 0;
        fb_flush();
    }
}

framebuffer_t *fb_get(void) {
    return &fb;
}

void fb_set_cursor_hidden(int hidden) {
    cursor_hidden_by_app = hidden;
    fb_update_cursor();
}

int fb_set_mode(uint64_t width, uint64_t height, uint64_t refresh_rate) {
    uint64_t font_width;
    uint64_t font_height;
    uint64_t req_width;
    uint64_t req_height;
    int hw_changed;
    uint64_t new_pitch;
    uint8_t new_bpp;
    uint16_t try_bpps[3];
    int i;
    uint16_t bpp_try;
    uint64_t candidate_h;
    int attempts_h;
    uint64_t candidate_w;
    int attempts_w;
    uint64_t next_w;
    uint64_t next_h;
    uint64_t row;
    uint64_t col;
    uint64_t fb_size;
    uint64_t bytes_per_pixel;
    uint64_t fb_phys;
    uint64_t num_pages;
    uint64_t page_idx;
    uint64_t phys;
    uint64_t virt;
    uint64_t line_pixels;
    uint64_t effective_width;
    uint64_t old_mapped_pages;
    uint64_t batch_size;
    uint64_t batch_start;
    uint64_t batch_end;

    if (original_fb_width == 0 || original_fb_height == 0) {
        return -3;
    }

    if (!fb_graphical) {
        return -3;
    }

    if (width == 0 || height == 0) {
        return -2;
    }

    font_width = (fb.font && fb.font->width) ? fb.font->width : 8;
    font_height = (fb.font && fb.font->height) ? fb.font->height : 16;
    if (font_width == 0) font_width = 8;
    if (font_height == 0) font_height = 16;

    req_width = width;
    req_height = height;

    hw_changed = 0;
    new_pitch = hw_pitch;
    new_bpp = fb.bpp;

    if (bga_is_available()) {
        try_bpps[0] = 32;
        try_bpps[1] = 24;
        try_bpps[2] = 16;
        for (i = 0; i < 3 && !hw_changed; i++) {
            bpp_try = try_bpps[i];
            candidate_h = req_height;
            attempts_h = 0;
            while (candidate_h > 0 && attempts_h < 64 && !hw_changed) {
                candidate_w = req_width;
                attempts_w = 0;
                while (candidate_w > 0 && attempts_w < 64 && !hw_changed) {
                    if (bga_set_mode((uint16_t)candidate_w, (uint16_t)candidate_h, bpp_try, &new_pitch) == 0) {
                        hw_changed = 1;
                        new_bpp = (uint8_t)bpp_try;
                        width = candidate_w;
                        height = candidate_h;
                        break;
                    }
                    next_w = decrease_width_step(candidate_w, font_width);
                    if (next_w == 0 || next_w == candidate_w) break;
                    candidate_w = next_w;
                    attempts_w++;
                }
                if (hw_changed) break;
                next_h = decrease_step(candidate_h, font_height);
                if (next_h == 0 || next_h == candidate_h) break;
                candidate_h = next_h;
                attempts_h++;
            }
        }
    }

    if (!hw_changed && vga_is_cirrus()) {
        try_bpps[0] = 32;
        try_bpps[1] = 24;
        try_bpps[2] = 16;
        for (i = 0; i < 3 && !hw_changed; i++) {
            bpp_try = try_bpps[i];
            candidate_h = req_height;
            attempts_h = 0;
            while (candidate_h > 0 && attempts_h < 64 && !hw_changed) {
                candidate_w = req_width;
                attempts_w = 0;
                while (candidate_w > 0 && attempts_w < 64 && !hw_changed) {
                    if (vga_set_mode((uint16_t)candidate_w, (uint16_t)candidate_h, bpp_try, &new_pitch) == 0) {
                        hw_changed = 1;
                        new_bpp = (uint8_t)bpp_try;
                        width = candidate_w;
                        height = candidate_h;
                        break;
                    }
                    next_w = decrease_width_step(candidate_w, font_width);
                    if (next_w == 0 || next_w == candidate_w) break;
                    candidate_w = next_w;
                    attempts_w++;
                }
                if (hw_changed) break;
                next_h = decrease_step(candidate_h, font_height);
                if (next_h == 0 || next_h == candidate_h) break;
                candidate_h = next_h;
                attempts_h++;
            }
        }
    }

    if (!hw_changed) {
        if (width > hw_width) width = hw_width;
        if (height > hw_height) height = hw_height;
        if (font_width) {
            width = (width / font_width) * font_width;
        }
        if (font_height) {
            height = (height / font_height) * font_height;
        }
        if (width == 0 || height == 0) {
            return -2;
        }
    }

    (void)row;
    (void)col;

    if (hw_changed) {
        fb_size = new_pitch * height;
        if (fb_vram_bytes && fb_size > fb_vram_bytes) {
            return -5;
        }

        bytes_per_pixel = (uint64_t)(new_bpp / 8u);
        if (bytes_per_pixel == 0) {
            return -6;
        }

        old_mapped_pages = mapped_pages_count;
        
        hw_pitch = new_pitch;
        hw_height = height;
        hw_width = new_pitch / bytes_per_pixel;

        fb.pitch = new_pitch;
        fb.bpp = new_bpp;
        original_fb_width = hw_width;
        original_fb_height = height;
        original_fb_pitch = new_pitch;
        original_fb_size = new_pitch * height;

        fb_phys = (uint64_t)fb.phys_addr;
        num_pages = (fb_size + 0xFFF) / 0x1000;

        if (num_pages > old_mapped_pages) {
            batch_size = 64;

            for (batch_start = old_mapped_pages; batch_start < num_pages; batch_start += batch_size) {
                batch_end = batch_start + batch_size;
                if (batch_end > num_pages) batch_end = num_pages;

                for (page_idx = batch_start; page_idx < batch_end; page_idx++) {
                    phys = fb_phys + (page_idx * 0x1000);
                    virt = (KERNEL_VMA + 0x20000000ULL) + (page_idx * 0x1000);
                    vmm_map_page(virt, phys, 0x003);
                    tlb_flush_page(virt);
                }

                memory_barrier();
                asm volatile("pause; pause; pause; pause; pause; pause; pause; pause");
            }
        } else {
            for (page_idx = 0; page_idx < num_pages; page_idx++) {
                phys = fb_phys + (page_idx * 0x1000);
                virt = (KERNEL_VMA + 0x20000000ULL) + (page_idx * 0x1000);
                vmm_map_page(virt, phys, 0x003);
                if ((page_idx & 0x1F) == 0x1F) {
                    tlb_flush_page(virt);
                    asm volatile("pause; pause");
                }
            }
        }

        mapped_pages_count = num_pages;

        tlb_flush_all();
        memory_barrier();

        asm volatile("pause; pause; pause; pause; pause; pause; pause; pause");

        vram_addr = (uint64_t *)(KERNEL_VMA + 0x20000000ULL);
        fb.addr = vram_addr;
    }

    if (refresh_rate > 0) {
        fb.refresh_rate = refresh_rate;
    }

    bytes_per_pixel = (uint64_t)(fb.bpp / 8u);
    if (bytes_per_pixel == 0 || hw_pitch == 0) {
        return -6;
    }

    line_pixels = hw_pitch / bytes_per_pixel;
    if (line_pixels == 0) {
        return -6;
    }

    if (fb.font && fb.font->height) {
        height = (height / fb.font->height) * fb.font->height;
        if (height == 0) {
            return -2;
        }
    }

    effective_width = width;
    if (effective_width > line_pixels) {
        effective_width = line_pixels;
    }
    if (fb.font && fb.font->width) {
        effective_width = (effective_width / fb.font->width) * fb.font->width;
        if (effective_width == 0) {
            return -2;
        }
    }

    fb.width = effective_width;
    fb.height = height;
    if (fb.font && fb.font->width && fb.font->height) {
        uint64_t old_cols;
        uint64_t new_cols_val;
        uint64_t new_rows_val;

        old_cols = fb.cols;
        new_cols_val = effective_width / fb.font->width;
        new_rows_val = height / fb.font->height;
        if (new_cols_val == 0 || new_rows_val == 0) {
            return -2;
        }
        if (new_cols_val > MAX_COLS) new_cols_val = MAX_COLS;

        fb_grow_screen_buffer(new_rows_val);

        if (console_is_initialized() && old_cols != new_cols_val) {
            console_rewrap_all(old_cols, new_cols_val, new_rows_val);
        }

        fb.cols = new_cols_val;
        fb.rows = new_rows_val;
        fb.width = fb.cols * fb.font->width;
    }

    if (fb.cols == 0 || fb.rows == 0) {
        return -2;
    }

    console_clamp_cursors(fb.cols, fb.rows);

    if (fb.cursor_x >= fb.cols) fb.cursor_x = fb.cols - 1;
    if (fb.cursor_y >= fb.rows) fb.cursor_y = fb.rows - 1;

    cursor_drawn = 0;
    cursor_prev_x = 0;
    cursor_prev_y = 0;

    fb_clear_region(0, hw_height);

    if (console_is_initialized()) {
        console_redraw_current();
        return 0;
    }

    fb.cursor_x = 0;
    fb.cursor_y = 0;

    return 0;
}

int fb_get_info(uint64_t *width, uint64_t *height, uint64_t *bpp, uint64_t *refresh_rate) {
    if (!width || !height || !bpp) {
        return -1; 
    }
    
    *width = fb.width;
    *height = fb.height;
    *bpp = fb.bpp;
    
    if (refresh_rate) {
        *refresh_rate = fb.refresh_rate;
    }
    
    return 0; 
}

static inline void map_physical_page(uint64_t phys) {
    uint64_t page;
    if (phys == 0) {
        return;
    }
    page = phys & ~0xFFFu;
    vmm_map_page(page + KERNEL_VMA, page, 0x003);
}


int fb_get_caps(uint64_t *out_words, uint64_t words) {
    uint64_t font_w;
    uint64_t font_h;
    uint64_t flags;

    if (!out_words || words < 16) {
        return -1;
    }

    font_w = (fb.font && fb.font->width) ? fb.font->width : 8;
    font_h = (fb.font && fb.font->height) ? fb.font->height : 16;
    if (font_w == 0) font_w = 8;
    if (font_h == 0) font_h = 16;

    flags = 0;
    if (bga_is_available()) {
        flags |= 1u;
        flags |= 2u;
    } else if (vga_is_cirrus()) {
        flags |= 1u;
        flags |= 4u;
    }

    out_words[0] = fb.width;
    out_words[1] = fb.height;
    out_words[2] = (uint64_t)fb.bpp;
    out_words[3] = fb.pitch;
    out_words[4] = fb.cols;
    out_words[5] = fb.rows;
    out_words[6] = font_w;
    out_words[7] = font_h;
    out_words[8] = hw_width;
    out_words[9] = hw_height;
    out_words[10] = hw_pitch;
    out_words[11] = fb_vram_bytes;
    out_words[12] = flags;
    out_words[13] = fb.refresh_rate;
    out_words[14] = 0;
    out_words[15] = 0;

    return 0;
}
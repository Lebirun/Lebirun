#ifndef _KERNEL_FRAMEBUFFER_H
#define _KERNEL_FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/psf.h>

typedef struct {
    uint32_t *addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t type;
    psf_font_t *font;
    uint32_t fg_color;
    uint32_t bg_color;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cols;
    uint32_t rows;
    uint64_t phys_addr;
    uint32_t refresh_rate;
} framebuffer_t;

int fb_init(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp, uint8_t type);
void fb_set_font(psf_font_t *font);
void fb_set_colors(uint32_t fg, uint32_t bg);
void fb_clear(void);
void fb_putpixel(uint32_t x, uint32_t y, uint32_t color);
void fb_putchar(char c, uint32_t cx, uint32_t cy);
void fb_scroll(void);
void fb_write_char(char c);
void fb_write_string(const char *str);
void fb_update_cursor(void);
void fb_set_cursor_hidden(int hidden);
void fb_tick(void);
void fb_flush(void);
framebuffer_t *fb_get(void);
int fb_set_mode(uint32_t width, uint32_t height, uint32_t refresh_rate);
int fb_get_info(uint32_t *width, uint32_t *height, uint32_t *bpp, uint32_t *refresh_rate);
int fb_get_caps(uint32_t *out_words, uint32_t words);

#endif

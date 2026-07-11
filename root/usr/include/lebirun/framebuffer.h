#ifndef _LEBIRUN_FRAMEBUFFER_H
#define _LEBIRUN_FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/psf.h>

typedef struct {
    uint64_t *addr;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint8_t bpp;
    uint8_t type;
    psf_font_t *font;
    uint64_t fg_color;
    uint64_t bg_color;
    uint64_t cursor_x;
    uint64_t cursor_y;
    uint64_t cols;
    uint64_t rows;
    uint64_t phys_addr;
    uint64_t refresh_rate;
} framebuffer_t;

int fb_init(uint64_t addr, uint64_t width, uint64_t height, uint64_t pitch, uint8_t bpp, uint8_t type);
void fb_init_textmode(const uint8_t *font_glyphs, uint16_t num_chars, uint8_t font_height);
const uint8_t *fb_get_default_font_data(void);
void fb_set_font(psf_font_t *font);
void fb_set_colors(uint64_t fg, uint64_t bg);
void fb_clear(void);
void fb_putpixel(uint64_t x, uint64_t y, uint64_t color);
void fb_putchar(char c, uint64_t cx, uint64_t cy);
void fb_scroll(void);
void fb_write_char(char c);
void fb_write_string(const char *str);
void fb_update_cursor(void);
void fb_set_cursor_hidden(int hidden);
void fb_tick(void);
void fb_flush(void);
framebuffer_t *fb_get(void);
int fb_set_mode(uint64_t width, uint64_t height, uint64_t refresh_rate);
int fb_get_info(uint64_t *width, uint64_t *height, uint64_t *bpp, uint64_t *refresh_rate);
int fb_get_caps(uint64_t *out_words, uint64_t words);
void fb_reclaim_unused(void);

#endif

#ifndef _LEBIRUN_VGA_MODES_H
#define _LEBIRUN_VGA_MODES_H

#include <stdint.h>

int vga_is_cirrus(void);
int vga_set_mode(uint16_t width, uint16_t height, uint16_t bpp, uint64_t *out_pitch);
int vga_set_text_mode(const uint8_t *font_data, uint16_t num_chars, uint8_t font_height);
uint64_t vga_get_framebuffer_base(void);
uint64_t vga_get_vram_bytes(void);

#endif

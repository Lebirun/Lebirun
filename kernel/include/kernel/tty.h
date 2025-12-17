#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <kernel/mutex.h>
#include <stddef.h>
#include <stdint.h>

void terminal_initialize(void);
void terminal_init_fb(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp, uint8_t type);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_setcursor(uint16_t pos);
int terminal_load_psf_font(const void *data, size_t size);

#endif

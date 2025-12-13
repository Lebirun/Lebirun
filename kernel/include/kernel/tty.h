#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <kernel/mutex.h>
#include <stddef.h>
#include <stdint.h>

void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_setcursor(uint16_t pos);

#endif

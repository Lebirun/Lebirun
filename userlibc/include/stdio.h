#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define stdin  0
#define stdout 1
#define stderr 2

int putchar(int c);
int puts(const char *s);
int getchar(void);

int printf(const char *fmt, ...);

#endif

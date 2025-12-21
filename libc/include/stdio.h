#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char* __restrict, ...);
int sprintf(char* __restrict, const char* __restrict, ...);
int snprintf(char* __restrict, size_t, const char* __restrict, ...);
int vsnprintf(char* __restrict, size_t, const char* __restrict, va_list);
int fprintf(FILE*, const char* __restrict, ...);
int putchar(int);
int puts(const char*);
int getchar(void);
int fputc(int, FILE*);
int fputs(const char*, FILE*);
int fgetc(FILE*);

#ifdef __cplusplus
}
#endif

#endif

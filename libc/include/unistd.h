#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((__noreturn__))
void _exit(int status);

int read(int fd, void* buf, size_t count);
int write(int fd, const void* buf, size_t count);

int getpid(void);
int yield(void);
int sleep_ms(int ms);
int waitpid(int pid, int *status, int options);
void *sbrk(int inc);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int kill(int pid, int sig);
unsigned int getticks(void);
unsigned int time(void *unused);
int isatty(int fd);

int fork(void);
int exec(const void *bin, unsigned int size);

int initrd_count(void);
int initrd_stat(int index, char *name, unsigned int *length);
int initrd_read(int index, void *buf, unsigned int maxlen);

int open(const char *path, int flags);
int close(int fd);
int fstat(int fd, unsigned int *size, unsigned char *type);

int fb_putpixel(int x, int y, unsigned int color);
int fb_setcolors(unsigned int fg, unsigned int bg);
int fb_getinfo(unsigned int *width, unsigned int *height, unsigned int *bpp, unsigned int *font_height, unsigned int *rows, unsigned int *cursor_row);
int fb_clear(void);

int console_switch(int console_num);
int console_getcur(void);
int console_clear(int console_num);

#ifdef __cplusplus
}
#endif

#endif

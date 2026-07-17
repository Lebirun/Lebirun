#ifndef LEBIRUN_H_INCLUDED
#define LEBIRUN_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

unsigned int getticks(void);

int initrd_count(void);
int initrd_stat(int index, char *name, unsigned int *length);
int initrd_read(int index, void *buf, unsigned int maxlen);

int vfs_open(const char *path, int flags);
int vfs_close_fd(int fd);
int vfs_read_fd(int fd, void *buf, unsigned int count);
int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index);
int vfs_stat(int fd, uint64_t *size, uint64_t *type);
int vfs_mounts(void);
int vfs_write_fd(int fd, const void *buf, unsigned int count);
int vfs_create(const char *path, unsigned int perms);
int vfs_mkdir(const char *path, unsigned int perms);
int vfs_unlink(const char *path);

int read_nb(int fd, void *buf, size_t count);
int sleep_ms(int ms);
int console_switch(int console_num);
int console_getcur(void);
int console_clear(int console_num);
int console_setcursor(int x, int y);
int console_setid(int console_num);

int fb_setcolors(unsigned int fg, unsigned int bg);
int fb_putpixel(unsigned int x, unsigned int y, unsigned int color);
int fb_getinfo(unsigned int *width, unsigned int *height, unsigned int *bpp,
	unsigned int *font_height, unsigned int *rows, unsigned int *cursor_row,
	unsigned int *font_width, unsigned int *cols);
int fb_clear(void);
int fb_set_mode(unsigned int width, unsigned int height,
	unsigned int refresh_rate);
int fb_getinfo_detailed(unsigned int *width, unsigned int *height,
	unsigned int *bpp, unsigned int *refresh_rate);
int fb_getcaps(unsigned int *words, unsigned int count);

#endif

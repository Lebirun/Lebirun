#include <stddef.h>
#include <stdint.h>

#define LEBIRUN_SYSCALL_FLAG 0x80000000u

#define SYS_GETTICKS        (10u | LEBIRUN_SYSCALL_FLAG)

#define SYS_INITRD_COUNT    (15u | LEBIRUN_SYSCALL_FLAG)
#define SYS_INITRD_STAT     (16u | LEBIRUN_SYSCALL_FLAG)
#define SYS_INITRD_READ     (17u | LEBIRUN_SYSCALL_FLAG)

#define SYS_VFS_OPEN        (28u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_CLOSE       (29u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_READ        (30u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_READDIR     (31u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_STAT        (32u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_MOUNTS      (33u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_WRITE       (34u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_CREATE      (35u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_MKDIR       (36u | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_UNLINK      (37u | LEBIRUN_SYSCALL_FLAG)

#define SYS_CONSOLE_SWITCH  (25u | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_GETCUR  (26u | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_CLEAR   (27u | LEBIRUN_SYSCALL_FLAG)

#define SYS_FB_SETCOLORS    (22u | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_PUTPIXEL     (21u | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_GETINFO      (23u | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_CLEAR        (24u | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_SET_MODE     (259u | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_GET_DETAILED_INFO (260u | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_GET_CAPS     (261u | LEBIRUN_SYSCALL_FLAG)

#define SYS_READ_NB         (39u | LEBIRUN_SYSCALL_FLAG)
#define SYS_SLEEP           (5u  | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_SETCURSOR (38u | LEBIRUN_SYSCALL_FLAG)

static long syscall0(unsigned int n)
{
    long ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static long syscall1(unsigned int n, long a1)
{
    long ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(n), "b"(a1) : "memory");
    return ret;
}

static long syscall2(unsigned int n, long a1, long a2)
{
    long ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(n), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static long syscall3(unsigned int n, long a1, long a2, long a3)
{
    long ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(n), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

static long syscall4(unsigned int n, long a1, long a2, long a3, long a4)
{
    long ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3), "S"(a4)
        : "memory");
    return ret;
}

unsigned int getticks(void)
{
    return (unsigned int)syscall0(SYS_GETTICKS);
}

int initrd_count(void)
{
    return (int)syscall0(SYS_INITRD_COUNT);
}

int initrd_stat(int index, char *name, unsigned int *length)
{
    return (int)syscall3(SYS_INITRD_STAT, index, (long)name, (long)length);
}

int initrd_read(int index, void *buf, unsigned int maxlen)
{
    return (int)syscall3(SYS_INITRD_READ, index, (long)buf, (long)maxlen);
}

int vfs_open(const char *path, int flags)
{
    return (int)syscall2(SYS_VFS_OPEN, (long)path, flags);
}

int vfs_close_fd(int fd)
{
    return (int)syscall1(SYS_VFS_CLOSE, fd);
}

int vfs_read_fd(int fd, void *buf, unsigned int count)
{
    return (int)syscall3(SYS_VFS_READ, fd, (long)buf, (long)count);
}

int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index)
{
    return (int)syscall4(SYS_VFS_READDIR, fd, (long)name, (long)type, (long)index);
}

int vfs_stat(int fd, unsigned int *size, unsigned int *type)
{
    return (int)syscall3(SYS_VFS_STAT, fd, (long)size, (long)type);
}

int vfs_mounts(void)
{
    return (int)syscall0(SYS_VFS_MOUNTS);
}

int vfs_write_fd(int fd, const void *buf, unsigned int count)
{
    return (int)syscall3(SYS_VFS_WRITE, fd, (long)buf, (long)count);
}

int vfs_create(const char *path, unsigned int perms)
{
    return (int)syscall2(SYS_VFS_CREATE, (long)path, (long)perms);
}

int vfs_mkdir(const char *path, unsigned int perms)
{
    return (int)syscall2(SYS_VFS_MKDIR, (long)path, (long)perms);
}

int vfs_unlink(const char *path)
{
    return (int)syscall1(SYS_VFS_UNLINK, (long)path);
}

int read_nb(int fd, void *buf, size_t count)
{
    return (int)syscall3(SYS_READ_NB, fd, (long)buf, (long)count);
}

int sleep_ms(int ms)
{
    return (int)syscall1(SYS_SLEEP, ms);
}

int console_switch(int console_num)
{
    return (int)syscall1(SYS_CONSOLE_SWITCH, console_num);
}

int console_getcur(void)
{
    return (int)syscall0(SYS_CONSOLE_GETCUR);
}

int console_clear(int console_num)
{
    return (int)syscall1(SYS_CONSOLE_CLEAR, console_num);
}

int console_setcursor(int x, int y)
{
    return (int)syscall2(SYS_CONSOLE_SETCURSOR, x, y);
}

int fb_setcolors(unsigned int fg, unsigned int bg)
{
    return (int)syscall2(SYS_FB_SETCOLORS, (long)fg, (long)bg);
}

int fb_putpixel(unsigned int x, unsigned int y, unsigned int color)
{
    return (int)syscall3(SYS_FB_PUTPIXEL, (long)x, (long)y, (long)color);
}

int fb_getinfo(unsigned int *width, unsigned int *height, unsigned int *bpp,
    unsigned int *font_height, unsigned int *rows, unsigned int *cursor_row,
    unsigned int *font_width, unsigned int *cols)
{
    unsigned int info[8];
    int ret = (int)syscall1(SYS_FB_GETINFO, (long)info);
    if (ret == 0) {
        if (width) *width = info[0];
        if (height) *height = info[1];
        if (bpp) *bpp = info[2];
        if (font_height) *font_height = info[3];
        if (rows) *rows = info[4];
        if (cursor_row) *cursor_row = info[5];
        if (font_width) *font_width = info[6];
        if (cols) *cols = info[7];
    }
    return ret;
}

int fb_clear(void)
{
    return (int)syscall0(SYS_FB_CLEAR);
}
int fb_set_mode(unsigned int width, unsigned int height, unsigned int refresh_rate)
{
    return (int)syscall3(SYS_FB_SET_MODE, (long)width, (long)height, (long)refresh_rate);
}

int fb_getinfo_detailed(unsigned int *width, unsigned int *height, unsigned int *bpp, unsigned int *refresh_rate)
{
    unsigned int info[4];
    int ret = (int)syscall1(SYS_FB_GET_DETAILED_INFO, (long)info);
    if (ret == 0) {
        if (width) *width = info[0];
        if (height) *height = info[1];
        if (bpp) *bpp = info[2];
        if (refresh_rate) *refresh_rate = info[3];
    }
    return ret;
}

int fb_getcaps(unsigned int *words, unsigned int count)
{
    if (!words) return -1;
    return (int)syscall2(SYS_FB_GET_CAPS, (long)words, (long)count);
}
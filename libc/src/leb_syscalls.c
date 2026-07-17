#include <stddef.h>
#include <stdint.h>
#include <lebirun.h>
#include <lebirun/syscall.h>

unsigned int getticks(void)
{
    return (unsigned int)leb_syscall0(LEB_SYSCALL_GETTICKS);
}

int initrd_count(void)
{
    return (int)leb_syscall0(LEB_SYSCALL_INITRD_COUNT);
}

int initrd_stat(int index, char *name, unsigned int *length)
{
    return (int)leb_syscall3(LEB_SYSCALL_INITRD_STAT, index, (long)name,
        (long)length);
}

int initrd_read(int index, void *buf, unsigned int maxlen)
{
    return (int)leb_syscall3(LEB_SYSCALL_INITRD_READ, index, (long)buf,
        (long)maxlen);
}

int vfs_open(const char *path, int flags)
{
    return (int)leb_syscall2(LEB_SYSCALL_VFS_OPEN, (long)path, flags);
}

int vfs_close_fd(int fd)
{
    return (int)leb_syscall1(LEB_SYSCALL_VFS_CLOSE, fd);
}

int vfs_read_fd(int fd, void *buf, unsigned int count)
{
    return (int)leb_syscall3(LEB_SYSCALL_VFS_READ, fd, (long)buf,
        (long)count);
}

int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index)
{
    return (int)leb_syscall4(LEB_SYSCALL_VFS_READDIR, fd, (long)name,
        (long)type, (long)index);
}

int vfs_stat(int fd, uint64_t *size, uint64_t *type)
{
    return (int)leb_syscall3(LEB_SYSCALL_VFS_STAT, fd, (long)size,
        (long)type);
}

int vfs_mounts(void)
{
    return (int)leb_syscall0(LEB_SYSCALL_VFS_MOUNTS);
}

int vfs_write_fd(int fd, const void *buf, unsigned int count)
{
    return (int)leb_syscall3(LEB_SYSCALL_VFS_WRITE, fd, (long)buf,
        (long)count);
}

int vfs_create(const char *path, unsigned int perms)
{
    return (int)leb_syscall2(LEB_SYSCALL_VFS_CREATE, (long)path,
        (long)perms);
}

int vfs_mkdir(const char *path, unsigned int perms)
{
    return (int)leb_syscall2(LEB_SYSCALL_VFS_MKDIR, (long)path,
        (long)perms);
}

int vfs_unlink(const char *path)
{
    return (int)leb_syscall1(LEB_SYSCALL_VFS_UNLINK, (long)path);
}

int read_nb(int fd, void *buf, size_t count)
{
    return (int)leb_syscall3(LEB_SYSCALL_READ_NB, fd, (long)buf,
        (long)count);
}

int sleep_ms(int ms)
{
    return (int)leb_syscall1(LEB_SYSCALL_SLEEP, ms);
}

int console_switch(int console_num)
{
    return (int)leb_syscall1(LEB_SYSCALL_CONSOLE_SWITCH, console_num);
}

int console_getcur(void)
{
    return (int)leb_syscall0(LEB_SYSCALL_CONSOLE_GETCUR);
}

int console_clear(int console_num)
{
    return (int)leb_syscall1(LEB_SYSCALL_CONSOLE_CLEAR, console_num);
}

int console_setcursor(int x, int y)
{
    return (int)leb_syscall2(LEB_SYSCALL_CONSOLE_SETCURSOR, x, y);
}

int console_setid(int console_num)
{
    return (int)leb_syscall1(LEB_SYSCALL_CONSOLE_SETID, console_num);
}

int fb_setcolors(unsigned int fg, unsigned int bg)
{
    return (int)leb_syscall2(LEB_SYSCALL_FB_SETCOLORS, (long)fg,
        (long)bg);
}

int fb_putpixel(unsigned int x, unsigned int y, unsigned int color)
{
    return (int)leb_syscall3(LEB_SYSCALL_FB_PUTPIXEL, (long)x, (long)y,
        (long)color);
}

int fb_getinfo(unsigned int *width, unsigned int *height, unsigned int *bpp,
    unsigned int *font_height, unsigned int *rows, unsigned int *cursor_row,
    unsigned int *font_width, unsigned int *cols)
{
    unsigned int info[8];
    int ret = (int)leb_syscall1(LEB_SYSCALL_FB_GETINFO, (long)info);
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
    return (int)leb_syscall0(LEB_SYSCALL_FB_CLEAR);
}
int fb_set_mode(unsigned int width, unsigned int height, unsigned int refresh_rate)
{
    return (int)leb_syscall3(LEB_SYSCALL_FB_SET_MODE, (long)width,
        (long)height, (long)refresh_rate);
}

int fb_getinfo_detailed(unsigned int *width, unsigned int *height, unsigned int *bpp, unsigned int *refresh_rate)
{
    unsigned int info[4];
    int ret = (int)leb_syscall1(LEB_SYSCALL_FB_GET_DETAILED_INFO,
        (long)info);
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
    return (int)leb_syscall2(LEB_SYSCALL_FB_GET_CAPS, (long)words,
        (long)count);
}

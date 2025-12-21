#include "syscall_defs.h"

static int sys_fb_putpixel(int x, const char *y_ptr, int color) {
    uint32_t y = (uint32_t)(uintptr_t)y_ptr;
    fb_putpixel((uint32_t)x, y, (uint32_t)color);
    return 0;
}

static int sys_fb_setcolors(int fg, const char *bg_ptr, int unused) {
    (void)unused;
    uint32_t bg = (uint32_t)(uintptr_t)bg_ptr;
    fb_set_colors((uint32_t)fg, bg);
    return 0;
}

static int sys_fb_getinfo(int info_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint32_t info_addr = (uint32_t)info_ptr;
    if (info_addr >= 0xC0000000 || info_addr < 0x1000) return -1;

    framebuffer_t *fb = fb_get();
    uint32_t *info = (uint32_t *)info_addr;
    info[0] = fb->width;
    info[1] = fb->height;
    info[2] = (uint32_t)fb->bpp;
    info[3] = fb->font ? fb->font->height : 16;
    info[4] = fb->rows;
    info[5] = fb->cursor_y;
    return 0;
}

static int sys_fb_clear(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    fb_clear();
    return 0;
}

void syscalls_fb_init(void) {
    syscall_table[SYSCALL_FB_PUTPIXEL] = sys_fb_putpixel;
    syscall_table[SYSCALL_FB_SETCOLORS] = sys_fb_setcolors;
    syscall_table[SYSCALL_FB_GETINFO] = sys_fb_getinfo;
    syscall_table[SYSCALL_FB_CLEAR] = sys_fb_clear;
}

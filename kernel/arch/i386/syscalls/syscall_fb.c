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
    if (info_addr >= 0xC0000000 || info_addr < 0x1000) return -EFAULT;

    framebuffer_t *fb = fb_get();
    uint32_t *info = (uint32_t *)info_addr;
    info[0] = fb->width;
    info[1] = fb->height;
    info[2] = (uint32_t)fb->bpp;
    info[3] = fb->font ? fb->font->height : 16;
    info[4] = fb->rows;
    info[5] = fb->cursor_y;
    info[6] = fb->font ? fb->font->width : 8;
    info[7] = fb->cols;
    return 0;
}

static int sys_fb_clear(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    fb_clear();
    return 0;
}

static int sys_fb_set_mode(int width, const char *height_ptr, int refresh_rate) {
    uint32_t height = (uint32_t)(uintptr_t)height_ptr;
    
    if (width <= 0 || height <= 0) {
        return -EINVAL;
    }
    
    int result = fb_set_mode((uint32_t)width, height, (uint32_t)refresh_rate);
    return result;
}

static int sys_fb_get_detailed_info(int info_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint32_t info_addr = (uint32_t)info_ptr;
    
    if (info_addr >= 0xC0000000 || info_addr < 0x1000) {
        return -EFAULT;
    }
    
    uint32_t *info = (uint32_t *)info_addr;
    uint32_t width, height, bpp, refresh_rate;
    
    int result = fb_get_info(&width, &height, &bpp, &refresh_rate);
    if (result != 0) {
        return result;
    }
    
    info[0] = width;
    info[1] = height;
    info[2] = bpp;
    info[3] = refresh_rate;
    
    return 0;
}

static int sys_fb_get_caps(int info_ptr, const char *words_ptr, int unused2) {
    (void)unused2;
    uint32_t info_addr = (uint32_t)info_ptr;
    uint32_t words = (uint32_t)(uintptr_t)words_ptr;
    uint32_t *out;
    uint32_t buffer[1056];
    uint32_t i;
    uint32_t copy_count;
    int ret;

    if (info_addr >= 0xC0000000 || info_addr < 0x1000) {
        return -EFAULT;
    }
    if (words < 16) {
        return -EINVAL;
    }

    out = (uint32_t *)info_addr;
    copy_count = words;
    if (copy_count > 1056) {
        copy_count = 1056;
    }

    ret = fb_get_caps(buffer, copy_count);
    if (ret != 0) {
        return ret;
    }

    for (i = 0; i < copy_count; i++) {
        out[i] = buffer[i];
    }
    return 0;
}

void syscalls_fb_init(void) {
    syscall_table[SYSCALL_FB_PUTPIXEL] = sys_fb_putpixel;
    syscall_table[SYSCALL_FB_SETCOLORS] = sys_fb_setcolors;
    syscall_table[SYSCALL_FB_GETINFO] = sys_fb_getinfo;
    syscall_table[SYSCALL_FB_CLEAR] = sys_fb_clear;
    syscall_table[SYSCALL_FB_SET_MODE] = sys_fb_set_mode;
    syscall_table[SYSCALL_FB_GET_DETAILED_INFO] = sys_fb_get_detailed_info;
    syscall_table[SYSCALL_FB_GET_CAPS] = sys_fb_get_caps;
}

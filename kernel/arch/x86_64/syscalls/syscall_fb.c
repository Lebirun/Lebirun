#include "syscall_defs.h"

static int sys_fb_putpixel(int x, const char *y_ptr, int color) {
    uint64_t y = (uint64_t)(uintptr_t)y_ptr;
    fb_putpixel((uint64_t)x, y, (uint64_t)color);
    return 0;
}

static int sys_fb_setcolors(int fg, const char *bg_ptr, int unused) {
    (void)unused;
    uint64_t bg = (uint64_t)(uintptr_t)bg_ptr;
    fb_set_colors((uint64_t)fg, bg);
    return 0;
}

static int sys_fb_getinfo(int info_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint64_t info_addr = (uint64_t)(uint32_t)info_ptr;
    if (info_addr >= KERNEL_VMA || info_addr < 0x1000) return -EFAULT;

    framebuffer_t *fb = fb_get();
    uint32_t *info = (uint32_t *)info_addr;
    info[0] = (uint32_t)fb->width;
    info[1] = (uint32_t)fb->height;
    info[2] = (uint32_t)fb->bpp;
    info[3] = fb->font ? (uint32_t)fb->font->height : 16;
    info[4] = (uint32_t)fb->rows;
    info[5] = (uint32_t)fb->cursor_y;
    info[6] = fb->font ? (uint32_t)fb->font->width : 8;
    info[7] = (uint32_t)fb->cols;
    return 0;
}

static int sys_fb_clear(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    fb_clear();
    return 0;
}

static int sys_fb_set_mode(int width, const char *height_ptr, int refresh_rate) {
    uint64_t height;
    int result;
    framebuffer_t *fb;
    int ci;

    height = (uint64_t)(uintptr_t)height_ptr;

    if (width <= 0 || height <= 0) {
        return -EINVAL;
    }

    result = fb_set_mode((uint64_t)width, height, (uint64_t)refresh_rate);
    if (result == 0) {
        fb = fb_get();
        for (ci = 0; ci < tty_count; ci++) {
            if (fb) {
                tty_winsize[ci].ws_col = fb->cols;
                tty_winsize[ci].ws_row = fb->rows;
                tty_winsize[ci].ws_xpixel = fb->width;
                tty_winsize[ci].ws_ypixel = fb->height;
            }
            if (tty_pgrp[ci] > 0) {
                pid_t pids[64];
                int npids;
                int si;

                npids = collect_pids_in_pgrp(tty_pgrp[ci], pids, 64);
                for (si = 0; si < npids; si++) {
                    task_t *t = task_find(pids[si]);
                    if (t) deliver_signal_to_task(t, 28);
                }
            }
        }
    }
    return result;
}

static int sys_fb_get_detailed_info(int info_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    uint64_t info_addr = (uint64_t)(uint32_t)info_ptr;
    
    if (info_addr >= KERNEL_VMA || info_addr < 0x1000) {
        return -EFAULT;
    }
    
    uint32_t *info = (uint32_t *)info_addr;
    uint64_t width, height, bpp, refresh_rate;
    
    int result = fb_get_info(&width, &height, &bpp, &refresh_rate);
    if (result != 0) {
        return result;
    }
    
    info[0] = (uint32_t)width;
    info[1] = (uint32_t)height;
    info[2] = (uint32_t)bpp;
    info[3] = (uint32_t)refresh_rate;
    
    return 0;
}

static int sys_fb_get_caps(int info_ptr, const char *words_ptr, int unused2) {
    uint64_t info_addr;
    uint64_t words;
    uint32_t *out;
    uint64_t buffer[16];
    uint64_t i;
    uint64_t copy_count;
    int ret;

    (void)unused2;
    info_addr = (uint64_t)(uint32_t)info_ptr;
    words = (uint64_t)(uintptr_t)words_ptr;

    if (info_addr >= KERNEL_VMA || info_addr < 0x1000) {
        return -EFAULT;
    }
    if (words < 16) {
        return -EINVAL;
    }

    out = (uint32_t *)info_addr;
    copy_count = words;
    if (copy_count > 16) {
        copy_count = 16;
    }

    ret = fb_get_caps(buffer, copy_count);
    if (ret != 0) {
        return ret;
    }

    for (i = 0; i < copy_count; i++) {
        out[i] = (uint32_t)buffer[i];
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

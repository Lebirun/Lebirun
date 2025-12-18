#include <graphics.h>
#include <unistd.h>

void gfx_draw_circle(int cx, int cy, int radius) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return;

    int max_rad_top = cy;
    int max_rad_bottom = (int)fb_height - 1 - cy;
    int max_rad_left = cx;
    int max_rad_right = (int)width - 1 - cx;
    int max_allowed = max_rad_top;
    if (max_rad_bottom < max_allowed) max_allowed = max_rad_bottom;
    if (max_rad_left < max_allowed) max_allowed = max_rad_left;
    if (max_rad_right < max_allowed) max_allowed = max_rad_right;

    if (max_allowed < 8) {
        cy = (int)fb_height / 2;
        max_rad_top = cy;
        max_rad_bottom = (int)fb_height - 1 - cy;
        max_allowed = max_rad_top;
        if (max_rad_bottom < max_allowed) max_allowed = max_rad_bottom;
        if (max_rad_left < max_allowed) max_allowed = max_rad_left;
        if (max_rad_right < max_allowed) max_allowed = max_rad_right;
    }

    if (radius > max_allowed) {
        radius = max_allowed;
        if (radius < 2) radius = 2;
    }

    if (cy < radius) cy = radius;
    if (cy > (int)fb_height - 1 - radius) cy = (int)fb_height - 1 - radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy <= radius*radius) {
                int px = cx + dx;
                int py = cy + dy;
                if (px >= 0 && px < (int)width && py >= 0 && py < (int)fb_height) {
                    unsigned int r = (unsigned int)(128 + dx * 127 / (radius > 1 ? radius : 1));
                    unsigned int g = (unsigned int)(128 + dy * 127 / (radius > 1 ? radius : 1));
                    unsigned int b = 200;
                    unsigned int color = (r << 16) | (g << 8) | b;
                    fb_putpixel(px, py, color);
                }
            }
        }
    }
}

int gfx_compute_circle_center_below(unsigned int sq_bottom_pixel, int gap, int radius) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return -1;

    long cy = (long)sq_bottom_pixel + gap + radius; 
    if (cy > (long)fb_height - 1 - radius) cy = (long)fb_height - 1 - radius;
    if (cy < radius) cy = radius;
    return (int)cy;
}

int gfx_draw_circle_below_squares(int cx, unsigned int sq_bottom_pixel, int gap, int radius) {
    int cy = gfx_compute_circle_center_below(sq_bottom_pixel, gap, radius);
    if (cy < 0) return cy;
    gfx_draw_circle(cx, cy, radius);
    return cy;
}

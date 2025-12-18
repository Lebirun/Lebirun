#include <graphics.h>
#include <unistd.h>
#include <stdio.h>

void gfx_advance_cursor_past_pixels(unsigned int top_pixel, unsigned int bottom_pixel) {
    unsigned int width, height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &height, &bpp, &font_h, &rows, &cursor_row) != 0) return;
    if (bottom_pixel < top_pixel) bottom_pixel = top_pixel;
    unsigned int start_pixel = cursor_row * font_h;
    if (bottom_pixel <= start_pixel) return;
    unsigned int pixels_used = bottom_pixel - start_pixel;
    unsigned int rows_used = (pixels_used + font_h - 1) / font_h + 1;
    if (cursor_row + rows_used >= rows) {
        if (rows > cursor_row) rows_used = rows - cursor_row;
        else rows_used = 0;
    }
    if (isatty(1)) {
        for (unsigned int i = 0; i < rows_used; i++) putchar('\n');
    }
}

int gfx_compute_squares_x_start(unsigned int count, unsigned int sq_size, unsigned int spacing, unsigned int x_start, int sq_x_anchor, int sq_x_offset) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return -1;
    unsigned int total_sq_width = count * sq_size + (count > 0 ? (count - 1) * spacing : 0);
    unsigned int computed_x_start;
    switch (sq_x_anchor) {
        case GFX_ALIGN_LEFT:
            computed_x_start = (unsigned int)((int)x_start + sq_x_offset);
            break;
        case GFX_ALIGN_RIGHT:
            if (total_sq_width >= width) computed_x_start = 0;
            else computed_x_start = (unsigned int)((int)width - (int)total_sq_width - (int)x_start + sq_x_offset);
            break;
        case GFX_ALIGN_CENTER:
        default:
            if (total_sq_width >= width) computed_x_start = 0;
            else computed_x_start = (unsigned int)(((int)width - (int)total_sq_width) / 2 + sq_x_offset);
            break;
    }
    if (computed_x_start + total_sq_width > width) {
        if (total_sq_width > width) computed_x_start = 0;
        else computed_x_start = width - total_sq_width;
    }

    return (int)computed_x_start;
}

void gfx_draw_bar_region(unsigned int y, unsigned int height, unsigned int xstart, unsigned int region_w) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return;
    for (unsigned int py = y; py < y + height && py < fb_height; py++) {
        for (unsigned int x = xstart; x < xstart + region_w && x < width; x++) {
            unsigned int r, g, b;
            unsigned int segment = (x * 6) / width;
            unsigned int pos = ((x * 6) % width) * 255 / width;
            switch (segment) {
                case 0: r = 255; g = pos; b = 0; break;
                case 1: r = 255 - pos; g = 255; b = 0; break;
                case 2: r = 0; g = 255; b = pos; break;
                case 3: r = 0; g = 255 - pos; b = 255; break;
                case 4: r = pos; g = 0; b = 255; break;
                default: r = 255; g = 0; b = 255 - pos; break;
            }
            unsigned int color = (r << 16) | (g << 8) | b;
            fb_putpixel((int)x, (int)py, color);
        }
    }
}

int gfx_compute_circle_center_x(int radius, int anchor, int offset, unsigned int x_start, unsigned int sq_size) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return -1;
    int anchor_x;
    switch (anchor) {
        case GFX_ALIGN_LEFT:
            anchor_x = (int)x_start + (int)sq_size / 2;
            break;
        case GFX_ALIGN_RIGHT:
            anchor_x = (int)width - 1 - radius;
            break;
        case GFX_ALIGN_CENTER:
        default:
            anchor_x = (int)width / 2;
            break;
    }
    int cx = anchor_x + offset;
    if (cx < radius) cx = radius;
    if (cx > (int)width - 1 - radius) cx = (int)width - 1 - radius;
    return cx;
}

void gfx_demo(void) {
    struct gfx_demo_opts opts;
    unsigned int default_colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF, 0xFF8000 };
    opts.bar_height = 40;
    opts.sq_size = 60;
    opts.sq_gap = 10;
    opts.sq_spacing = 10;
    opts.x_start = 40;    opts.sq_x_offset = 0;
    opts.sq_x_anchor = GFX_ALIGN_LEFT;    opts.colors = default_colors;
    opts.colors_count = sizeof(default_colors)/sizeof(default_colors[0]);
    opts.circle_radius = 80;
    opts.circle_gap = 20;
    opts.circle_x_offset = -40;
    opts.circle_x_anchor = GFX_ALIGN_RIGHT;
    gfx_demo_with_options(&opts);
}

void gfx_demo_with_options(const struct gfx_demo_opts *opts) {
    unsigned int width, height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &height, &bpp, &font_h, &rows, &cursor_row) != 0) return;

    unsigned int bar_y = cursor_row * font_h + 4;
    gfx_draw_bar(bar_y, opts->bar_height);

    unsigned int sq_y = bar_y + opts->bar_height + opts->sq_gap;
    int computed_x_start = gfx_compute_squares_x_start(opts->colors_count, opts->sq_size, opts->sq_spacing, opts->x_start, opts->sq_x_anchor, opts->sq_x_offset);
    if (computed_x_start < 0) return;
    gfx_draw_squares(opts->sq_size, sq_y, opts->colors, opts->colors_count, (unsigned int)computed_x_start, opts->sq_spacing);

    unsigned int sq_bottom = sq_y + opts->sq_size;
    int cy = gfx_compute_circle_center_below(sq_bottom, (int)opts->circle_gap, opts->circle_radius);
    int cx = gfx_compute_circle_center_x(opts->circle_radius, opts->circle_x_anchor, opts->circle_x_offset, opts->x_start, opts->sq_size);
    if (cx < 0) return;
    gfx_draw_circle(cx, cy, opts->circle_radius);

    unsigned int bottom_pixel = (unsigned int)(cy + opts->circle_radius);
    gfx_advance_cursor_past_pixels(bar_y, bottom_pixel);
}

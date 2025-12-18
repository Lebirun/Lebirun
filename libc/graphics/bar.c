#include <graphics.h>
#include <unistd.h>

void gfx_draw_bar(unsigned int y, unsigned int height) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return;
    for (unsigned int py = y; py < y + height && py < fb_height; py++) {
        for (unsigned int x = 0; x < width; x++) {
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

void gfx_draw_bar_at_cursor(unsigned int height) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return;
    unsigned int y = cursor_row * font_h + 4;
    gfx_draw_bar(y, height);
}

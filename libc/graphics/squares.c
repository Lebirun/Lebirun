#include <graphics.h>
#include <unistd.h>

void gfx_draw_squares(unsigned int sq_size, unsigned int sq_y, const unsigned int *colors, unsigned int count, unsigned int x_start, unsigned int spacing) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return;
    for (unsigned int i = 0; i < count; i++) {
        unsigned int sq_x = x_start + i * (sq_size + spacing);
        if (sq_x + sq_size >= width) break;
        for (unsigned int y = sq_y; y < sq_y + sq_size && y < fb_height; y++) {
            for (unsigned int x = sq_x; x < sq_x + sq_size; x++) {
                fb_putpixel((int)x, (int)y, colors[i]);
            }
        }
    }
}

void gfx_draw_squares_at_cursor(unsigned int sq_size, unsigned int gap_above, const unsigned int *colors, unsigned int count, unsigned int x_start, unsigned int spacing) {
    unsigned int width, fb_height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &fb_height, &bpp, &font_h, &rows, &cursor_row) != 0) return;
    unsigned int sq_y = cursor_row * font_h + gap_above;
    gfx_draw_squares(sq_size, sq_y, colors, count, x_start, spacing);
}

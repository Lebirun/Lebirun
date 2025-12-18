#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gfx_draw_bar(unsigned int y, unsigned int height);
void gfx_draw_bar_at_cursor(unsigned int height);

void gfx_draw_squares(unsigned int sq_size, unsigned int sq_y, const unsigned int *colors, unsigned int count, unsigned int x_start, unsigned int spacing);
void gfx_draw_squares_at_cursor(unsigned int sq_size, unsigned int gap_above, const unsigned int *colors, unsigned int count, unsigned int x_start, unsigned int spacing);

void gfx_draw_circle(int cx, int cy, int radius);
int  gfx_compute_circle_center_below(unsigned int sq_bottom_pixel, int gap, int radius);
int  gfx_draw_circle_below_squares(int cx, unsigned int sq_bottom_pixel, int gap, int radius);

void gfx_advance_cursor_past_pixels(unsigned int top_pixel, unsigned int bottom_pixel);

#define GFX_ALIGN_LEFT   0
#define GFX_ALIGN_CENTER 1
#define GFX_ALIGN_RIGHT  2

struct gfx_demo_opts {
    unsigned int bar_height;     
    unsigned int sq_size;        
    unsigned int sq_gap;     
    unsigned int sq_spacing; 
    unsigned int x_start;      
    int sq_x_offset;         
    int sq_x_anchor;          
    const unsigned int *colors; 
    unsigned int colors_count;
    int circle_radius;         
    int circle_gap;             
    int circle_x_offset;  
    int circle_x_anchor;      
};

void gfx_demo(void);
void gfx_demo_with_options(const struct gfx_demo_opts *opts);

int  gfx_compute_squares_x_start(unsigned int count, unsigned int sq_size, unsigned int spacing, unsigned int x_start, int sq_x_anchor, int sq_x_offset);
void gfx_draw_bar_region(unsigned int y, unsigned int height, unsigned int xstart, unsigned int region_w);
int  gfx_compute_circle_center_x(int radius, int anchor, int offset, unsigned int x_start, unsigned int sq_size);

#ifdef __cplusplus
}
#endif

#endif

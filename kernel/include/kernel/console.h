#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NUM_CONSOLES 9
#define CONSOLE_BUFFER_ROWS 256
#define CONSOLE_BUFFER_COLS 512

bool console_is_initialized(void);

typedef struct {
    char buffer[CONSOLE_BUFFER_ROWS][CONSOLE_BUFFER_COLS];
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t scroll_offset;

    int esc_state;
    char esc_buf[32];
    int esc_len;
} console_t;

void console_init(void);
void console_switch(int console_num);
int console_get_current(void);
void console_putchar(char c);
void console_putchar_to(int console_num, char c);
void console_write(const char *data, size_t size);
void console_write_to(int console_num, const char *data, size_t size);
void console_write_to_fb_only(int console_num, const char *data, size_t size);
void console_writestring(const char *data);
void console_clear(int console_num);
void console_setcursor(int console_num, int x, int y);
int console_getcursor(int console_num, int *x, int *y);
void console_redraw_current(void);
void console_clamp_cursors(uint32_t max_cols, uint32_t max_rows);

#endif

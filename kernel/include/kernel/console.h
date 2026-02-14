#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NUM_CONSOLES 4
#define CONSOLE_BUFFER_COLS 160
#define CONSOLE_WRITE_BUFFER_INIT 4096
#define CONSOLE_WRITE_BUFFER_MAX  65536

bool console_is_initialized(void);

typedef struct {
    char (*buffer)[CONSOLE_BUFFER_COLS];
    uint8_t *line_wrapped;
    uint32_t buffer_rows;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t scroll_offset;

    int esc_state;
    char esc_buf[32];
    int esc_len;
    uint8_t ansi_fg;
    uint8_t ansi_bg;
    uint8_t ansi_bold;
    uint8_t ansi_reverse;

    uint32_t scroll_top;
    uint32_t scroll_bottom;
    uint8_t cursor_visible;

    uint32_t saved_cursor_x;
    uint32_t saved_cursor_y;

    char *write_buffer;
    uint32_t write_buffer_size;
    volatile uint32_t write_head;
    volatile uint32_t write_tail;
    volatile uint32_t dirty;
    int allocated;
} console_t;

void console_init(void);
void console_switch(int console_num);
void console_switch_via_interrupt(int console_num);
void console_switch_tty(int tty_num);
void console_process_pending(void);
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
void console_tick_redraw(void);
void console_clamp_cursors(uint32_t max_cols, uint32_t max_rows);
void console_rewrap_all(uint32_t old_cols, uint32_t new_cols, uint32_t new_rows);
void console_writer_init(void);
void console_writer_flush(void);
void console_tick(void);

#endif

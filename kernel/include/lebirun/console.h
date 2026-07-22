#ifndef _LEBIRUN_CONSOLE_H
#define _LEBIRUN_CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NUM_CONSOLES 12
#define CONSOLE_BUFFER_COLS 160
#define CONSOLE_INACTIVE_INITIAL_ROWS 4
#define CONSOLE_WRITE_BUFFER_INIT 512
#define CONSOLE_WRITE_BUFFER_MAX  65536

bool console_is_initialized(void);

typedef struct {
    char (*buffer)[CONSOLE_BUFFER_COLS];
    uint8_t (*color_buffer)[CONSOLE_BUFFER_COLS];
    uint8_t *line_wrapped;
    uint64_t buffer_rows;
    uint64_t cursor_x;
    uint64_t cursor_y;
    uint64_t scroll_offset;

    int esc_state;
    char esc_buf[32];
    int esc_len;
    uint8_t ansi_fg;
    uint8_t ansi_bg;
    uint8_t ansi_bold;
    uint8_t ansi_reverse;

    uint64_t scroll_top;
    uint64_t scroll_bottom;
    uint8_t cursor_visible;

    uint64_t saved_cursor_x;
    uint64_t saved_cursor_y;

    char *write_buffer;
    uint8_t *write_flags;
    uint64_t write_buffer_size;
    volatile uint64_t write_head;
    volatile uint64_t write_tail;
    volatile uint64_t dirty;
    int allocated;

    int alt_screen_active;
    volatile int alt_screen_pending;
    char (*alt_saved_buffer)[CONSOLE_BUFFER_COLS];
    uint8_t (*alt_saved_color)[CONSOLE_BUFFER_COLS];
    uint8_t *alt_saved_wrapped;
    uint64_t alt_saved_rows;
    uint64_t alt_saved_cx;
    uint64_t alt_saved_cy;
    uint64_t alt_saved_scroll;

    int graphics_mode;
    int graphics_owner_pid;
} console_t;

void console_init(void);
void console_reinit(void);
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
void console_force_redraw(void);
void console_tick_redraw(void);
void console_clamp_cursors(uint64_t max_cols, uint64_t max_rows);
void console_rewrap_all(uint64_t old_cols, uint64_t new_cols, uint64_t new_rows);
void console_writer_init(void);
void console_writer_flush(void);
void console_tick(void);
int console_alloc(int n);
int console_alt_screen_active(int n);
void console_reclaim_unused(void);
void console_memory_stats(uint64_t *buffers, uint64_t *bytes);
int console_get_cell(int console_num, uint64_t x, uint64_t y, char *ch, uint8_t *attr);
int console_set_graphics_mode(int console_num, int enabled, int owner_pid);
int console_get_graphics_mode(int console_num);
void console_release_graphics_owner(int owner_pid);

#endif

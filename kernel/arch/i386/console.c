#include <kernel/console.h>
#include <kernel/framebuffer.h>
#include <kernel/tty.h>
#include <kernel/common.h>
#include <string.h>
#include <stdint.h>

static console_t consoles[NUM_CONSOLES];
static int current_console = 0;
static int console_initialized = 0;

void console_init(void) {
    for (int i = 0; i < NUM_CONSOLES; i++) {
        for (int row = 0; row < CONSOLE_BUFFER_ROWS; row++) {
            for (int col = 0; col < CONSOLE_BUFFER_COLS; col++) {
                consoles[i].buffer[row][col] = ' ';
            }
        }
        consoles[i].cursor_x = 0;
        consoles[i].cursor_y = 0;
        consoles[i].scroll_offset = 0;
    }
    current_console = 0;
    console_initialized = 1;
}

static void console_redraw(void) {
    framebuffer_t *fb = fb_get();
    if (!fb || !fb->font) return;

    uint32_t rows = fb->rows;
    uint32_t cols = fb->cols;
    console_t *con = &consoles[current_console];

    uint32_t *pixel = fb->addr;
    for (uint32_t y = 0; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            pixel[x] = fb->bg_color;
        }
        pixel = (uint32_t *)((uint8_t *)pixel + fb->pitch);
    }

    for (uint32_t row = 0; row < rows && row < CONSOLE_BUFFER_ROWS; row++) {
        for (uint32_t col = 0; col < cols && col < CONSOLE_BUFFER_COLS; col++) {
            char c = con->buffer[row][col];
            if (c >= 32) {
                fb_putchar(c, col, row);
            }
        }
    }

    fb->cursor_x = con->cursor_x;
    fb->cursor_y = con->cursor_y;
    fb_update_cursor();
}

void console_switch(int console_num) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;
    if (!console_initialized) return;
    
    framebuffer_t *fb = fb_get();
    if (fb) {
        consoles[current_console].cursor_x = fb->cursor_x;
        consoles[current_console].cursor_y = fb->cursor_y;
    }
    
    current_console = console_num;
    
    if (fb) {
        fb->cursor_x = consoles[current_console].cursor_x;
        fb->cursor_y = consoles[current_console].cursor_y;
    }
    
    console_redraw();
}

int console_get_current(void) {
    return current_console;
}

static void console_scroll(console_t *con) {
    for (int row = 0; row < CONSOLE_BUFFER_ROWS - 1; row++) {
        for (int col = 0; col < CONSOLE_BUFFER_COLS; col++) {
            con->buffer[row][col] = con->buffer[row + 1][col];
        }
    }
    for (int col = 0; col < CONSOLE_BUFFER_COLS; col++) {
        con->buffer[CONSOLE_BUFFER_ROWS - 1][col] = ' ';
    }
}

void console_putchar(char c) {
    console_putchar_to(current_console, c);
}

void console_putchar_to(int console_num, char c) {
    if (!console_initialized) {
        terminal_putchar(c);
        return;
    }
    
    if (console_num < 0 || console_num >= NUM_CONSOLES) {
        console_num = current_console;
    }
    
    if (console_num == 0) {
        serial_putchar(c);
    }
    
    console_t *con = &consoles[console_num];
    framebuffer_t *fb = fb_get();
    uint32_t rows = fb ? fb->rows : 25;
    uint32_t cols = fb ? fb->cols : 80;
    
    int is_active = (console_num == current_console);

    if (c == '\n') {
        con->cursor_x = 0;
        con->cursor_y++;
        if (con->cursor_y >= rows) {
            console_scroll(con);
            con->cursor_y = rows - 1;
            if (is_active && fb) {
                fb_scroll();
                fb->cursor_x = con->cursor_x;
                fb->cursor_y = con->cursor_y;
            }
        }
        if (is_active && fb) {
            fb->cursor_x = con->cursor_x;
            fb->cursor_y = con->cursor_y;
            fb_update_cursor();
        }
        return;
    }

    if (c == '\r') {
        con->cursor_x = 0;
        if (is_active && fb) {
            fb->cursor_x = 0;
            fb_update_cursor();
        }
        return;
    }

    if (c == '\b') {
        if (con->cursor_x > 0) {
            con->cursor_x--;
            con->buffer[con->cursor_y][con->cursor_x] = ' ';
            if (is_active && fb) {
                fb->cursor_x = con->cursor_x;
                fb_putchar(' ', con->cursor_x, con->cursor_y);
                fb_update_cursor();
            }
        }
        return;
    }

    if (c == '\t') {
        uint32_t tab_stop = 8 - (con->cursor_x % 8);
        for (uint32_t i = 0; i < tab_stop; i++) {
            console_putchar_to(console_num, ' ');
        }
        return;
    }

    if (con->cursor_y < CONSOLE_BUFFER_ROWS && con->cursor_x < CONSOLE_BUFFER_COLS) {
        con->buffer[con->cursor_y][con->cursor_x] = c;
    }

    if (is_active && fb) {
        fb_putchar(c, con->cursor_x, con->cursor_y);
    }

    con->cursor_x++;
    if (con->cursor_x >= cols) {
        con->cursor_x = 0;
        con->cursor_y++;
        if (con->cursor_y >= rows) {
            console_scroll(con);
            con->cursor_y = rows - 1;
            if (is_active && fb) {
                fb_scroll();
            }
        }
    }

    if (is_active && fb) {
        fb->cursor_x = con->cursor_x;
        fb->cursor_y = con->cursor_y;
        fb_update_cursor();
    }
}

void console_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        console_putchar(data[i]);
    }
}

void console_write_to(int console_num, const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        console_putchar_to(console_num, data[i]);
    }
}

void console_writestring(const char *data) {
    while (*data) {
        console_putchar(*data++);
    }
}

void console_clear(int console_num) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;
    
    console_t *con = &consoles[console_num];
    for (int row = 0; row < CONSOLE_BUFFER_ROWS; row++) {
        for (int col = 0; col < CONSOLE_BUFFER_COLS; col++) {
            con->buffer[row][col] = ' ';
        }
    }
    con->cursor_x = 0;
    con->cursor_y = 0;
    
    if (console_num == current_console) {
        framebuffer_t *fb = fb_get();
        if (fb) {
            fb_clear();
        }
    }
}

bool console_is_initialized(void) {
    return console_initialized;
}

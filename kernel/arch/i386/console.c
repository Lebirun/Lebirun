#include <kernel/console.h>
#include <kernel/framebuffer.h>
#include <kernel/tty.h>
#include <kernel/common.h>
#include <kernel/spinlock.h>
#include <kernel/vring.h>
#include <kernel/task.h>
#include <string.h>
#include <stdint.h>

static console_t consoles[NUM_CONSOLES];
static int current_console = 0;
static int console_initialized = 0;
static int console_batch = 0;
static spinlock_t console_lock;

static inline uint32_t console_irqsave(void) {
    uint32_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void console_irqrestore(uint32_t flags) {
    asm volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static inline int console_interrupts_enabled(void) {
    uint32_t flags;
    asm volatile("pushf; pop %0" : "=r"(flags) : : "memory");
    return (flags & (1u << 9)) != 0;
}

void console_init(void) {
    spinlock_init(&console_lock);
    for (int i = 0; i < NUM_CONSOLES; i++) {
        for (int row = 0; row < CONSOLE_BUFFER_ROWS; row++) {
            for (int col = 0; col < CONSOLE_BUFFER_COLS; col++) {
                consoles[i].buffer[row][col] = ' ';
            }
        }
        consoles[i].cursor_x = 0;
        consoles[i].cursor_y = 0;
        consoles[i].scroll_offset = 0;
        consoles[i].esc_state = 0;
        consoles[i].esc_len = 0;
        consoles[i].esc_buf[0] = '\0';
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

    fb_clear();

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

    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    
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

    spin_unlock(&console_lock);
    console_irqrestore(flags);
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

static int parse_csi_params(const char *buf, int len, int *params, int max_params) {
    int count = 0;
    int val = 0;
    int has_digit = 0;
    for (int i = 0; i < len && count < max_params; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
            has_digit = 1;
        } else if (buf[i] == ';') {
            params[count++] = has_digit ? val : 1;
            val = 0;
            has_digit = 0;
        }
    }
    if (has_digit && count < max_params) {
        params[count++] = val;
    }
    return count;
}

static void console_handle_csi(int console_num, console_t *con, framebuffer_t *fb, uint32_t rows, uint32_t cols, int is_active) {
    if (con->esc_len == 0) return;

    char cmd = con->esc_buf[con->esc_len - 1];
    
    int param_start = 0;
    int is_private = 0;
    if (con->esc_buf[0] == '?') {
        is_private = 1;
        param_start = 1;
    }
    
    int params[8] = {0};
    int nparams = parse_csi_params(con->esc_buf + param_start, con->esc_len - 1 - param_start, params, 8);

    if (is_private) {
        return;
    }

    switch (cmd) {
    case 'H':
    case 'f': {
        int row = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        int col = (nparams >= 2 && params[1] > 0) ? params[1] - 1 : 0;
        if ((uint32_t)row >= rows) row = rows - 1;
        if ((uint32_t)col >= cols) col = cols - 1;
        con->cursor_x = col;
        con->cursor_y = row;
        if (is_active && fb) {
            fb->cursor_x = col;
            fb->cursor_y = row;
            if (!console_batch) fb_update_cursor();
        }
        break;
    }
    case 'A': {
        int n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if (con->cursor_y >= (uint32_t)n) con->cursor_y -= n;
        else con->cursor_y = 0;
        if (is_active && fb) { fb->cursor_y = con->cursor_y; fb_update_cursor(); }
        break;
    }
    case 'B': {
        int n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_y += n;
        if (con->cursor_y >= rows) con->cursor_y = rows - 1;
        if (is_active && fb) { fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
        break;
    }
    case 'C': {
        int n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x += n;
        if (con->cursor_x >= cols) con->cursor_x = cols - 1;
        if (is_active && fb) { fb->cursor_x = con->cursor_x; if (!console_batch) fb_update_cursor(); }
        break;
    }
    case 'D': {
        int n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if (con->cursor_x >= (uint32_t)n) con->cursor_x -= n;
        else con->cursor_x = 0;
        if (is_active && fb) { fb->cursor_x = con->cursor_x; if (!console_batch) fb_update_cursor(); }
        break;
    }
    case 'J': {
        int mode = (nparams >= 1) ? params[0] : 0;
        if (mode == 2 || mode == 3) {
            console_clear(console_num);
        } else if (mode == 0) {
            for (uint32_t col = con->cursor_x; col < cols && col < CONSOLE_BUFFER_COLS; col++)
                con->buffer[con->cursor_y][col] = ' ';
            for (uint32_t row = con->cursor_y + 1; row < rows && row < CONSOLE_BUFFER_ROWS; row++)
                for (uint32_t col = 0; col < cols && col < CONSOLE_BUFFER_COLS; col++)
                    con->buffer[row][col] = ' ';
            if (is_active && fb) {
                for (uint32_t col = con->cursor_x; col < cols; col++)
                    fb_putchar(' ', col, con->cursor_y);
                for (uint32_t row = con->cursor_y + 1; row < rows; row++)
                    for (uint32_t col = 0; col < cols; col++)
                        fb_putchar(' ', col, row);
            }
        } else if (mode == 1) {
            for (uint32_t row = 0; row < con->cursor_y && row < CONSOLE_BUFFER_ROWS; row++)
                for (uint32_t col = 0; col < cols && col < CONSOLE_BUFFER_COLS; col++)
                    con->buffer[row][col] = ' ';
            for (uint32_t col = 0; col <= con->cursor_x && col < CONSOLE_BUFFER_COLS; col++)
                con->buffer[con->cursor_y][col] = ' ';
            if (is_active && fb) {
                for (uint32_t row = 0; row < con->cursor_y; row++)
                    for (uint32_t col = 0; col < cols; col++)
                        fb_putchar(' ', col, row);
                for (uint32_t col = 0; col <= con->cursor_x; col++)
                    fb_putchar(' ', col, con->cursor_y);
            }
        }
        break;
    }
    case 'K': {
        int mode = (nparams >= 1) ? params[0] : 0;
        if (mode == 0) {
            for (uint32_t col = con->cursor_x; col < cols && col < CONSOLE_BUFFER_COLS; col++) {
                con->buffer[con->cursor_y][col] = ' ';
                if (is_active && fb) fb_putchar(' ', col, con->cursor_y);
            }
        } else if (mode == 1) {
            for (uint32_t col = 0; col <= con->cursor_x && col < CONSOLE_BUFFER_COLS; col++) {
                con->buffer[con->cursor_y][col] = ' ';
                if (is_active && fb) fb_putchar(' ', col, con->cursor_y);
            }
        } else if (mode == 2) {
            for (uint32_t col = 0; col < cols && col < CONSOLE_BUFFER_COLS; col++) {
                con->buffer[con->cursor_y][col] = ' ';
                if (is_active && fb) fb_putchar(' ', col, con->cursor_y);
            }
        }
        break;
    }
    case 'm':
        break;
    case 's':
    case 'u':
    case 'h':
    case 'l':
    case '?':
    default:
        break;
    }
}

static void console_putchar_to_nolock(int console_num, char c) {
    if (!console_initialized) {
        terminal_putchar(c);
        return;
    }
    
    if (console_num < 0 || console_num >= NUM_CONSOLES) {
        console_num = current_console;
    }
    
    if (console_num == 0) {
        if (kprint_is_ready()) {
            kprint_serial_async(&c, 1);
        } else {
            serial_putchar(c);
        }
    }
    
    console_t *con = &consoles[console_num];
    framebuffer_t *fb = fb_get();
    uint32_t rows = fb ? fb->rows : 25;
    uint32_t cols = fb ? fb->cols : 80;
    
    int is_active = (console_num == current_console);

    if (con->esc_state == 1) {
        if (c == '[') {
            con->esc_state = 2;
            con->esc_len = 0;
        } else {
            con->esc_state = 0;
        }
        if (con->esc_state != 0) return;
    }
    
    if (con->esc_state == 2) {
        if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
            if (con->esc_len < (int)(sizeof(con->esc_buf) - 1)) {
                con->esc_buf[con->esc_len++] = c;
            }
            return;
        }
        if (con->esc_len < (int)(sizeof(con->esc_buf) - 1)) {
            con->esc_buf[con->esc_len++] = c;
        }
        con->esc_buf[con->esc_len] = '\0';
        console_handle_csi(console_num, con, fb, rows, cols, is_active);
        con->esc_state = 0;
        con->esc_len = 0;
        return;
    }

    if (c == '\033') {
        con->esc_state = 1;
        con->esc_len = 0;
        return;
    }

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
            if (!console_batch) fb_update_cursor();
        }
        return;
    }

    if (c == '\r') {
        con->cursor_x = 0;
        if (is_active && fb) {
            fb->cursor_x = 0;
            if (!console_batch) fb_update_cursor();
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
                if (!console_batch) fb_update_cursor();
            }
        }
        return;
    }

    if (c == '\t') {
        uint32_t tab_stop = 8 - (con->cursor_x % 8);
        for (uint32_t i = 0; i < tab_stop; i++) {
            console_putchar_to_nolock(console_num, ' ');
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
        if (!console_batch) fb_update_cursor();
    }
}

void console_putchar_to(int console_num, char c) {
    if (!console_initialized) {
        terminal_putchar(c);
        return;
    }

    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    console_putchar_to_nolock(console_num, c);
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        console_putchar(data[i]);
    }
}

static void console_write_internal(int console_num, const char *data, size_t size, int skip_serial_async);

void console_write_to(int console_num, const char *data, size_t size) {
    console_write_internal(console_num, data, size, 0);
}

void console_write_to_fb_only(int console_num, const char *data, size_t size) {
    console_write_internal(console_num, data, size, 1);
}

static void console_write_internal(int console_num, const char *data, size_t size, int skip_serial_async) {
    if (!console_initialized) {
        for (size_t i = 0; i < size; i++) terminal_putchar(data[i]);
        return;
    }

    if (console_num == 0 && kprint_is_ready() && !skip_serial_async) {
        kprint_serial_async(data, size);
    }

    int target_console = console_num;
    if (target_console < 0 || target_console >= NUM_CONSOLES) {
        target_console = current_console;
    }

    int skip_serial = (target_console == 0 && kprint_is_ready());

    int batch_started = 0;
    size_t off = 0;
    while (off < size) {
        size_t chunk = size - off;
        if (chunk > 64) chunk = 64;

        uint32_t flags = console_irqsave();
        spin_lock(&console_lock);
        if (!batch_started) {
            console_batch++;
            batch_started = 1;
        }

        console_t *con = &consoles[target_console];
        framebuffer_t *fb = fb_get();
        uint32_t rows = fb ? fb->rows : 25;
        uint32_t cols = fb ? fb->cols : 80;
        int is_active = (target_console == current_console);

        for (size_t i = 0; i < chunk; i++) {
            char c = data[off + i];
            if (target_console == 0 && !skip_serial_async && !skip_serial && !kprint_is_ready()) {
                serial_putchar(c);
            }

            if (con->esc_state == 1) {
                if (c == '[') {
                    con->esc_state = 2;
                    con->esc_len = 0;
                } else {
                    con->esc_state = 0;
                }
                if (con->esc_state != 0) continue;
            }

            if (con->esc_state == 2) {
                if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
                    if (con->esc_len < (int)(sizeof(con->esc_buf) - 1)) {
                        con->esc_buf[con->esc_len++] = c;
                    }
                    continue;
                }
                if (con->esc_len < (int)(sizeof(con->esc_buf) - 1)) {
                    con->esc_buf[con->esc_len++] = c;
                }
                con->esc_buf[con->esc_len] = '\0';
                console_handle_csi(target_console, con, fb, rows, cols, is_active);
                con->esc_state = 0;
                con->esc_len = 0;
                continue;
            }

            if (c == '\033') {
                con->esc_state = 1;
                con->esc_len = 0;
                continue;
            }

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
                }
                continue;
            }

            if (c == '\r') {
                con->cursor_x = 0;
                if (is_active && fb) {
                    fb->cursor_x = 0;
                }
                continue;
            }

            if (c == '\b') {
                if (con->cursor_x > 0) {
                    con->cursor_x--;
                    con->buffer[con->cursor_y][con->cursor_x] = ' ';
                    if (is_active && fb) {
                        fb->cursor_x = con->cursor_x;
                        fb_putchar(' ', con->cursor_x, con->cursor_y);
                    }
                }
                continue;
            }

            if (c == '\t') {
                uint32_t tab_stop = 8 - (con->cursor_x % 8);
                for (uint32_t t = 0; t < tab_stop; t++) {
                    if (con->cursor_y < CONSOLE_BUFFER_ROWS && con->cursor_x < CONSOLE_BUFFER_COLS) {
                        con->buffer[con->cursor_y][con->cursor_x] = ' ';
                    }
                    if (is_active && fb) {
                        fb_putchar(' ', con->cursor_x, con->cursor_y);
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
                }
                continue;
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
            }
        }

        spin_unlock(&console_lock);
        console_irqrestore(flags);

        off += chunk;
        if (current_task && console_interrupts_enabled() && (off % 1024) == 0) {
            yield();
        }
    }

    if (batch_started) {
        uint32_t flags = console_irqsave();
        spin_lock(&console_lock);
        console_batch--;
        if (console_batch == 0 && console_initialized) {
            framebuffer_t *fb = fb_get();
            if (fb && fb->font && target_console == current_console) {
                fb->cursor_x = consoles[current_console].cursor_x;
                fb->cursor_y = consoles[current_console].cursor_y;
                fb_update_cursor();
            }
        }
        spin_unlock(&console_lock);
        console_irqrestore(flags);
    }
}

void console_writestring(const char *data) {
    while (*data) {
        console_putchar(*data++);
    }
}

void console_clear(int console_num) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;

    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    
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

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_setcursor(int console_num, int x, int y) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;

    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    
    console_t *con = &consoles[console_num];
    framebuffer_t *fb = fb_get();
    uint32_t cols = fb ? fb->cols : 80;
    uint32_t rows = fb ? fb->rows : 25;
    
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= cols) x = cols - 1;
    if ((uint32_t)y >= rows) y = rows - 1;
    
    con->cursor_x = (uint32_t)x;
    con->cursor_y = (uint32_t)y;
    
    if (console_num == current_console && fb) {
        fb->cursor_x = con->cursor_x;
        fb->cursor_y = con->cursor_y;
        fb_update_cursor();
    }

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

int console_getcursor(int console_num, int *x, int *y) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return -1;

    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    
    console_t *con = &consoles[console_num];
    if (x) *x = (int)con->cursor_x;
    if (y) *y = (int)con->cursor_y;

    spin_unlock(&console_lock);
    console_irqrestore(flags);
    return 0;
}

bool console_is_initialized(void) {
    return console_initialized;
}

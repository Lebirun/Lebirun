#include <kernel/console.h>
#include <kernel/framebuffer.h>
#include <kernel/tty.h>
#include <kernel/common.h>
#include <kernel/spinlock.h>
#include <kernel/vring.h>
#include <kernel/task.h>
#include <string.h>
#include <stdint.h>

extern void terminal_putchar(char c);
extern void serial_putchar(char c);
extern framebuffer_t *fb_get(void);
extern void fb_clear(void);
extern void fb_putchar(char c, uint32_t x, uint32_t y);
extern void fb_scroll(void);
extern void fb_update_cursor(void);
extern task_t *current_task;
extern void yield(void);
extern void sleep_ms(uint32_t ms);
extern void wake_task(task_t *task);

static console_t consoles[NUM_CONSOLES];
static int current_console = 0;
static int console_initialized = 0;
static int console_batch = 0;
static spinlock_t console_lock = {0};
static task_t *writer_thread = NULL;
static volatile int writer_thread_running = 0;
static volatile int console_switching = 0;
static volatile int console_switch_in_progress = 0;
static volatile int pending_console_switch = -1;

static volatile int console_redraw_pending = 0;
static char console_redraw_buffer[CONSOLE_BUFFER_ROWS][CONSOLE_BUFFER_COLS];
static uint32_t console_redraw_cursor_x = 0;
static uint32_t console_redraw_cursor_y = 0;
static uint32_t console_redraw_rows = 0;
static uint32_t console_redraw_cols = 0;
static uint32_t console_redraw_visible_rows = 0;
static uint32_t console_redraw_visible_cols = 0;
static uint32_t console_redraw_row = 0;
static int console_redraw_console = 0;

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

static void console_redraw_prepare(int console_num) {
    framebuffer_t *fb = fb_get();
    uint32_t rows;
    uint32_t cols;
    uint32_t row;
    uint32_t col;
    console_t *con;
    uint32_t flags;
    uint32_t visible_rows;
    uint32_t visible_cols;

    if (!fb || !fb->font) {
        console_redraw_pending = 0;
        return;
    }

    rows = fb->rows;
    cols = fb->cols;
    if (rows == 0 || cols == 0) {
        console_redraw_pending = 0;
        return;
    }

    visible_rows = rows < CONSOLE_BUFFER_ROWS ? rows : CONSOLE_BUFFER_ROWS;
    visible_cols = cols < CONSOLE_BUFFER_COLS ? cols : CONSOLE_BUFFER_COLS;

    flags = console_irqsave();
    spin_lock(&console_lock);

    if (console_num < 0 || console_num >= NUM_CONSOLES) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        console_redraw_pending = 0;
        return;
    }

    if (console_redraw_pending) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }

    con = &consoles[console_num];
    console_redraw_cursor_x = con->cursor_x;
    console_redraw_cursor_y = con->cursor_y;

    for (row = 0; row < visible_rows; row++) {
        for (col = 0; col < visible_cols; col++) {
            console_redraw_buffer[row][col] = con->buffer[row][col];
        }
    }

    console_redraw_rows = rows;
    console_redraw_cols = cols;
    console_redraw_visible_rows = visible_rows;
    console_redraw_visible_cols = visible_cols;
    console_redraw_row = 0;
    console_redraw_console = console_num;
    console_redraw_pending = 1;
    console_switch_in_progress = 1;

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

static void console_redraw_step(uint32_t max_rows) {
    framebuffer_t *fb = fb_get();
    uint32_t row;
    uint32_t col;
    char c;
    uint32_t end_row;
    uint32_t cols;
    uint32_t rows_processed;
    uint32_t visible_rows_cached;
    uint32_t visible_cols_cached;
    uint32_t current_row_cached;
    uint32_t fb_rows;

    if (!console_redraw_pending) return;
    if (!fb || !fb->font) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        return;
    }

    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    visible_rows_cached = console_redraw_visible_rows;
    visible_cols_cached = console_redraw_visible_cols;
    current_row_cached = console_redraw_row;
    fb_rows = fb->rows;
    spin_unlock(&console_lock);
    console_irqrestore(flags);

    end_row = current_row_cached + max_rows;
    if (end_row > fb_rows) {
        end_row = fb_rows;
    }

    rows_processed = 0;
    for (row = current_row_cached; row < end_row; row++) {
        cols = (row < visible_rows_cached) ? visible_cols_cached : fb->cols;
        for (col = 0; col < cols; col++) {
            if (row < visible_rows_cached && col < visible_cols_cached) {
                c = console_redraw_buffer[row][col];
            } else {
                c = ' ';
            }

            if ((unsigned char)c >= 32) {
                fb_putchar(c, col, row);
            } else {
                fb_putchar(' ', col, row);
            }
        }
        rows_processed++;
        if (rows_processed >= 4 && current_task && console_interrupts_enabled()) {
            yield();
            rows_processed = 0;
        }
    }

    flags = console_irqsave();
    spin_lock(&console_lock);
    console_redraw_row = end_row;
    if (console_redraw_row >= fb->rows) {
        fb->cursor_x = console_redraw_cursor_x;
        fb->cursor_y = console_redraw_cursor_y;
        fb_update_cursor();
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_tick_redraw(void) {
    if (!console_redraw_pending) return;
    console_redraw_step(2);
}

static void console_redraw_sync(int console_num) {
    uint32_t chunk_rows;
    console_redraw_prepare(console_num);
    if (!console_redraw_pending) return;
    
    if (console_redraw_visible_rows >= 240) {
        chunk_rows = 12;
    } else if (console_redraw_visible_rows >= 200) {
        chunk_rows = 16;
    } else if (console_redraw_visible_rows >= 100) {
        chunk_rows = 32;
    } else if (console_redraw_visible_rows >= 50) {
        chunk_rows = 16;
    } else {
        chunk_rows = 8;
    }
    
    while (console_redraw_pending) {
        console_redraw_step(chunk_rows);
        if (current_task && console_interrupts_enabled()) {
            yield();
        }
    }
}

static void console_clamp_cursors_locked(uint32_t max_cols, uint32_t max_rows) {
    if (max_cols == 0) max_cols = 1;
    if (max_rows == 0) max_rows = 1;
    if (max_cols > CONSOLE_BUFFER_COLS) max_cols = CONSOLE_BUFFER_COLS;
    if (max_rows > CONSOLE_BUFFER_ROWS) max_rows = CONSOLE_BUFFER_ROWS;

    for (int i = 0; i < NUM_CONSOLES; i++) {
        console_t *con = &consoles[i];
        if (con->cursor_x >= max_cols) {
            con->cursor_x = max_cols - 1;
        }
        if (con->cursor_y >= max_rows) {
            con->cursor_y = max_rows - 1;
        }
    }

    if (current_console >= 0 && current_console < NUM_CONSOLES) {
        framebuffer_t *fb = fb_get();
        if (fb) {
            if (fb->cursor_x >= max_cols) {
                fb->cursor_x = max_cols - 1;
            }
            if (fb->cursor_y >= max_rows) {
                fb->cursor_y = max_rows - 1;
            }
        }
    }
}

void console_clamp_cursors(uint32_t max_cols, uint32_t max_rows) {
    uint32_t flags = console_irqsave();
    spin_lock(&console_lock);
    console_clamp_cursors_locked(max_cols, max_rows);
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_init(void) {
    if (console_initialized) return;
    
    for (int i = 0; i < NUM_CONSOLES; i++) {
        console_t *con = &consoles[i];
        for (int row = 0; row < CONSOLE_BUFFER_ROWS; row++) {
            for (int col = 0; col < CONSOLE_BUFFER_COLS; col++) {
                con->buffer[row][col] = ' ';
            }
        }
        con->cursor_x = 0;
        con->cursor_y = 0;
        con->scroll_offset = 0;
        con->esc_state = 0;
        con->esc_len = 0;
        con->write_head = 0;
        con->write_tail = 0;
        con->dirty = 0;
    }
    
    current_console = 0;
    console_switching = 0;
    console_switch_in_progress = 0;
    pending_console_switch = -1;
    console_redraw_pending = 0;
    console_batch = 0;
    
    console_initialized = 1;
}

void console_redraw_current(void) {
    if (!console_initialized) return;
    if (console_switch_in_progress) return;
    if (writer_thread_running) {
        console_redraw_prepare(current_console);
        return;
    }
    console_redraw_sync(current_console);
}

static void console_switch_internal_impl(int console_num, int from_interrupt) {
    framebuffer_t *fb;
    uint32_t rows;
    uint32_t cols;
    uint32_t flags;
    console_t *old_con;
    console_t *new_con;
    int lock_acquired;
    
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;
    if (!console_initialized) return;
    if (console_num == current_console) return;

    if (console_switching) {
        pending_console_switch = console_num;
        return;
    }

    flags = console_irqsave();
    lock_acquired = spin_trylock(&console_lock);
    if (!lock_acquired) {
        console_irqrestore(flags);
        pending_console_switch = console_num;
        return;
    }
    
    console_switching = 1;
    console_switch_in_progress = 1;
    
    fb = fb_get();
    if (fb) {
        consoles[current_console].cursor_x = fb->cursor_x;
        consoles[current_console].cursor_y = fb->cursor_y;
    }
    
    old_con = &consoles[current_console];
    new_con = &consoles[console_num];
    
    old_con->esc_state = 0;
    old_con->esc_len = 0;
    new_con->esc_state = 0;
    new_con->esc_len = 0;
    
    rows = fb ? fb->rows : 25;
    cols = fb ? fb->cols : 80;
    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
    
    console_clamp_cursors_locked(cols, rows);
    
    current_console = console_num;
    
    if (fb) {
        uint32_t new_cx = consoles[current_console].cursor_x;
        uint32_t new_cy = consoles[current_console].cursor_y;
        if (new_cx >= cols) new_cx = cols - 1;
        if (new_cy >= rows) new_cy = rows - 1;
        fb->cursor_x = new_cx;
        fb->cursor_y = new_cy;
        consoles[current_console].cursor_x = new_cx;
        consoles[current_console].cursor_y = new_cy;
    }
    
    spin_unlock(&console_lock);
    console_irqrestore(flags);

    console_redraw_prepare(current_console);
    if (!writer_thread_running && !from_interrupt) {
        console_redraw_sync(current_console);
    }
}

static void console_switch_internal(int console_num) {
    console_switch_internal_impl(console_num, 0);
}

void console_switch(int console_num) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;
    if (!console_initialized) return;
    if (console_num == current_console) return;

    if (!console_interrupts_enabled()) {
        if (console_switching) {
            pending_console_switch = console_num;
            return;
        }
        pending_console_switch = console_num;
        if (writer_thread) {
            wake_task(writer_thread);
        }
        return;
    }
    
    console_switch_internal(console_num);
}

void console_switch_via_interrupt(int console_num) {
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;
    if (!console_initialized) return;
    if (console_num == current_console) return;

    console_switch_internal_impl(console_num, 1);
}

void console_process_pending(void) {
    int pending;
    uint32_t flags;
    
    if (!console_initialized) return;
    if (!console_interrupts_enabled()) return;
    
    static volatile int in_processing = 0;
    if (in_processing) return;
    in_processing = 1;

    while (1) {
        if (console_switching) break;
        flags = console_irqsave();
        pending = pending_console_switch;
        if (pending >= 0 && pending < NUM_CONSOLES) {
            pending_console_switch = -1;
        }
        console_irqrestore(flags);

        if (pending >= 0 && pending < NUM_CONSOLES) {
            console_switch_internal(pending);
            continue;
        }
        break;
    }

    in_processing = 0;
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

    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;

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

    console_t *con = &consoles[target_console];
    
    if (writer_thread_running) {
        for (size_t i = 0; i < size; i++) {
            uint32_t head = con->write_head;
            uint32_t next_head = (head + 1) % CONSOLE_WRITE_BUFFER_SIZE;
            
            while (next_head == con->write_tail) {
                yield();
                next_head = (con->write_head + 1) % CONSOLE_WRITE_BUFFER_SIZE;
            }
            
            con->write_buffer[head] = data[i];
            con->write_head = next_head;
            con->dirty = 1;
        }
        return;
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
        if (rows == 0) rows = 1;
        if (cols == 0) cols = 1;
        if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
        if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
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

static void console_writer_thread(void) {
    writer_thread_running = 1;
    
    while (1) {
        int work_done = 0;
        int pending_switch_requested = 0;

        if (console_redraw_pending) {
            uint32_t chunk_rows;
            uint32_t visible_rows_local;
            
            uint32_t flags = console_irqsave();
            spin_lock(&console_lock);
            visible_rows_local = console_redraw_visible_rows;
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            
            if (visible_rows_local >= 240) {
                chunk_rows = 12;
            } else if (visible_rows_local >= 200) {
                chunk_rows = 16;
            } else if (visible_rows_local >= 100) {
                chunk_rows = 32;
            } else if (visible_rows_local >= 50) {
                chunk_rows = 16;
            } else {
                chunk_rows = 8;
            }
            console_redraw_step(chunk_rows);
            if (console_redraw_pending) {
                yield();
                continue;
            }
        }

        if (pending_console_switch >= 0) {
            pending_switch_requested = 1;
            goto handle_pending;
        }

        for (int i = 0; i < NUM_CONSOLES; i++) {
            console_t *con = &consoles[i];
            while (con->write_tail != con->write_head) {
                if (pending_console_switch >= 0) {
                    pending_switch_requested = 1;
                    goto handle_pending;
                }
                uint32_t tail = con->write_tail;
                uint32_t head = con->write_head;
                uint32_t available = (head >= tail) ? (head - tail) : (CONSOLE_WRITE_BUFFER_SIZE - tail + head);
                
                if (available == 0) break;
                
                char chunk[64];
                uint32_t chunk_size = (available > 64) ? 64 : available;
                
                for (uint32_t j = 0; j < chunk_size; j++) {
                    chunk[j] = con->write_buffer[(tail + j) % CONSOLE_WRITE_BUFFER_SIZE];
                }
                
                uint32_t flags = console_irqsave();
                spin_lock(&console_lock);
                console_batch++;
                
                framebuffer_t *fb = fb_get();
                uint32_t rows = fb ? fb->rows : 25;
                uint32_t cols = fb ? fb->cols : 80;
                if (rows == 0) rows = 1;
                if (cols == 0) cols = 1;
                if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
                if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
                int is_active = (i == current_console);
                
                for (uint32_t j = 0; j < chunk_size; j++) {
                    char c = chunk[j];
                    
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
                        console_handle_csi(i, con, fb, rows, cols, is_active);
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
                                uint32_t saved_cx = con->cursor_x;
                                uint32_t saved_cy = con->cursor_y;
                                spin_unlock(&console_lock);
                                console_irqrestore(flags);
                                fb_scroll();
                                flags = console_irqsave();
                                spin_lock(&console_lock);
                                fb->cursor_x = saved_cx;
                                fb->cursor_y = saved_cy;
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
                        if (is_active && fb) fb->cursor_x = 0;
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
                            if (is_active && fb) fb_putchar(' ', con->cursor_x, con->cursor_y);
                            con->cursor_x++;
                            if (con->cursor_x >= cols) {
                                con->cursor_x = 0;
                                con->cursor_y++;
                                if (con->cursor_y >= rows) {
                                    console_scroll(con);
                                    con->cursor_y = rows - 1;
                                    if (is_active && fb) fb_scroll();
                                }
                            }
                        }
                        continue;
                    }
                    
                    if (con->cursor_y < CONSOLE_BUFFER_ROWS && con->cursor_x < CONSOLE_BUFFER_COLS) {
                        con->buffer[con->cursor_y][con->cursor_x] = c;
                    }
                    if (is_active && fb) fb_putchar(c, con->cursor_x, con->cursor_y);
                    
                    con->cursor_x++;
                    if (con->cursor_x >= cols) {
                        con->cursor_x = 0;
                        con->cursor_y++;
                        if (con->cursor_y >= rows) {
                            console_scroll(con);
                            con->cursor_y = rows - 1;
                            if (is_active && fb) {
                                spin_unlock(&console_lock);
                                console_irqrestore(flags);
                                fb_scroll();
                                flags = console_irqsave();
                                spin_lock(&console_lock);
                            }
                        }
                    }
                    
                    if (is_active && fb) {
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                    }
                }
                
                con->write_tail = (tail + chunk_size) % CONSOLE_WRITE_BUFFER_SIZE;
                con->dirty = 0;
                
                console_batch--;
                if (console_batch == 0 && is_active && fb) {
                    fb->cursor_x = con->cursor_x;
                    fb->cursor_y = con->cursor_y;
                    fb_update_cursor();
                }
                
                spin_unlock(&console_lock);
                console_irqrestore(flags);

                work_done = 1;
                if (pending_console_switch >= 0) {
                    pending_switch_requested = 1;
                    goto handle_pending;
                }
                yield();
            }
        }
        
        if (pending_console_switch >= 0) {
            pending_switch_requested = 1;
        }

handle_pending:
        console_process_pending();
        if (pending_switch_requested) {
            continue;
        }
        
        if (!work_done) {
            if (pending_console_switch == -1) {
                sleep_ms(5);
            } else {
                yield();
            }
        } else {
            yield();
        }
    }
}

void console_writer_init(void) {
    if (writer_thread_running) return;
    
    extern task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode);
    extern void lock_scheduler(void);
    extern void unlock_scheduler(void);
    extern void add_task_to_runqueue(task_t* new_task);
    
    writer_thread = create_task(console_writer_thread, TASK_READY, false);
    if (writer_thread) {
        writer_thread->is_kernel_task = true;
        strcpy(writer_thread->name, "console_writer");
        lock_scheduler();
        add_task_to_runqueue(writer_thread);
        unlock_scheduler();
    }
}

void console_writer_flush(void) {
    while (writer_thread_running) {
        int all_empty = 1;
        for (int i = 0; i < NUM_CONSOLES; i++) {
            if (consoles[i].write_tail != consoles[i].write_head) {
                all_empty = 0;
                break;
            }
        }
        if (all_empty) break;
        yield();
    }
}
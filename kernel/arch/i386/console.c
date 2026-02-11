#include <kernel/console.h>
#include <kernel/framebuffer.h>
#include <kernel/tty.h>
#include <kernel/common.h>
#include <kernel/spinlock.h>
#include <kernel/vring.h>
#include <kernel/task.h>
#include <kernel/mem_map.h>
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
static char (*console_redraw_buffer)[CONSOLE_BUFFER_COLS];

static int console_ensure_alloc(int n) {
    console_t *con;

    if (n < 0 || n >= NUM_CONSOLES) return -1;
    con = &consoles[n];
    if (con->allocated) return 0;
    con->buffer = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
    if (!con->buffer) return -1;
    con->write_buffer = (char *)kmalloc(CONSOLE_WRITE_BUFFER_SIZE);
    if (!con->write_buffer) {
        kfree(con->buffer);
        con->buffer = NULL;
        return -1;
    }
    memset(con->buffer, ' ', CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
    memset(con->line_wrapped, 0, CONSOLE_BUFFER_ROWS);
    memset(con->write_buffer, 0, CONSOLE_WRITE_BUFFER_SIZE);
    con->allocated = 1;
    return 0;
}
static uint32_t console_redraw_cursor_x = 0;
static uint32_t console_redraw_cursor_y = 0;
static uint32_t console_redraw_rows = 0;
static uint32_t console_redraw_cols = 0;
static uint32_t console_redraw_visible_rows = 0;
static uint32_t console_redraw_visible_cols = 0;
static uint32_t console_redraw_row = 0;
static int console_redraw_console = 0;

static int batch_scroll_count = 0;
static int batch_fb_skip = 0;

static void console_fast_redraw_locked(int console_num) {
    framebuffer_t *fb = fb_get();
    console_t *con;
    uint32_t rows, cols, row, col;
    char c;

    if (!fb || !fb->font) return;
    if (console_num < 0 || console_num >= NUM_CONSOLES) return;

    con = &consoles[console_num];
    if (!con->allocated) return;
    rows = fb->rows;
    cols = fb->cols;
    if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;

    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            c = con->buffer[row][col];
            if ((unsigned char)c >= 32) {
                fb_putchar(c, col, row);
            } else {
                fb_putchar(' ', col, row);
            }
        }
    }

    fb->cursor_x = con->cursor_x;
    fb->cursor_y = con->cursor_y;
    fb_update_cursor();
}

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

    if (!console_redraw_buffer) {
        console_redraw_buffer = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
        if (!console_redraw_buffer) {
            console_redraw_pending = 0;
            return;
        }
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

    if (!console_redraw_buffer) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        console_redraw_pending = 0;
        return;
    }

    for (row = 0; row < visible_rows; row++) {
        for (col = 0; col < visible_cols; col++) {
            console_redraw_buffer[row][col] = (con->allocated && con->buffer) ? con->buffer[row][col] : ' ';
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
    uint32_t flags;

    if (!console_redraw_pending) return;
    if (!fb || !fb->font) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        return;
    }

    flags = console_irqsave();
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
        cols = fb->cols;
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
    fb_flush();
}

void console_tick_redraw(void) {
    if (!console_redraw_pending) return;
    console_redraw_step(8);
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
    int i;
    console_t *con;
    framebuffer_t *fb;

    if (max_cols == 0) max_cols = 1;
    if (max_rows == 0) max_rows = 1;
    if (max_cols > CONSOLE_BUFFER_COLS) max_cols = CONSOLE_BUFFER_COLS;
    if (max_rows > CONSOLE_BUFFER_ROWS) max_rows = CONSOLE_BUFFER_ROWS;

    for (i = 0; i < NUM_CONSOLES; i++) {
        con = &consoles[i];
        if (con->cursor_x >= max_cols) {
            con->cursor_x = max_cols - 1;
        }
        if (con->cursor_y >= max_rows) {
            con->cursor_y = max_rows - 1;
        }
    }

    if (current_console >= 0 && current_console < NUM_CONSOLES) {
        fb = fb_get();
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
    uint32_t flags;

    flags = console_irqsave();
    spin_lock(&console_lock);
    console_clamp_cursors_locked(max_cols, max_rows);
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

static void console_rewrap_one(console_t *con, uint32_t old_cols, uint32_t new_cols, uint32_t new_rows) {
    char *linebuf;
    uint32_t linebuf_len;
    uint32_t linebuf_cap;
    char (*new_buf)[CONSOLE_BUFFER_COLS];
    uint8_t new_wrapped[CONSOLE_BUFFER_ROWS];
    uint32_t out_row;
    uint32_t src_row;
    uint32_t row_end;
    uint32_t col;
    uint32_t lpos;
    uint32_t chars_left;
    uint32_t chunk;
    int cursor_found;
    int total_chars_before_cursor;
    int chars_counted;
    int new_cursor_chars;

    if (!con->allocated || !con->buffer) return;
    if (old_cols == 0) old_cols = 1;
    if (new_cols == 0) new_cols = 1;
    if (new_rows == 0) new_rows = 1;
    if (new_cols > CONSOLE_BUFFER_COLS) new_cols = CONSOLE_BUFFER_COLS;
    if (new_rows > CONSOLE_BUFFER_ROWS) new_rows = CONSOLE_BUFFER_ROWS;
    if (old_cols > CONSOLE_BUFFER_COLS) old_cols = CONSOLE_BUFFER_COLS;

    linebuf_cap = CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS;
    linebuf = (char *)kmalloc(linebuf_cap);
    if (!linebuf) return;

    new_buf = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
    if (!new_buf) {
        kfree(linebuf);
        return;
    }

    memset(new_buf, ' ', CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
    memset(new_wrapped, 0, CONSOLE_BUFFER_ROWS);

    total_chars_before_cursor = 0;
    cursor_found = 0;
    src_row = 0;

    for (src_row = 0; src_row < CONSOLE_BUFFER_ROWS; src_row++) {
        if (src_row < con->cursor_y) {
            row_end = old_cols;
            while (row_end > 0 && con->buffer[src_row][row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = old_cols;
            total_chars_before_cursor += row_end;
            if (!con->line_wrapped[src_row]) total_chars_before_cursor++;
        } else if (src_row == con->cursor_y) {
            total_chars_before_cursor += con->cursor_x;
            cursor_found = 1;
            break;
        }
    }

    out_row = 0;
    src_row = 0;

    while (src_row < CONSOLE_BUFFER_ROWS) {
        linebuf_len = 0;

        while (src_row < CONSOLE_BUFFER_ROWS) {
            row_end = old_cols;
            while (row_end > 0 && con->buffer[src_row][row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = old_cols;

            for (col = 0; col < row_end && linebuf_len < linebuf_cap; col++) {
                linebuf[linebuf_len++] = con->buffer[src_row][col];
            }

            if (!con->line_wrapped[src_row]) {
                src_row++;
                break;
            }
            src_row++;
        }

        while (linebuf_len > 0 && linebuf[linebuf_len - 1] == ' ') linebuf_len--;

        if (linebuf_len == 0) {
            if (out_row < CONSOLE_BUFFER_ROWS) {
                out_row++;
            }
        } else {
            lpos = 0;
            while (lpos < linebuf_len) {
                if (out_row >= CONSOLE_BUFFER_ROWS) {
                    memmove(new_buf[0], new_buf[1], (CONSOLE_BUFFER_ROWS - 1) * CONSOLE_BUFFER_COLS);
                    memmove(new_wrapped, new_wrapped + 1, CONSOLE_BUFFER_ROWS - 1);
                    memset(new_buf[CONSOLE_BUFFER_ROWS - 1], ' ', CONSOLE_BUFFER_COLS);
                    new_wrapped[CONSOLE_BUFFER_ROWS - 1] = 0;
                    out_row = CONSOLE_BUFFER_ROWS - 1;
                }

                chars_left = linebuf_len - lpos;
                chunk = (chars_left > new_cols) ? new_cols : chars_left;

                for (col = 0; col < chunk; col++) {
                    new_buf[out_row][col] = linebuf[lpos + col];
                }

                lpos += chunk;

                if (lpos < linebuf_len) {
                    new_wrapped[out_row] = 1;
                } else {
                    new_wrapped[out_row] = 0;
                }

                out_row++;
            }
        }
    }

    memcpy(con->buffer, new_buf, CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
    memcpy(con->line_wrapped, new_wrapped, CONSOLE_BUFFER_ROWS);

    if (cursor_found) {
        chars_counted = 0;
        con->cursor_x = 0;
        con->cursor_y = 0;
        new_cursor_chars = total_chars_before_cursor;

        for (src_row = 0; src_row < CONSOLE_BUFFER_ROWS; src_row++) {
            row_end = new_cols;
            while (row_end > 0 && con->buffer[src_row][row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = new_cols;

            if (chars_counted + (int)row_end >= new_cursor_chars && !con->line_wrapped[src_row]) {
                con->cursor_y = src_row;
                con->cursor_x = new_cursor_chars - chars_counted;
                if (con->cursor_x >= new_cols) con->cursor_x = new_cols - 1;
                break;
            }

            chars_counted += row_end;
            if (!con->line_wrapped[src_row]) chars_counted++;
        }

        if (src_row >= CONSOLE_BUFFER_ROWS) {
            con->cursor_y = (out_row > 0) ? out_row - 1 : 0;
            con->cursor_x = 0;
        }
    }

    if (con->cursor_x >= new_cols) con->cursor_x = new_cols - 1;
    if (con->cursor_y >= new_rows) con->cursor_y = new_rows - 1;

    kfree(new_buf);
    kfree(linebuf);
}

void console_rewrap_all(uint32_t old_cols, uint32_t new_cols, uint32_t new_rows) {
    uint32_t flags;
    int i;

    if (!console_initialized) return;
    if (old_cols == new_cols) return;

    flags = console_irqsave();
    spin_lock(&console_lock);

    for (i = 0; i < NUM_CONSOLES; i++) {
        if (consoles[i].allocated) {
            console_rewrap_one(&consoles[i], old_cols, new_cols, new_rows);
        }
    }

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_init(void) {
    int i;
    console_t *con;

    if (console_initialized) return;
    
    for (i = 0; i < NUM_CONSOLES; i++) {
        con = &consoles[i];
        con->buffer = NULL;
        con->write_buffer = NULL;
        con->allocated = 0;
        con->cursor_x = 0;
        con->cursor_y = 0;
        con->scroll_offset = 0;
        con->esc_state = 0;
        con->esc_len = 0;
        con->write_head = 0;
        con->write_tail = 0;
        con->dirty = 0;
    }
    
    console_ensure_alloc(0);
    
    console_redraw_buffer = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(CONSOLE_BUFFER_ROWS * CONSOLE_BUFFER_COLS);
    
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
    uint32_t new_cx;
    uint32_t new_cy;
    
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
    
    if (!consoles[console_num].allocated) {
        if (from_interrupt) {
            console_switching = 0;
            console_switch_in_progress = 0;
            pending_console_switch = console_num;
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            if (writer_thread) {
                wake_task(writer_thread);
            }
            return;
        }
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        console_ensure_alloc(console_num);
        flags = console_irqsave();
        spin_lock(&console_lock);
        if (!consoles[console_num].allocated) {
            console_switching = 0;
            console_switch_in_progress = 0;
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            return;
        }
    }
    
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
        new_cx = consoles[current_console].cursor_x;
        new_cy = consoles[current_console].cursor_y;
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
    static volatile int in_processing = 0;
    
    if (!console_initialized) return;
    if (!console_interrupts_enabled()) return;
    
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
    int row;
    int col;

    if (!con->allocated || !con->buffer) return;
    for (row = 0; row < CONSOLE_BUFFER_ROWS - 1; row++) {
        for (col = 0; col < CONSOLE_BUFFER_COLS; col++) {
            con->buffer[row][col] = con->buffer[row + 1][col];
        }
        con->line_wrapped[row] = con->line_wrapped[row + 1];
    }
    for (col = 0; col < CONSOLE_BUFFER_COLS; col++) {
        con->buffer[CONSOLE_BUFFER_ROWS - 1][col] = ' ';
    }
    con->line_wrapped[CONSOLE_BUFFER_ROWS - 1] = 0;
}

void console_putchar(char c) {
    console_putchar_to(current_console, c);
}

static int parse_csi_params(const char *buf, int len, int *params, int max_params) {
    int count = 0;
    int val = 0;
    int has_digit = 0;
    int i;

    for (i = 0; i < len && count < max_params; i++) {
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
    char cmd;
    int param_start;
    int is_private;
    int params[8];
    int nparams;
    int n;
    int row;
    int col;
    int mode;
    uint32_t r;
    uint32_t c2;

    if (con->esc_len == 0) return;

    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;

    cmd = con->esc_buf[con->esc_len - 1];
    
    param_start = 0;
    is_private = 0;
    if (con->esc_buf[0] == '?') {
        is_private = 1;
        param_start = 1;
    }
    
    memset(params, 0, sizeof(params));
    nparams = parse_csi_params(con->esc_buf + param_start, con->esc_len - 1 - param_start, params, 8);

    if (is_private) {
        return;
    }

    switch (cmd) {
    case 'H':
    case 'f':
        row = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        col = (nparams >= 2 && params[1] > 0) ? params[1] - 1 : 0;
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
    case 'A':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if (con->cursor_y >= (uint32_t)n) con->cursor_y -= n;
        else con->cursor_y = 0;
        if (is_active && fb) { fb->cursor_y = con->cursor_y; fb_update_cursor(); }
        break;
    case 'B':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_y += n;
        if (con->cursor_y >= rows) con->cursor_y = rows - 1;
        if (is_active && fb) { fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
        break;
    case 'C':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x += n;
        if (con->cursor_x >= cols) con->cursor_x = cols - 1;
        if (is_active && fb) { fb->cursor_x = con->cursor_x; if (!console_batch) fb_update_cursor(); }
        break;
    case 'D':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if (con->cursor_x >= (uint32_t)n) con->cursor_x -= n;
        else con->cursor_x = 0;
        if (is_active && fb) { fb->cursor_x = con->cursor_x; if (!console_batch) fb_update_cursor(); }
        break;
    case 'J':
        mode = (nparams >= 1) ? params[0] : 0;
        if (mode == 2 || mode == 3) {
            console_clear(console_num);
        } else if (mode == 0) {
            for (c2 = con->cursor_x; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++)
                con->buffer[con->cursor_y][c2] = ' ';
            for (r = con->cursor_y + 1; r < rows && r < CONSOLE_BUFFER_ROWS; r++)
                for (c2 = 0; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++)
                    con->buffer[r][c2] = ' ';
            if (is_active && fb) {
                for (c2 = con->cursor_x; c2 < cols; c2++)
                    fb_putchar(' ', c2, con->cursor_y);
                for (r = con->cursor_y + 1; r < rows; r++)
                    for (c2 = 0; c2 < cols; c2++)
                        fb_putchar(' ', c2, r);
            }
        } else if (mode == 1) {
            for (r = 0; r < con->cursor_y && r < CONSOLE_BUFFER_ROWS; r++)
                for (c2 = 0; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++)
                    con->buffer[r][c2] = ' ';
            for (c2 = 0; c2 <= con->cursor_x && c2 < CONSOLE_BUFFER_COLS; c2++)
                con->buffer[con->cursor_y][c2] = ' ';
            if (is_active && fb) {
                for (r = 0; r < con->cursor_y; r++)
                    for (c2 = 0; c2 < cols; c2++)
                        fb_putchar(' ', c2, r);
                for (c2 = 0; c2 <= con->cursor_x; c2++)
                    fb_putchar(' ', c2, con->cursor_y);
            }
        }
        break;
    case 'K':
        mode = (nparams >= 1) ? params[0] : 0;
        if (mode == 0) {
            for (c2 = con->cursor_x; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        } else if (mode == 1) {
            for (c2 = 0; c2 <= con->cursor_x && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        } else if (mode == 2) {
            for (c2 = 0; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        }
        break;
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
    console_t *con;
    framebuffer_t *fb;
    uint32_t rows;
    uint32_t cols;
    int is_active;
    uint32_t tab_stop;
    uint32_t i;

    if (!console_initialized) {
        terminal_putchar(c);
        return;
    }
    
    if (console_num < 0 || console_num >= NUM_CONSOLES) {
        console_num = current_console;
    }
    
    if (!consoles[console_num].allocated) {
        console_ensure_alloc(console_num);
        if (!consoles[console_num].allocated) return;
    }
    
    if (console_num == 0) {
        if (kprint_is_ready()) {
            kprint_serial_async(&c, 1);
        } else {
            serial_putchar(c);
        }
    }
    
    con = &consoles[console_num];
    fb = fb_get();
    rows = fb ? fb->rows : 25;
    cols = fb ? fb->cols : 80;
    
    is_active = (console_num == current_console);

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
        tab_stop = 8 - (con->cursor_x % 8);
        for (i = 0; i < tab_stop; i++) {
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
        con->line_wrapped[con->cursor_y] = 1;
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
    uint32_t flags;

    if (!console_initialized) {
        terminal_putchar(c);
        return;
    }

    flags = console_irqsave();
    spin_lock(&console_lock);
    console_putchar_to_nolock(console_num, c);
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_write(const char *data, size_t size) {
    size_t i;

    for (i = 0; i < size; i++) {
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
    int target_console;
    console_t *con;
    size_t i;
    uint32_t head;
    uint32_t next_head;
    int skip_serial;
    int batch_started;
    size_t off;
    size_t chunk;
    uint32_t flags;
    framebuffer_t *fb;
    uint32_t rows;
    uint32_t cols;
    int is_active;
    int fb_ok;
    char c;
    uint32_t tab_stop;
    uint32_t t;

    if (!console_initialized) {
        for (i = 0; i < size; i++) terminal_putchar(data[i]);
        return;
    }

    if (console_num == 0 && kprint_is_ready() && !skip_serial_async) {
        kprint_serial_async(data, size);
    }

    target_console = console_num;
    if (target_console < 0 || target_console >= NUM_CONSOLES) {
        target_console = current_console;
    }

    if (!consoles[target_console].allocated) {
        console_ensure_alloc(target_console);
        if (!consoles[target_console].allocated) return;
    }

    con = &consoles[target_console];
    
    if (writer_thread_running) {
        for (i = 0; i < size; i++) {
            head = con->write_head;
            next_head = (head + 1) % CONSOLE_WRITE_BUFFER_SIZE;
            
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
    
    skip_serial = (target_console == 0 && kprint_is_ready());

    batch_started = 0;
    off = 0;
    while (off < size) {
        chunk = size - off;
        if (chunk > 256) chunk = 256;

        flags = console_irqsave();
        spin_lock(&console_lock);
        if (!batch_started) {
            console_batch++;
            batch_started = 1;
            batch_scroll_count = 0;
            batch_fb_skip = 0;
        }

        con = &consoles[target_console];
        fb = fb_get();
        rows = fb ? fb->rows : 25;
        cols = fb ? fb->cols : 80;
        if (rows == 0) rows = 1;
        if (cols == 0) cols = 1;
        if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
        if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
        is_active = (target_console == current_console);
        fb_ok = is_active && fb && !batch_fb_skip;

        for (i = 0; i < chunk; i++) {
            c = data[off + i];
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
                console_handle_csi(target_console, con, fb, rows, cols, fb_ok);
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
                    batch_scroll_count++;
                    if (batch_scroll_count > (int)rows) {
                        batch_fb_skip = 1;
                        fb_ok = 0;
                    }
                    if (fb_ok) {
                        fb_scroll();
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                    }
                }
                if (fb_ok) {
                    fb->cursor_x = con->cursor_x;
                    fb->cursor_y = con->cursor_y;
                }
                continue;
            }

            if (c == '\r') {
                con->cursor_x = 0;
                if (fb_ok) {
                    fb->cursor_x = 0;
                }
                continue;
            }

            if (c == '\b') {
                if (con->cursor_x > 0) {
                    con->cursor_x--;
                    con->buffer[con->cursor_y][con->cursor_x] = ' ';
                    if (fb_ok) {
                        fb->cursor_x = con->cursor_x;
                        fb_putchar(' ', con->cursor_x, con->cursor_y);
                    }
                }
                continue;
            }

            if (c == '\t') {
                tab_stop = 8 - (con->cursor_x % 8);
                for (t = 0; t < tab_stop; t++) {
                    if (con->cursor_y < CONSOLE_BUFFER_ROWS && con->cursor_x < CONSOLE_BUFFER_COLS) {
                        con->buffer[con->cursor_y][con->cursor_x] = ' ';
                    }
                    if (fb_ok) {
                        fb_putchar(' ', con->cursor_x, con->cursor_y);
                    }
                    con->cursor_x++;
                    if (con->cursor_x >= cols) {
                        con->cursor_x = 0;
                        con->cursor_y++;
                        if (con->cursor_y >= rows) {
                            console_scroll(con);
                            con->cursor_y = rows - 1;
                            batch_scroll_count++;
                            if (batch_scroll_count > (int)rows) {
                                batch_fb_skip = 1;
                                fb_ok = 0;
                            }
                            if (fb_ok) {
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

            if (fb_ok) {
                fb_putchar(c, con->cursor_x, con->cursor_y);
            }

            con->cursor_x++;
            if (con->cursor_x >= cols) {
                con->cursor_x = 0;
                con->cursor_y++;
                if (con->cursor_y >= rows) {
                    console_scroll(con);
                    con->cursor_y = rows - 1;
                    batch_scroll_count++;
                    if (batch_scroll_count > (int)rows) {
                        batch_fb_skip = 1;
                        fb_ok = 0;
                    }
                    if (fb_ok) {
                        fb_scroll();
                    }
                }
            }

            if (fb_ok) {
                fb->cursor_x = con->cursor_x;
                fb->cursor_y = con->cursor_y;
            }
        }

        spin_unlock(&console_lock);
        console_irqrestore(flags);

        off += chunk;
        if (current_task && console_interrupts_enabled() && (off % 4096) == 0) {
            yield();
        }
    }

    if (batch_started) {
        flags = console_irqsave();
        spin_lock(&console_lock);
        console_batch--;
        if (console_batch == 0 && console_initialized) {
            fb = fb_get();
            if (fb && fb->font && target_console == current_console) {
                if (batch_fb_skip) {
                    console_fast_redraw_locked(target_console);
                } else {
                    fb->cursor_x = consoles[current_console].cursor_x;
                    fb->cursor_y = consoles[current_console].cursor_y;
                    fb_update_cursor();
                }
            }
            batch_fb_skip = 0;
            batch_scroll_count = 0;
        }
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        fb_flush();
    }
}

void console_writestring(const char *data) {
    while (*data) {
        console_putchar(*data++);
    }
}

void console_clear(int console_num) {
    uint32_t flags;
    console_t *con;
    framebuffer_t *fb;
    int row;
    int col;

    if (console_num < 0 || console_num >= NUM_CONSOLES) return;

    flags = console_irqsave();
    spin_lock(&console_lock);
    
    con = &consoles[console_num];
    if (con->allocated && con->buffer) {
        for (row = 0; row < CONSOLE_BUFFER_ROWS; row++) {
            for (col = 0; col < CONSOLE_BUFFER_COLS; col++) {
                con->buffer[row][col] = ' ';
            }
            con->line_wrapped[row] = 0;
        }
    }
    con->cursor_x = 0;
    con->cursor_y = 0;
    
    if (console_num == current_console) {
        fb = fb_get();
        if (fb) {
            fb_clear();
        }
    }

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_setcursor(int console_num, int x, int y) {
    uint32_t flags;
    console_t *con;
    framebuffer_t *fb;
    uint32_t cols;
    uint32_t rows;

    if (console_num < 0 || console_num >= NUM_CONSOLES) return;

    flags = console_irqsave();
    spin_lock(&console_lock);
    
    con = &consoles[console_num];
    fb = fb_get();
    cols = fb ? fb->cols : 80;
    rows = fb ? fb->rows : 25;
    
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
    uint32_t flags;
    console_t *con;

    if (console_num < 0 || console_num >= NUM_CONSOLES) return -1;

    flags = console_irqsave();
    spin_lock(&console_lock);
    
    con = &consoles[console_num];
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
    int work_done;
    int pending_switch_requested;
    uint32_t chunk_rows;
    uint32_t visible_rows_local;
    uint32_t flags;
    int i;
    console_t *con;
    int wt_scroll_count;
    int wt_fb_skip;
    uint32_t tail;
    uint32_t head;
    uint32_t available;
    char chunk[256];
    uint32_t chunk_size;
    uint32_t j;
    framebuffer_t *fb;
    uint32_t rows;
    uint32_t cols;
    int is_active;
    int wt_fb_ok;
    char c;
    uint32_t tab_stop;
    uint32_t wt;

    writer_thread_running = 1;
    
    while (1) {
        work_done = 0;
        pending_switch_requested = 0;

        if (console_redraw_pending) {
            flags = console_irqsave();
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

        for (i = 0; i < NUM_CONSOLES; i++) {
            con = &consoles[i];
            if (!con->allocated) continue;
            while (con->write_tail != con->write_head) {
                wt_scroll_count = 0;
                wt_fb_skip = 0;

                if (pending_console_switch >= 0) {
                    pending_switch_requested = 1;
                    goto handle_pending;
                }
                tail = con->write_tail;
                head = con->write_head;
                available = (head >= tail) ? (head - tail) : (CONSOLE_WRITE_BUFFER_SIZE - tail + head);
                
                if (available == 0) break;
                
                chunk_size = (available > 256) ? 256 : available;
                
                for (j = 0; j < chunk_size; j++) {
                    chunk[j] = con->write_buffer[(tail + j) % CONSOLE_WRITE_BUFFER_SIZE];
                }
                
                flags = console_irqsave();
                spin_lock(&console_lock);
                console_batch++;
                
                fb = fb_get();
                rows = fb ? fb->rows : 25;
                cols = fb ? fb->cols : 80;
                if (rows == 0) rows = 1;
                if (cols == 0) cols = 1;
                if (rows > CONSOLE_BUFFER_ROWS) rows = CONSOLE_BUFFER_ROWS;
                if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
                is_active = (i == current_console);
                wt_fb_ok = is_active && fb;
                
                for (j = 0; j < chunk_size; j++) {
                    c = chunk[j];
                    
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
                        console_handle_csi(i, con, fb, rows, cols, wt_fb_ok);
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
                            wt_scroll_count++;
                            if (wt_scroll_count > (int)rows) {
                                wt_fb_skip = 1;
                                wt_fb_ok = 0;
                            }
                            if (wt_fb_ok) {
                                fb_scroll();
                                fb->cursor_x = con->cursor_x;
                                fb->cursor_y = con->cursor_y;
                            }
                        }
                        if (wt_fb_ok) {
                            fb->cursor_x = con->cursor_x;
                            fb->cursor_y = con->cursor_y;
                        }
                        continue;
                    }
                    
                    if (c == '\r') {
                        con->cursor_x = 0;
                        if (wt_fb_ok) fb->cursor_x = 0;
                        continue;
                    }
                    
                    if (c == '\b') {
                        if (con->cursor_x > 0) {
                            con->cursor_x--;
                            con->buffer[con->cursor_y][con->cursor_x] = ' ';
                            if (wt_fb_ok) {
                                fb->cursor_x = con->cursor_x;
                                fb_putchar(' ', con->cursor_x, con->cursor_y);
                            }
                        }
                        continue;
                    }
                    
                    if (c == '\t') {
                        tab_stop = 8 - (con->cursor_x % 8);
                        for (wt = 0; wt < tab_stop; wt++) {
                            if (con->cursor_y < CONSOLE_BUFFER_ROWS && con->cursor_x < CONSOLE_BUFFER_COLS) {
                                con->buffer[con->cursor_y][con->cursor_x] = ' ';
                            }
                            if (wt_fb_ok) fb_putchar(' ', con->cursor_x, con->cursor_y);
                            con->cursor_x++;
                            if (con->cursor_x >= cols) {
                                con->cursor_x = 0;
                                con->cursor_y++;
                                if (con->cursor_y >= rows) {
                                    console_scroll(con);
                                    con->cursor_y = rows - 1;
                                    wt_scroll_count++;
                                    if (wt_scroll_count > (int)rows) {
                                        wt_fb_skip = 1;
                                        wt_fb_ok = 0;
                                    }
                                    if (wt_fb_ok) fb_scroll();
                                }
                            }
                        }
                        continue;
                    }
                    
                    if (con->cursor_y < CONSOLE_BUFFER_ROWS && con->cursor_x < CONSOLE_BUFFER_COLS) {
                        con->buffer[con->cursor_y][con->cursor_x] = c;
                    }
                    if (wt_fb_ok) fb_putchar(c, con->cursor_x, con->cursor_y);
                    
                    con->cursor_x++;
                    if (con->cursor_x >= cols) {
                        con->cursor_x = 0;
                        con->cursor_y++;
                        if (con->cursor_y >= rows) {
                            console_scroll(con);
                            con->cursor_y = rows - 1;
                            wt_scroll_count++;
                            if (wt_scroll_count > (int)rows) {
                                wt_fb_skip = 1;
                                wt_fb_ok = 0;
                            }
                            if (wt_fb_ok) {
                                fb_scroll();
                            }
                        }
                    }
                    
                    if (wt_fb_ok) {
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                    }
                }
                
                con->write_tail = (tail + chunk_size) % CONSOLE_WRITE_BUFFER_SIZE;
                con->dirty = 0;
                
                console_batch--;
                if (console_batch == 0 && is_active && fb) {
                    if (wt_fb_skip) {
                        console_fast_redraw_locked(i);
                    } else {
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                        fb_update_cursor();
                    }
                }
                
                spin_unlock(&console_lock);
                console_irqrestore(flags);

                fb_flush();
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
    int all_empty;
    int i;

    while (writer_thread_running) {
        all_empty = 1;
        for (i = 0; i < NUM_CONSOLES; i++) {
            if (!consoles[i].allocated) continue;
            if (consoles[i].write_tail != consoles[i].write_head) {
                all_empty = 0;
                break;
            }
        }
        if (all_empty) break;
        yield();
    }
}
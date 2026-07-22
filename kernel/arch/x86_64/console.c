#include <lebirun/console.h>
#include <lebirun/framebuffer.h>
#include <lebirun/tty.h>
#include <lebirun/common.h>
#include <lebirun/spinlock.h>
#include <lebirun/vring.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/cmdline.h>
#include <string.h>
#include <stdint.h>

extern void terminal_putchar(char c);
extern void serial_putchar(char c);
extern void serial_write_direct(const char *buf, size_t len);
extern framebuffer_t *fb_get(void);
extern void fb_clear(void);
extern void fb_putchar(char c, uint64_t x, uint64_t y);
extern void fb_scroll(void);
extern void fb_update_cursor(void);
extern task_t *current_task;
extern void yield(void);
extern void sleep_ms(uint64_t ms);
extern void wake_task(task_t *task);

static console_t *consoles;
static console_t console_fallback[1];
static int console_count = 0;
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
static uint8_t (*console_redraw_color_buffer)[CONSOLE_BUFFER_COLS];
static uint64_t console_redraw_buffer_rows = 0;

static inline uint64_t console_irqsave(void);
static inline void console_irqrestore(uint64_t flags);
static void console_fast_redraw_locked(int console_num);

static uint64_t console_calc_rows(void) {
    uint64_t r;
    framebuffer_t *fb;

    fb = fb_get();
    r = fb ? fb->rows : 25;
    if (r == 0) r = 25;
    return r;
}

static int console_runtime_count(void) {
    int count;

    count = cmdline_get_consoles();
    if (count <= 0) count = 1;
    if (count > NUM_CONSOLES) count = NUM_CONSOLES;
    return count;
}

static int console_valid_index(int n) {
    if (!consoles) return 0;
    if (n < 0 || n >= console_count) return 0;
    return 1;
}

int console_set_graphics_mode(int console_num, int enabled, int owner_pid) {
    console_t *con;
    framebuffer_t *fb;
    uint64_t flags;
    int redraw;

    if (!console_valid_index(console_num)) return -1;
    con = &consoles[console_num];
    fb = fb_get();
    redraw = 0;
    if (enabled && (!fb || !fb->addr)) return -1;
    flags = console_irqsave();
    spin_lock(&console_lock);
    if (enabled) {
        if (con->graphics_mode && con->graphics_owner_pid != owner_pid) {
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            return -2;
        }
        con->graphics_mode = 1;
        con->graphics_owner_pid = owner_pid;
        if (console_num == current_console) {
            console_redraw_pending = 0;
            console_switch_in_progress = 0;
            console_switching = 0;
        }
    } else {
        if (con->graphics_mode && con->graphics_owner_pid > 0 &&
            con->graphics_owner_pid != owner_pid) {
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            return -2;
        }
        con->graphics_mode = 0;
        con->graphics_owner_pid = 0;
        redraw = console_num == current_console;
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
    if (enabled && console_num == current_console)
        fb_set_cursor_hidden(1);
    if (redraw) {
        fb_set_cursor_hidden(!con->cursor_visible);
        console_force_redraw();
    }
    return 0;
}

int console_get_graphics_mode(int console_num) {
    if (!console_valid_index(console_num)) return 0;
    return consoles[console_num].graphics_mode;
}

void console_release_graphics_owner(int owner_pid) {
    int console_num;
    int redraw;
    uint64_t flags;

    if (owner_pid <= 0) return;
    redraw = 0;
    flags = console_irqsave();
    spin_lock(&console_lock);
    for (console_num = 0; console_num < console_count; console_num++) {
        if (consoles[console_num].graphics_mode &&
            consoles[console_num].graphics_owner_pid == owner_pid) {
            consoles[console_num].graphics_mode = 0;
            consoles[console_num].graphics_owner_pid = 0;
            if (console_num == current_console) redraw = 1;
        }
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
    if (redraw) {
        fb_set_cursor_hidden(0);
        console_force_redraw();
    }
}

static int console_ensure_pool(void) {
    int count;

    if (consoles) return 0;

    count = console_runtime_count();
    consoles = (console_t *)kmalloc(count * sizeof(console_t));
    if (!consoles) {
        consoles = console_fallback;
        count = 1;
    }
    console_count = count;
    memset(consoles, 0, console_count * sizeof(console_t));
    return 0;
}

static int console_ensure_alloc(int n) {
    uint64_t rows;
    console_t *con;

    if (!console_valid_index(n)) return -1;
    con = &consoles[n];
    if (con->allocated) return 0;
    if (n == current_console) {
        rows = console_calc_rows();
    } else {
        rows = CONSOLE_INACTIVE_INITIAL_ROWS;
    }
    if (rows == 0) rows = 25;
    con->buffer_rows = rows;
    con->buffer = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(rows * CONSOLE_BUFFER_COLS);
    if (!con->buffer) return -1;
    con->line_wrapped = (uint8_t *)kmalloc(rows);
    if (!con->line_wrapped) {
        kfree(con->buffer);
        con->buffer = NULL;
        return -1;
    }
    memset(con->buffer, ' ', rows * CONSOLE_BUFFER_COLS);
    memset(con->line_wrapped, 0, rows);
    con->allocated = 1;
    return 0;
}

static uint8_t console_current_attr(console_t *con);

static int console_ensure_color_buffer(console_t *con) {
    uint8_t (*new_color)[CONSOLE_BUFFER_COLS];

    if (!con || !con->allocated) return -1;
    if (con->color_buffer) return 0;
    if (con->buffer_rows == 0) return -1;
    new_color = (uint8_t (*)[CONSOLE_BUFFER_COLS])kmalloc(con->buffer_rows * CONSOLE_BUFFER_COLS);
    if (!new_color) return -1;
    memset(new_color, 0x70, con->buffer_rows * CONSOLE_BUFFER_COLS);
    con->color_buffer = new_color;
    return 0;
}

static void console_ensure_nondefault_color(console_t *con) {
    if (!con) return;
    if (console_current_attr(con) != 0x70) {
        console_ensure_color_buffer(con);
    }
}

static int console_ensure_write_buffer(console_t *con) {
    if (!con || !con->allocated) return -1;
    if (con->write_buffer && con->write_flags && con->write_buffer_size > 0) return 0;
    con->write_buffer_size = CONSOLE_WRITE_BUFFER_INIT;
    con->write_head = 0;
    con->write_tail = 0;
    con->write_buffer = (char *)kmalloc(con->write_buffer_size);
    if (!con->write_buffer) {
        con->write_buffer_size = 0;
        return -1;
    }
    con->write_flags = (uint8_t *)kmalloc(con->write_buffer_size);
    if (!con->write_flags) {
        kfree(con->write_buffer);
        con->write_buffer = NULL;
        con->write_buffer_size = 0;
        return -1;
    }
    memset(con->write_buffer, 0, con->write_buffer_size);
    memset(con->write_flags, 0, con->write_buffer_size);
    return 0;
}

int console_alloc(int n) {
    return console_ensure_alloc(n);
}

int console_alt_screen_active(int n) {
    if (!console_valid_index(n)) return 0;
    return consoles[n].alt_screen_active;
}

static void console_enter_alt_screen(console_t *con) {
    uint64_t rows;
    uint8_t *new_wrapped;
    uint64_t flags;
    char (*new_buf)[CONSOLE_BUFFER_COLS];
    uint8_t (*new_color)[CONSOLE_BUFFER_COLS];

    if (con->alt_screen_active) return;
    rows = con->buffer_rows;
    if (!rows || !con->buffer) return;

    new_buf = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(rows * CONSOLE_BUFFER_COLS);
    new_color = NULL;
    if (con->color_buffer) {
        new_color = (uint8_t (*)[CONSOLE_BUFFER_COLS])kmalloc(rows * CONSOLE_BUFFER_COLS);
    }
    new_wrapped = (uint8_t *)kmalloc(rows);
    if (!new_buf || (con->color_buffer && !new_color) || !new_wrapped) {
        if (new_buf) kfree(new_buf);
        if (new_color) kfree(new_color);
        if (new_wrapped) kfree(new_wrapped);
        return;
    }
    memset(new_buf, ' ', rows * CONSOLE_BUFFER_COLS);
    if (new_color) memset(new_color, 0x70, rows * CONSOLE_BUFFER_COLS);
    memset(new_wrapped, 0, rows);

    flags = console_irqsave();
    spin_lock(&console_lock);

    con->alt_saved_buffer = con->buffer;
    con->alt_saved_color = con->color_buffer;
    con->alt_saved_wrapped = con->line_wrapped;
    con->alt_saved_rows = rows;
    con->alt_saved_cx = con->cursor_x;
    con->alt_saved_cy = con->cursor_y;
    con->alt_saved_scroll = con->scroll_offset;

    con->buffer = new_buf;
    con->color_buffer = new_color;
    con->line_wrapped = new_wrapped;
    con->cursor_x = 0;
    con->cursor_y = 0;
    con->scroll_offset = 0;
    con->alt_screen_active = 1;

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

static void console_leave_alt_screen(console_t *con) {
    uint8_t *old_wrapped;
    uint64_t flags;
    char (*old_buf)[CONSOLE_BUFFER_COLS];
    uint8_t (*old_color)[CONSOLE_BUFFER_COLS];

    if (!con->alt_screen_active) return;
    if (!con->alt_saved_buffer) return;

    flags = console_irqsave();
    spin_lock(&console_lock);

    old_buf = con->buffer;
    old_color = con->color_buffer;
    old_wrapped = con->line_wrapped;

    con->buffer = con->alt_saved_buffer;
    con->color_buffer = con->alt_saved_color;
    con->line_wrapped = con->alt_saved_wrapped;
    con->buffer_rows = con->alt_saved_rows;
    con->cursor_x = con->alt_saved_cx;
    con->cursor_y = con->alt_saved_cy;
    con->scroll_offset = con->alt_saved_scroll;

    con->alt_saved_buffer = NULL;
    con->alt_saved_color = NULL;
    con->alt_saved_wrapped = NULL;
    con->alt_screen_active = 0;

    spin_unlock(&console_lock);
    console_irqrestore(flags);

    kfree(old_buf);
    kfree(old_color);
    kfree(old_wrapped);
}

static void console_process_alt_screen_pending(int console_num) {
    int pending;
    console_t *con;

    con = &consoles[console_num];
    pending = con->alt_screen_pending;
    if (pending == 0) return;
    con->alt_screen_pending = 0;

    if (pending == 1) {
        console_enter_alt_screen(con);
    } else if (pending == -1) {
        console_leave_alt_screen(con);
    }

    if (console_num == current_console && !con->graphics_mode) {
        console_fast_redraw_locked(console_num);
    }
}

static void console_grow_buffer(console_t *con, uint64_t needed_rows) {
    uint8_t *new_wrapped;
    uint64_t old_rows;
    uint64_t copy_rows;
    char (*new_buf)[CONSOLE_BUFFER_COLS];
    uint8_t (*new_color)[CONSOLE_BUFFER_COLS];

    if (!con->allocated) return;
    if (needed_rows <= con->buffer_rows) return;
    old_rows = con->buffer_rows;
    new_buf = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(needed_rows * CONSOLE_BUFFER_COLS);
    if (!new_buf) return;
    new_color = NULL;
    if (con->color_buffer) {
        new_color = (uint8_t (*)[CONSOLE_BUFFER_COLS])kmalloc(needed_rows * CONSOLE_BUFFER_COLS);
        if (!new_color) {
            kfree(new_buf);
            return;
        }
    }
    new_wrapped = (uint8_t *)kmalloc(needed_rows);
    if (!new_wrapped) {
        if (new_color) kfree(new_color);
        kfree(new_buf);
        return;
    }
    memset(new_buf, ' ', needed_rows * CONSOLE_BUFFER_COLS);
    if (new_color) memset(new_color, 0x70, needed_rows * CONSOLE_BUFFER_COLS);
    memset(new_wrapped, 0, needed_rows);
    copy_rows = old_rows;
    if (copy_rows > needed_rows) copy_rows = needed_rows;
    if (con->buffer) {
        memcpy(new_buf, con->buffer, copy_rows * CONSOLE_BUFFER_COLS);
        kfree(con->buffer);
    }
    if (con->color_buffer) {
        memcpy(new_color, con->color_buffer, copy_rows * CONSOLE_BUFFER_COLS);
        kfree(con->color_buffer);
    }
    if (con->line_wrapped) {
        memcpy(new_wrapped, con->line_wrapped, copy_rows);
        kfree(con->line_wrapped);
    }
    con->buffer = new_buf;
    con->color_buffer = new_color;
    con->line_wrapped = new_wrapped;
    con->buffer_rows = needed_rows;
}

static void console_grow_write_buffer(console_t *con) {
    char *new_wb;
    uint8_t *new_wf;
    char *old_wb;
    uint8_t *old_wf;
    uint64_t new_size;
    uint64_t old_size;
    uint64_t tail;
    uint64_t head;
    uint64_t used;
    uint64_t i;
    uint64_t gflags;

    if (!con->allocated || !con->write_buffer) return;
    old_size = con->write_buffer_size;
    new_size = old_size * 2;
    if (new_size > CONSOLE_WRITE_BUFFER_MAX) new_size = CONSOLE_WRITE_BUFFER_MAX;
    if (new_size <= old_size) return;
    new_wb = (char *)kmalloc(new_size);
    if (!new_wb) return;
    new_wf = (uint8_t *)kmalloc(new_size);
    if (!new_wf) { kfree(new_wb); return; }
    gflags = console_irqsave();
    spin_lock(&console_lock);
    tail = con->write_tail;
    head = con->write_head;
    used = (head >= tail) ? (head - tail) : (old_size - tail + head);
    for (i = 0; i < used; i++) {
        new_wb[i] = con->write_buffer[(tail + i) % old_size];
        new_wf[i] = con->write_flags[(tail + i) % old_size];
    }
    memset(new_wb + used, 0, new_size - used);
    memset(new_wf + used, 0, new_size - used);
    old_wb = con->write_buffer;
    old_wf = con->write_flags;
    con->write_buffer = new_wb;
    con->write_flags = new_wf;
    con->write_buffer_size = new_size;
    con->write_tail = 0;
    con->write_head = used;
    spin_unlock(&console_lock);
    console_irqrestore(gflags);
    kfree(old_wb);
    kfree(old_wf);
}

static void console_reclaim_default_color(console_t *con) {
    uint64_t rows;
    uint64_t cols;
    uint64_t r;
    uint64_t c;

    if (!con || !con->color_buffer) return;
    rows = con->buffer_rows;
    cols = CONSOLE_BUFFER_COLS;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            if (con->color_buffer[r][c] != 0x70) return;
        }
    }
    kfree(con->color_buffer);
    con->color_buffer = NULL;
}

void console_reclaim_unused(void) {
    uint64_t flags;
    int i;
    console_t *con;

    if (!console_initialized) return;
    flags = console_irqsave();
    spin_lock(&console_lock);
    for (i = 0; i < console_count; i++) {
        con = &consoles[i];
        if (!con->allocated) continue;
        console_reclaim_default_color(con);
        if (con->write_buffer && con->write_flags && con->write_head == con->write_tail) {
            kfree(con->write_buffer);
            kfree(con->write_flags);
            con->write_buffer = NULL;
            con->write_flags = NULL;
            con->write_buffer_size = 0;
            con->write_head = 0;
            con->write_tail = 0;
        }
    }
    if (!console_redraw_pending) {
        if (console_redraw_buffer) {
            kfree(console_redraw_buffer);
            console_redraw_buffer = NULL;
        }
        if (console_redraw_color_buffer) {
            kfree(console_redraw_color_buffer);
            console_redraw_color_buffer = NULL;
        }
        console_redraw_buffer_rows = 0;
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_memory_stats(uint64_t *buffers, uint64_t *bytes) {
    uint64_t b;
    uint64_t sz;
    uint64_t flags;
    int i;
    console_t *con;

    b = 0;
    sz = 0;
    if (console_initialized) {
        flags = console_irqsave();
        spin_lock(&console_lock);
        for (i = 0; i < console_count; i++) {
            con = &consoles[i];
            if (!con->allocated) continue;
            if (con->buffer) {
                b++;
                sz += con->buffer_rows * CONSOLE_BUFFER_COLS;
            }
            if (con->color_buffer) {
                b++;
                sz += con->buffer_rows * CONSOLE_BUFFER_COLS;
            }
            if (con->line_wrapped) {
                b++;
                sz += con->buffer_rows;
            }
            if (con->write_buffer) {
                b++;
                sz += con->write_buffer_size;
            }
            if (con->write_flags) {
                b++;
                sz += con->write_buffer_size;
            }
        }
        if (console_redraw_buffer) {
            b++;
            sz += console_redraw_buffer_rows * CONSOLE_BUFFER_COLS;
        }
        if (console_redraw_color_buffer) {
            b++;
            sz += console_redraw_buffer_rows * CONSOLE_BUFFER_COLS;
        }
        spin_unlock(&console_lock);
        console_irqrestore(flags);
    }
    if (buffers) *buffers = b;
    if (bytes) *bytes = sz;
}

static void console_drop_redraw_buffers(void) {
    uint64_t flags;

    flags = console_irqsave();
    spin_lock(&console_lock);
    if (!console_redraw_pending) {
        if (console_redraw_buffer) {
            kfree(console_redraw_buffer);
            console_redraw_buffer = NULL;
        }
        if (console_redraw_color_buffer) {
            kfree(console_redraw_color_buffer);
            console_redraw_color_buffer = NULL;
        }
        console_redraw_buffer_rows = 0;
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

int console_get_cell(int console_num, uint64_t x, uint64_t y, char *ch, uint8_t *attr) {
    console_t *con;

    if (!console_valid_index(console_num)) return -1;
    con = &consoles[console_num];
    if (!con->allocated || !con->buffer) return -1;
    if (x >= CONSOLE_BUFFER_COLS || y >= con->buffer_rows) return -1;
    if (ch) *ch = con->buffer[y][x];
    if (attr) {
        if (con->color_buffer) *attr = con->color_buffer[y][x];
        else *attr = 0x70;
    }
    return 0;
}

static uint64_t console_redraw_cursor_x = 0;
static uint64_t console_redraw_cursor_y = 0;
static uint64_t console_redraw_rows = 0;
static uint64_t console_redraw_cols = 0;
static uint64_t console_redraw_visible_rows = 0;
static uint64_t console_redraw_visible_cols = 0;
static uint64_t console_redraw_row = 0;
static int console_redraw_console = 0;

static int batch_scroll_count = 0;
static int batch_fb_skip = 0;

static uint64_t console_ansi_color(uint8_t idx, int bright) {
    static const uint64_t normal[8] = {
        0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAA5500,
        0xFF0000AA, 0xFFAA00AA, 0xFF00AAAA, 0xFFAAAAAA
    };
    static const uint64_t intense[8] = {
        0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
        0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF
    };
    idx &= 7;
    return bright ? intense[idx] : normal[idx];
}

static void console_apply_colors(console_t *con, framebuffer_t *fb) {
    uint8_t fg;
    uint8_t bg;
    uint8_t t;
    int fg_bright;
    int bg_bright;
    uint64_t fg_rgb;
    uint64_t bg_rgb;

    if (!con || !fb) return;

    fg = con->ansi_fg;
    bg = con->ansi_bg;
    if (con->ansi_reverse) {
        t = fg;
        fg = bg;
        bg = t;
    }

    fg_bright = (con->ansi_bold != 0) || (fg >= 8);
    bg_bright = (bg >= 8);
    fg_rgb = console_ansi_color(fg, fg_bright);
    bg_rgb = console_ansi_color(bg, bg_bright);
    fb_set_colors(fg_rgb, bg_rgb);
}

static uint8_t console_current_attr(console_t *con) {
    uint8_t fg;
    uint8_t bg;
    uint8_t t;

    fg = con->ansi_fg;
    bg = con->ansi_bg;
    if (con->ansi_reverse) {
        t = fg;
        fg = bg;
        bg = t;
    }
    if (con->ansi_bold && fg < 8) {
        fg += 8;
    }
    return (uint8_t)((fg << 4) | (bg & 0x0F));
}

static void console_apply_attr(uint8_t attr, framebuffer_t *fb) {
    uint8_t fg_idx;
    uint8_t bg_idx;
    uint64_t fg_rgb;
    uint64_t bg_rgb;
    (void)fb;

    fg_idx = (attr >> 4) & 0x0F;
    bg_idx = attr & 0x0F;
    fg_rgb = console_ansi_color(fg_idx & 7, fg_idx >= 8);
    bg_rgb = console_ansi_color(bg_idx & 7, bg_idx >= 8);
    fb_set_colors(fg_rgb, bg_rgb);
}

static void console_fast_redraw_locked(int console_num) {
    uint64_t rows, cols, row, col;
    char c;
    uint8_t attr;
    uint8_t prev_attr;
    framebuffer_t *fb = fb_get();
    console_t *con;

    if (!fb || (fb->rows == 0 && fb->cols == 0)) return;
    if (!console_valid_index(console_num)) return;

    con = &consoles[console_num];
    if (!con->allocated) return;
    rows = fb->rows;
    cols = fb->cols;
    if (rows > con->buffer_rows) rows = con->buffer_rows;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;

    prev_attr = 0xFF;
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            c = con->buffer[row][col];
            attr = con->color_buffer ? con->color_buffer[row][col] : 0x70;
            if (attr != prev_attr) {
                console_apply_attr(attr, fb);
                prev_attr = attr;
            }
            if ((unsigned char)c >= 32) {
                fb_putchar(c, col, row);
            } else {
                fb_putchar(' ', col, row);
            }
        }
    }

    console_apply_colors(con, fb);
    fb->cursor_x = con->cursor_x;
    fb->cursor_y = con->cursor_y;
    fb_update_cursor();
}

static inline uint64_t console_irqsave(void) {
    uint64_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void console_irqrestore(uint64_t flags) {
    asm volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static inline int console_interrupts_enabled(void) {
    uint64_t flags;
    asm volatile("pushf; pop %0" : "=r"(flags) : : "memory");
    return (flags & (1u << 9)) != 0;
}

static void console_redraw_prepare(int console_num) {
    uint64_t rows;
    uint64_t cols;
    uint64_t row;
    uint64_t col;
    uint64_t flags;
    uint64_t visible_rows;
    uint64_t visible_cols;
    framebuffer_t *fb = fb_get();
    console_t *con;
    int need_color;

    if (!fb || (fb->rows == 0 && fb->cols == 0)) {
        console_redraw_pending = 0;
        return;
    }

    rows = fb->rows;
    cols = fb->cols;
    if (rows == 0 || cols == 0) {
        console_redraw_pending = 0;
        return;
    }

    need_color = 0;
    if (console_valid_index(console_num)) {
        con = &consoles[console_num];
        if (con->allocated && con->color_buffer) need_color = 1;
    }

    if (!console_redraw_buffer || console_redraw_buffer_rows < rows) {
        if (console_redraw_buffer) kfree(console_redraw_buffer);
        if (console_redraw_color_buffer) kfree(console_redraw_color_buffer);
        console_redraw_buffer_rows = rows;
        console_redraw_buffer = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(rows * CONSOLE_BUFFER_COLS);
        console_redraw_color_buffer = NULL;
        if (!console_redraw_buffer) {
            if (console_redraw_buffer) { kfree(console_redraw_buffer); console_redraw_buffer = NULL; }
            console_redraw_buffer_rows = 0;
            console_redraw_pending = 0;
            return;
        }
    }
    if (!need_color && console_redraw_color_buffer) {
        kfree(console_redraw_color_buffer);
        console_redraw_color_buffer = NULL;
    }
    if (need_color && !console_redraw_color_buffer) {
        console_redraw_color_buffer = (uint8_t (*)[CONSOLE_BUFFER_COLS])kmalloc(console_redraw_buffer_rows * CONSOLE_BUFFER_COLS);
        if (!console_redraw_color_buffer) {
            console_redraw_pending = 0;
            return;
        }
    }

    visible_rows = rows;
    visible_cols = cols < CONSOLE_BUFFER_COLS ? cols : CONSOLE_BUFFER_COLS;

    flags = console_irqsave();
    spin_lock(&console_lock);

    if (!console_valid_index(console_num)) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        console_redraw_pending = 0;
        return;
    }

    if (console_redraw_pending) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
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

    if (con->allocated && visible_rows > con->buffer_rows) visible_rows = con->buffer_rows;
    if (visible_rows > console_redraw_buffer_rows) visible_rows = console_redraw_buffer_rows;

    for (row = 0; row < visible_rows; row++) {
        for (col = 0; col < visible_cols; col++) {
            console_redraw_buffer[row][col] = (con->allocated && con->buffer) ? con->buffer[row][col] : ' ';
            if (console_redraw_color_buffer) {
                console_redraw_color_buffer[row][col] = (con->allocated && con->color_buffer) ? con->color_buffer[row][col] : 0x70;
            }
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

static void console_redraw_step(uint64_t max_rows) {
    uint64_t row;
    uint64_t col;
    char c;
    uint64_t end_row;
    uint64_t cols;
    uint64_t rows_processed;
    uint64_t visible_rows_cached;
    uint64_t visible_cols_cached;
    uint64_t current_row_cached;
    uint64_t fb_rows;
    uint64_t flags;
    uint8_t attr;
    uint8_t prev_attr;
    framebuffer_t *fb = fb_get();

    if (!console_redraw_pending) return;
    if (!console_redraw_buffer) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        return;
    }
    if (!fb || (fb->rows == 0 && fb->cols == 0)) {
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

    prev_attr = 0xFF;
    rows_processed = 0;
    for (row = current_row_cached; row < end_row; row++) {
        cols = fb->cols;
        for (col = 0; col < cols; col++) {
            if (row < visible_rows_cached && col < visible_cols_cached) {
                c = console_redraw_buffer[row][col];
                attr = console_redraw_color_buffer ? console_redraw_color_buffer[row][col] : 0x70;
            } else {
                c = ' ';
                attr = 0x70;
            }

            if (attr != prev_attr) {
                console_apply_attr(attr, fb);
                prev_attr = attr;
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
        if (console_valid_index(console_redraw_console)) {
            console_apply_colors(&consoles[console_redraw_console], fb);
        }
        fb->cursor_x = console_redraw_cursor_x;
        fb->cursor_y = console_redraw_cursor_y;
        if (console_valid_index(console_redraw_console)) {
            fb_set_cursor_hidden(!consoles[console_redraw_console].cursor_visible);
        }
        fb_update_cursor();
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        console_fast_redraw_locked(console_redraw_console);
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
    fb_flush();
}

void console_tick_redraw(void) {
    uint64_t flags;

    if (!console_redraw_pending) return;
    flags = console_irqsave();
    spin_lock(&console_lock);
    if (consoles[current_console].graphics_mode) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
    console_redraw_step(8);
}

static void console_redraw_sync(int console_num) {
    uint64_t chunk_rows;
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
    console_drop_redraw_buffers();
}

static void console_clamp_cursors_locked(uint64_t max_cols, uint64_t max_rows) {
    int i;
    console_t *con;
    framebuffer_t *fb;
    uint64_t shift;
    uint64_t r;
    uint64_t row_limit;

    if (max_cols == 0) max_cols = 1;
    if (max_rows == 0) max_rows = 1;
    if (max_cols > CONSOLE_BUFFER_COLS) max_cols = CONSOLE_BUFFER_COLS;

    for (i = 0; i < console_count; i++) {
        con = &consoles[i];
        if (!con->allocated) continue;
        if (i == current_console) {
            console_grow_buffer(con, max_rows);
            row_limit = max_rows;
        } else {
            row_limit = con->buffer_rows;
            if (row_limit == 0) row_limit = 1;
        }
        if (con->cursor_x >= max_cols) {
            con->cursor_x = max_cols - 1;
        }
        if (con->cursor_y >= row_limit) {
            shift = con->cursor_y - (row_limit - 1);
            if (shift > 0 && con->buffer && con->buffer_rows > shift) {
                memmove(con->buffer[0], con->buffer[shift], (con->buffer_rows - shift) * CONSOLE_BUFFER_COLS);
                if (con->color_buffer)
                    memmove(con->color_buffer[0], con->color_buffer[shift], (con->buffer_rows - shift) * CONSOLE_BUFFER_COLS);
                if (con->line_wrapped)
                    memmove(con->line_wrapped, con->line_wrapped + shift, con->buffer_rows - shift);
                for (r = con->buffer_rows - shift; r < con->buffer_rows; r++) {
                    memset(con->buffer[r], ' ', CONSOLE_BUFFER_COLS);
                    if (con->color_buffer)
                        memset(con->color_buffer[r], 0x70, CONSOLE_BUFFER_COLS);
                    if (con->line_wrapped)
                        con->line_wrapped[r] = 0;
                }
            }
            con->cursor_y = row_limit - 1;
        }
        if (con->scroll_bottom > row_limit) {
            con->scroll_bottom = row_limit;
        }
        if (con->scroll_top >= row_limit) {
            con->scroll_top = 0;
        }
    }

    if (console_valid_index(current_console)) {
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

void console_clamp_cursors(uint64_t max_cols, uint64_t max_rows) {
    uint64_t flags;

    flags = console_irqsave();
    spin_lock(&console_lock);
    console_clamp_cursors_locked(max_cols, max_rows);
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

static void console_rewrap_one(console_t *con, uint64_t old_cols, uint64_t new_cols, uint64_t new_rows) {
    char *linebuf;
    uint8_t *colorbuf;
    uint64_t linebuf_len;
    uint64_t linebuf_cap;
    uint8_t *new_wrapped;
    uint64_t out_row;
    uint64_t src_row;
    uint64_t row_end;
    uint64_t col;
    uint64_t lpos;
    uint64_t chars_left;
    uint64_t chunk;
    int cursor_found;
    int total_chars_before_cursor;
    int chars_counted;
    int new_cursor_chars;
    uint64_t buf_rows;
    int have_colors;
    char (*new_buf)[CONSOLE_BUFFER_COLS];
    uint8_t (*new_color_buf)[CONSOLE_BUFFER_COLS];

    if (!con->allocated || !con->buffer) return;
    if (old_cols == 0) old_cols = 1;
    if (new_cols == 0) new_cols = 1;
    if (new_rows == 0) new_rows = 1;
    if (new_cols > CONSOLE_BUFFER_COLS) new_cols = CONSOLE_BUFFER_COLS;
    if (old_cols > CONSOLE_BUFFER_COLS) old_cols = CONSOLE_BUFFER_COLS;

    console_grow_buffer(con, new_rows);
    buf_rows = con->buffer_rows;
    have_colors = (con->color_buffer != NULL);

    linebuf_cap = buf_rows * CONSOLE_BUFFER_COLS;
    linebuf = (char *)kmalloc(linebuf_cap);
    if (!linebuf) return;

    colorbuf = NULL;
    if (have_colors) {
        colorbuf = (uint8_t *)kmalloc(linebuf_cap);
        if (!colorbuf) {
            kfree(linebuf);
            return;
        }
    }

    new_buf = (char (*)[CONSOLE_BUFFER_COLS])kmalloc(buf_rows * CONSOLE_BUFFER_COLS);
    if (!new_buf) {
        kfree(colorbuf);
        kfree(linebuf);
        return;
    }

    new_color_buf = NULL;
    if (have_colors) {
        new_color_buf = (uint8_t (*)[CONSOLE_BUFFER_COLS])kmalloc(buf_rows * CONSOLE_BUFFER_COLS);
        if (!new_color_buf) {
            kfree(new_buf);
            kfree(colorbuf);
            kfree(linebuf);
            return;
        }
    }

    new_wrapped = (uint8_t *)kmalloc(buf_rows);
    if (!new_wrapped) {
        kfree(new_color_buf);
        kfree(new_buf);
        kfree(colorbuf);
        kfree(linebuf);
        return;
    }

    memset(new_buf, ' ', buf_rows * CONSOLE_BUFFER_COLS);
    if (new_color_buf)
        memset(new_color_buf, 0x70, buf_rows * CONSOLE_BUFFER_COLS);
    memset(new_wrapped, 0, buf_rows);

    total_chars_before_cursor = 0;
    cursor_found = 0;
    src_row = 0;

    for (src_row = 0; src_row < buf_rows; src_row++) {
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

    while (src_row < buf_rows) {
        linebuf_len = 0;

        while (src_row < buf_rows) {
            row_end = old_cols;
            while (row_end > 0 && con->buffer[src_row][row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = old_cols;

            for (col = 0; col < row_end && linebuf_len < linebuf_cap; col++) {
                linebuf[linebuf_len] = con->buffer[src_row][col];
                if (colorbuf)
                    colorbuf[linebuf_len] = con->color_buffer[src_row][col];
                linebuf_len++;
            }

            if (!con->line_wrapped[src_row]) {
                src_row++;
                break;
            }
            src_row++;
        }

        while (linebuf_len > 0 && linebuf[linebuf_len - 1] == ' ') linebuf_len--;

        if (linebuf_len == 0) {
            if (out_row < buf_rows) {
                out_row++;
            }
        } else {
            lpos = 0;
            while (lpos < linebuf_len) {
                if (out_row >= buf_rows) {
                    memmove(new_buf[0], new_buf[1], (buf_rows - 1) * CONSOLE_BUFFER_COLS);
                    if (new_color_buf)
                        memmove(new_color_buf[0], new_color_buf[1], (buf_rows - 1) * CONSOLE_BUFFER_COLS);
                    memmove(new_wrapped, new_wrapped + 1, buf_rows - 1);
                    memset(new_buf[buf_rows - 1], ' ', CONSOLE_BUFFER_COLS);
                    if (new_color_buf)
                        memset(new_color_buf[buf_rows - 1], 0x70, CONSOLE_BUFFER_COLS);
                    new_wrapped[buf_rows - 1] = 0;
                    out_row = buf_rows - 1;
                }

                chars_left = linebuf_len - lpos;
                chunk = (chars_left > new_cols) ? new_cols : chars_left;

                for (col = 0; col < chunk; col++) {
                    new_buf[out_row][col] = linebuf[lpos + col];
                    if (new_color_buf && colorbuf)
                        new_color_buf[out_row][col] = colorbuf[lpos + col];
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

    memcpy(con->buffer, new_buf, buf_rows * CONSOLE_BUFFER_COLS);
    memcpy(con->line_wrapped, new_wrapped, buf_rows);

    if (have_colors && new_color_buf) {
        for (src_row = 0; src_row < buf_rows; src_row++)
            memcpy(con->color_buffer[src_row], new_color_buf[src_row], CONSOLE_BUFFER_COLS);
    }

    if (cursor_found) {
        chars_counted = 0;
        con->cursor_x = 0;
        con->cursor_y = 0;
        new_cursor_chars = total_chars_before_cursor;

        for (src_row = 0; src_row < buf_rows; src_row++) {
            row_end = new_cols;
            while (row_end > 0 && con->buffer[src_row][row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = new_cols;

            if (chars_counted + (int)row_end >= new_cursor_chars) {
                con->cursor_y = src_row;
                con->cursor_x = new_cursor_chars - chars_counted;
                if ((uint64_t)con->cursor_x >= new_cols) con->cursor_x = (int)new_cols - 1;
                break;
            }

            chars_counted += row_end;
            if (!con->line_wrapped[src_row]) chars_counted++;
        }

        if (src_row >= buf_rows) {
            con->cursor_y = (out_row > 0) ? out_row - 1 : 0;
            con->cursor_x = 0;
        }
    }

    if (con->cursor_x >= new_cols) con->cursor_x = new_cols - 1;
    if (con->cursor_y >= new_rows) {
        uint64_t shift;
        uint64_t r;
        shift = con->cursor_y - (new_rows - 1);
        if (shift > buf_rows) shift = buf_rows;
        memmove(con->buffer, con->buffer + shift,
                (buf_rows - shift) * CONSOLE_BUFFER_COLS);
        if (con->color_buffer)
            memmove(con->color_buffer, con->color_buffer + shift,
                    (buf_rows - shift) * CONSOLE_BUFFER_COLS);
        memmove(con->line_wrapped, con->line_wrapped + shift,
                (buf_rows - shift) * sizeof(con->line_wrapped[0]));
        for (r = buf_rows - shift; r < buf_rows; r++) {
            memset(con->buffer[r], ' ', CONSOLE_BUFFER_COLS);
            if (con->color_buffer)
                memset(con->color_buffer[r], 0x70, CONSOLE_BUFFER_COLS);
            con->line_wrapped[r] = 0;
        }
        con->cursor_y = new_rows - 1;
    }

    if (con->scroll_bottom > new_rows) {
        con->scroll_bottom = new_rows;
    }
    if (con->scroll_top >= new_rows) {
        con->scroll_top = 0;
    }

    kfree(new_color_buf);
    kfree(new_buf);
    kfree(colorbuf);
    kfree(linebuf);
}

void console_rewrap_all(uint64_t old_cols, uint64_t new_cols, uint64_t new_rows) {
    uint64_t flags;
    int i;

    if (!console_initialized) return;
    if (old_cols == new_cols) return;

    flags = console_irqsave();
    spin_lock(&console_lock);

    for (i = 0; i < console_count; i++) {
        if (consoles[i].allocated) {
            console_rewrap_one(&consoles[i], old_cols, new_cols, new_rows);
        }
    }

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_init(void) {
    int i;
    int alloc_ok;
    console_t *con;

    if (console_initialized) return;
    console_ensure_pool();
    if (!consoles) return;
    
    for (i = 0; i < console_count; i++) {
        con = &consoles[i];
        con->buffer = NULL;
        con->color_buffer = NULL;
        con->line_wrapped = NULL;
        con->write_buffer = NULL;
        con->buffer_rows = 0;
        con->write_buffer_size = 0;
        con->allocated = 0;
        con->cursor_x = 0;
        con->cursor_y = 0;
        con->scroll_offset = 0;
        con->esc_state = 0;
        con->esc_len = 0;
        con->write_head = 0;
        con->write_tail = 0;
        con->dirty = 0;
        con->ansi_fg = 7;
        con->ansi_bg = 0;
        con->ansi_bold = 0;
        con->ansi_reverse = 0;
        con->scroll_top = 0;
        con->scroll_bottom = 0;
        con->cursor_visible = 1;
        con->saved_cursor_x = 0;
        con->saved_cursor_y = 0;
        con->alt_screen_active = 0;
        con->alt_screen_pending = 0;
        con->alt_saved_buffer = NULL;
        con->alt_saved_color = NULL;
        con->alt_saved_wrapped = NULL;
        con->alt_saved_rows = 0;
        con->alt_saved_cx = 0;
        con->alt_saved_cy = 0;
        con->alt_saved_scroll = 0;
        con->graphics_mode = 0;
        con->graphics_owner_pid = 0;
    }
    
    alloc_ok = console_ensure_alloc(0);
    if (alloc_ok != 0) return;
    
    current_console = 0;
    console_switching = 0;
    console_switch_in_progress = 0;
    pending_console_switch = -1;
    console_redraw_pending = 0;
    console_batch = 0;
    
    console_initialized = 1;
}

void console_reinit(void) {
    console_initialized = 0;
    console_init();
}

void console_redraw_current(void) {
    if (!console_initialized) return;
    if (console_switch_in_progress) return;
    if (console_get_graphics_mode(current_console)) return;
    if (writer_thread_running) {
        console_redraw_prepare(current_console);
        return;
    }
    console_redraw_sync(current_console);
}

void console_force_redraw(void) {
    if (!console_initialized) return;
    if (console_get_graphics_mode(current_console)) return;
    console_switch_in_progress = 0;
    console_redraw_pending = 0;
    console_redraw_sync(current_console);
}

static void console_switch_internal_impl(int console_num, int from_interrupt) {
    uint64_t rows;
    uint64_t cols;
    uint64_t flags;
    int lock_acquired;
    uint64_t new_cx;
    uint64_t new_cy;
    framebuffer_t *fb;
    console_t *new_con;
    
    if (!console_valid_index(console_num)) return;
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
    
    new_con = &consoles[console_num];
    
    rows = fb ? fb->rows : 25;
    cols = fb ? fb->cols : 80;
    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
    if (from_interrupt && new_con->buffer_rows < rows) {
        console_switching = 0;
        console_switch_in_progress = 0;
        pending_console_switch = console_num;
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        if (writer_thread) wake_task(writer_thread);
        return;
    }
    console_grow_buffer(new_con, rows);
    
    console_clamp_cursors_locked(cols, rows);
    
    current_console = console_num;
    
    if (fb) {
        console_apply_colors(new_con, fb);
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

    if (new_con->graphics_mode) {
        fb_set_cursor_hidden(1);
        flags = console_irqsave();
        spin_lock(&console_lock);
        console_switching = 0;
        console_switch_in_progress = 0;
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }

    console_redraw_prepare(current_console);
    if (!writer_thread_running && !from_interrupt) {
        console_redraw_sync(current_console);
    }
}

static void console_switch_internal(int console_num) {
    console_switch_internal_impl(console_num, 0);
}

void console_switch(int console_num) {
    if (!console_valid_index(console_num)) return;
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
    uint64_t flags;

    if (!console_valid_index(console_num)) return;
    if (!console_initialized) return;
    if (console_num == current_console) return;

    flags = console_irqsave();
    pending_console_switch = console_num;
    console_irqrestore(flags);
    if (writer_thread) {
        wake_task(writer_thread);
    }
}

void console_process_pending(void) {
    int pending;
    uint64_t flags;
    static volatile int in_processing = 0;
    
    if (!console_initialized) return;
    if (!console_interrupts_enabled()) return;
    
    if (in_processing) return;
    in_processing = 1;

    while (1) {
        if (console_switching) break;
        flags = console_irqsave();
        pending = pending_console_switch;
        if (console_valid_index(pending)) {
            pending_console_switch = -1;
        }
        console_irqrestore(flags);

        if (console_valid_index(pending)) {
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

static void console_scroll_region_up(console_t *con, uint64_t top, uint64_t bottom, uint64_t cols) {
    uint64_t row;
    uint64_t copy_cols;

    if (top >= bottom) return;
    if (bottom > con->buffer_rows) bottom = con->buffer_rows;
    copy_cols = cols < CONSOLE_BUFFER_COLS ? cols : CONSOLE_BUFFER_COLS;
    for (row = top; row < bottom - 1; row++) {
        memcpy(con->buffer[row], con->buffer[row + 1], copy_cols);
        if (con->color_buffer)
            memcpy(con->color_buffer[row], con->color_buffer[row + 1], copy_cols);
        con->line_wrapped[row] = con->line_wrapped[row + 1];
    }
    if (bottom > 0) {
        memset(con->buffer[bottom - 1], ' ', copy_cols);
        if (con->color_buffer)
            memset(con->color_buffer[bottom - 1], console_current_attr(con), copy_cols);
        con->line_wrapped[bottom - 1] = 0;
    }
}

static void console_scroll_region_down(console_t *con, uint64_t top, uint64_t bottom, uint64_t cols) {
    uint64_t row;
    uint64_t copy_cols;

    if (top >= bottom) return;
    if (bottom > con->buffer_rows) bottom = con->buffer_rows;
    copy_cols = cols < CONSOLE_BUFFER_COLS ? cols : CONSOLE_BUFFER_COLS;
    for (row = bottom - 1; row > top; row--) {
        memcpy(con->buffer[row], con->buffer[row - 1], copy_cols);
        if (con->color_buffer)
            memcpy(con->color_buffer[row], con->color_buffer[row - 1], copy_cols);
        con->line_wrapped[row] = con->line_wrapped[row - 1];
    }
    memset(con->buffer[top], ' ', copy_cols);
    if (con->color_buffer)
        memset(con->color_buffer[top], console_current_attr(con), copy_cols);
    con->line_wrapped[top] = 0;
}

static void console_handle_csi(int console_num, console_t *con, framebuffer_t *fb, uint64_t rows, uint64_t cols, int is_active) {
    char cmd;
    int param_start;
    int is_private;
    int params[8];
    int nparams;
    int n;
    int p;
    int row;
    int col;
    int mode;
    uint64_t r;
    uint64_t c2;
    uint64_t top;
    uint64_t bot;
    int count;
    (void)console_num;

    if (con->esc_len == 0) return;

    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    console_grow_buffer(con, rows);
    if (rows > con->buffer_rows) rows = con->buffer_rows;
    if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
    console_ensure_nondefault_color(con);

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
        if (cmd == 'h') {
            if (nparams >= 1 && params[0] == 25) {
                con->cursor_visible = 1;
                if (is_active && fb) {
                    fb_set_cursor_hidden(0);
                    if (!console_batch) fb_update_cursor();
                }
            }
            if (nparams >= 1 && params[0] == 1049) {
                con->alt_screen_pending = 1;
                con->dirty = 1;
            }
        } else if (cmd == 'l') {
            if (nparams >= 1 && params[0] == 25) {
                con->cursor_visible = 0;
                if (is_active && fb) {
                    fb_set_cursor_hidden(1);
                }
            }
            if (nparams >= 1 && params[0] == 1049) {
                con->alt_screen_pending = -1;
                con->dirty = 1;
            }
        }
        return;
    }

    switch (cmd) {
    case 'H':
    case 'f':
        row = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        col = (nparams >= 2 && params[1] > 0) ? params[1] - 1 : 0;
        if ((uint64_t)row >= rows) row = rows - 1;
        if ((uint64_t)col >= cols) col = cols - 1;
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
        if (con->cursor_y >= (uint64_t)n) con->cursor_y -= n;
        else con->cursor_y = 0;
        if (is_active && fb) { fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
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
        if (con->cursor_x >= (uint64_t)n) con->cursor_x -= n;
        else con->cursor_x = 0;
        if (is_active && fb) { fb->cursor_x = con->cursor_x; if (!console_batch) fb_update_cursor(); }
        break;
    case 'E':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x = 0;
        con->cursor_y += n;
        if (con->cursor_y >= rows) con->cursor_y = rows - 1;
        if (is_active && fb) { fb->cursor_x = 0; fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
        break;
    case 'F':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x = 0;
        if (con->cursor_y >= (uint64_t)n) con->cursor_y -= n;
        else con->cursor_y = 0;
        if (is_active && fb) { fb->cursor_x = 0; fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
        break;
    case 'G':
        col = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        if ((uint64_t)col >= cols) col = cols - 1;
        con->cursor_x = col;
        if (is_active && fb) { fb->cursor_x = col; if (!console_batch) fb_update_cursor(); }
        break;
    case 'd':
        row = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        if ((uint64_t)row >= rows) row = rows - 1;
        con->cursor_y = row;
        if (is_active && fb) { fb->cursor_y = row; if (!console_batch) fb_update_cursor(); }
        break;
    case 'J':
        mode = (nparams >= 1) ? params[0] : 0;
        if (mode == 2 || mode == 3) {
            if (con->allocated && con->buffer) {
                for (r = 0; r < con->buffer_rows; r++) {
                    for (c2 = 0; c2 < CONSOLE_BUFFER_COLS; c2++) {
                        con->buffer[r][c2] = ' ';
                        if (con->color_buffer) con->color_buffer[r][c2] = console_current_attr(con);
                    }
                    con->line_wrapped[r] = 0;
                }
            }
            con->cursor_x = 0;
            con->cursor_y = 0;
            if (is_active && fb) {
                fb->cursor_x = 0;
                fb->cursor_y = 0;
                console_fast_redraw_locked(console_num);
            }
        } else if (mode == 0) {
            for (c2 = con->cursor_x; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
            }
            for (r = con->cursor_y + 1; r < rows && r < con->buffer_rows; r++)
                for (c2 = 0; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
                    con->buffer[r][c2] = ' ';
                    if (con->color_buffer) con->color_buffer[r][c2] = console_current_attr(con);
                }
            if (is_active && fb) {
                for (c2 = con->cursor_x; c2 < cols; c2++)
                    fb_putchar(' ', c2, con->cursor_y);
                for (r = con->cursor_y + 1; r < rows; r++)
                    for (c2 = 0; c2 < cols; c2++)
                        fb_putchar(' ', c2, r);
            }
        } else if (mode == 1) {
            for (r = 0; r < con->cursor_y && r < con->buffer_rows; r++)
                for (c2 = 0; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
                    con->buffer[r][c2] = ' ';
                    if (con->color_buffer) con->color_buffer[r][c2] = console_current_attr(con);
                }
            for (c2 = 0; c2 <= con->cursor_x && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
            }
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
                if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        } else if (mode == 1) {
            for (c2 = 0; c2 <= con->cursor_x && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        } else if (mode == 2) {
            for (c2 = 0; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
                con->buffer[con->cursor_y][c2] = ' ';
                if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        }
        break;
    case 'X':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        for (c2 = con->cursor_x; c2 < con->cursor_x + (uint64_t)n && c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
            con->buffer[con->cursor_y][c2] = ' ';
            if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
            if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
        }
        break;
    case '@':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if ((uint64_t)n > cols - con->cursor_x) n = cols - con->cursor_x;
        for (c2 = cols - 1; c2 >= con->cursor_x + (uint64_t)n && c2 < CONSOLE_BUFFER_COLS; c2--) {
            con->buffer[con->cursor_y][c2] = con->buffer[con->cursor_y][c2 - n];
            if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = con->color_buffer[con->cursor_y][c2 - n];
        }
        for (c2 = con->cursor_x; c2 < con->cursor_x + (uint64_t)n && c2 < CONSOLE_BUFFER_COLS; c2++) {
            con->buffer[con->cursor_y][c2] = ' ';
            if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
        }
        if (is_active && fb) {
            for (c2 = 0; c2 < cols; c2++) {
                if (con->color_buffer) console_apply_attr(con->color_buffer[con->cursor_y][c2], fb);
                fb_putchar(con->buffer[con->cursor_y][c2], c2, con->cursor_y);
            }
            console_apply_colors(con, fb);
        }
        break;
    case 'P':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if ((uint64_t)n > cols - con->cursor_x) n = cols - con->cursor_x;
        for (c2 = con->cursor_x; c2 + (uint64_t)n < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
            con->buffer[con->cursor_y][c2] = con->buffer[con->cursor_y][c2 + n];
            if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = con->color_buffer[con->cursor_y][c2 + n];
        }
        for (c2 = cols - (uint64_t)n; c2 < cols && c2 < CONSOLE_BUFFER_COLS; c2++) {
            con->buffer[con->cursor_y][c2] = ' ';
            if (con->color_buffer) con->color_buffer[con->cursor_y][c2] = console_current_attr(con);
        }
        if (is_active && fb) {
            for (c2 = 0; c2 < cols; c2++) {
                if (con->color_buffer) console_apply_attr(con->color_buffer[con->cursor_y][c2], fb);
                fb_putchar(con->buffer[con->cursor_y][c2], c2, con->cursor_y);
            }
            console_apply_colors(con, fb);
        }
        break;
    case 'L':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        top = con->cursor_y;
        bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
        if (top >= bot) break;
        for (count = 0; count < n; count++) {
            console_scroll_region_down(con, top, bot, cols);
        }
        if (is_active && fb) {
            for (r = top; r < bot; r++)
                for (c2 = 0; c2 < cols; c2++) {
                    if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                    fb_putchar(con->buffer[r][c2], c2, r);
                }
            console_apply_colors(con, fb);
        }
        break;
    case 'M':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        top = con->cursor_y;
        bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
        if (top >= bot) break;
        for (count = 0; count < n; count++) {
            console_scroll_region_up(con, top, bot, cols);
        }
        if (is_active && fb) {
            if (top == 0 && bot == rows) {
                for (count = 0; count < n; count++) fb_scroll();
            } else {
                for (r = top; r < bot; r++)
                    for (c2 = 0; c2 < cols; c2++) {
                        if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                        fb_putchar(con->buffer[r][c2], c2, r);
                    }
            }
            console_apply_colors(con, fb);
        }
        break;
    case 'S':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        top = (con->scroll_top > 0) ? con->scroll_top : 0;
        bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
        for (count = 0; count < n; count++) {
            console_scroll_region_up(con, top, bot, cols);
        }
        if (is_active && fb) {
            if (top == 0 && bot == rows) {
                for (count = 0; count < n; count++) fb_scroll();
            } else {
                for (r = top; r < bot; r++)
                    for (c2 = 0; c2 < cols; c2++) {
                        if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                        fb_putchar(con->buffer[r][c2], c2, r);
                    }
            }
            console_apply_colors(con, fb);
        }
        break;
    case 'T':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        top = (con->scroll_top > 0) ? con->scroll_top : 0;
        bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
        for (count = 0; count < n; count++) {
            console_scroll_region_down(con, top, bot, cols);
        }
        if (is_active && fb) {
            for (r = top; r < bot; r++)
                for (c2 = 0; c2 < cols; c2++) {
                    if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                    fb_putchar(con->buffer[r][c2], c2, r);
                }
            console_apply_colors(con, fb);
        }
        break;
    case 'r':
        if (nparams == 0) {
            con->scroll_top = 0;
            con->scroll_bottom = 0;
        } else {
            con->scroll_top = (nparams >= 1 && params[0] > 0) ? (uint64_t)(params[0] - 1) : 0;
            con->scroll_bottom = (nparams >= 2 && params[1] > 0) ? (uint64_t)params[1] : rows;
            if (con->scroll_top >= rows) con->scroll_top = 0;
            if (con->scroll_bottom > rows) con->scroll_bottom = rows;
            if (con->scroll_top >= con->scroll_bottom) {
                con->scroll_top = 0;
                con->scroll_bottom = 0;
            }
        }
        con->cursor_x = 0;
        con->cursor_y = 0;
        if (is_active && fb) {
            fb->cursor_x = 0;
            fb->cursor_y = 0;
            if (!console_batch) fb_update_cursor();
        }
        break;
    case 'm':
        if (nparams == 0) {
            con->ansi_fg = 7;
            con->ansi_bg = 0;
            con->ansi_bold = 0;
            con->ansi_reverse = 0;
        } else {
            for (n = 0; n < nparams; n++) {
                p = params[n];
                if (p == 0) {
                    con->ansi_fg = 7;
                    con->ansi_bg = 0;
                    con->ansi_bold = 0;
                    con->ansi_reverse = 0;
                } else if (p == 1) {
                    con->ansi_bold = 1;
                } else if (p == 2) {
                    con->ansi_bold = 0;
                } else if (p == 4 || p == 24) {
                    ;
                } else if (p == 5 || p == 25) {
                    ;
                } else if (p == 7) {
                    con->ansi_reverse = 1;
                } else if (p == 8) {
                    ;
                } else if (p == 10 || p == 11 || p == 12) {
                    ;
                } else if (p == 22) {
                    con->ansi_bold = 0;
                } else if (p == 27) {
                    con->ansi_reverse = 0;
                } else if (p >= 30 && p <= 37) {
                    con->ansi_fg = (uint8_t)(p - 30);
                } else if (p >= 90 && p <= 97) {
                    con->ansi_fg = (uint8_t)(8 + (p - 90));
                } else if (p == 39) {
                    con->ansi_fg = 7;
                } else if (p >= 40 && p <= 47) {
                    con->ansi_bg = (uint8_t)(p - 40);
                } else if (p >= 100 && p <= 107) {
                    con->ansi_bg = (uint8_t)(8 + (p - 100));
                } else if (p == 49) {
                    con->ansi_bg = 0;
                }
            }
        }
        console_ensure_nondefault_color(con);
        if (is_active && fb) {
            console_apply_colors(con, fb);
        }
        break;
    case 'h':
    case 'l':
    case 's':
    case 'u':
    default:
        break;
    }
}

static void console_putchar_to_nolock(int console_num, char c) {
    uint64_t rows;
    uint64_t cols;
    int is_active;
    uint64_t tab_stop;
    uint64_t i;
    uint64_t sc_top;
    uint64_t sc_bot;
    uint64_t r;
    uint64_t c2;
    console_t *con;
    framebuffer_t *fb;

    if (!console_initialized) {
        terminal_putchar(c);
        return;
    }
    
    if (!console_valid_index(console_num)) {
        console_num = current_console;
    }
    
    if (console_num == 0) {
        if (kprint_is_ready()) {
            kprint_serial_async(&c, 1);
        } else {
            serial_write_direct(&c, 1);
        }
    }
    
    if (!consoles[console_num].allocated) {
        console_ensure_alloc(console_num);
        if (!consoles[console_num].allocated) return;
    }
    
    con = &consoles[console_num];
    fb = fb_get();
    is_active = (console_num == current_console && !console_switch_in_progress &&
                 !con->graphics_mode);
    rows = fb ? fb->rows : 25;
    cols = fb ? fb->cols : 80;
    if (rows == 0) rows = 25;
    if (cols == 0) cols = 80;
    if (!is_active && con->buffer_rows > 0) rows = con->buffer_rows;

    console_grow_buffer(con, rows);
    if (rows > con->buffer_rows) rows = con->buffer_rows;

    if (con->esc_state == 1) {
        if (c == '[') {
            con->esc_state = 2;
            con->esc_len = 0;
            return;
        }
        if (c == '(' || c == ')') {
            con->esc_state = 3;
            return;
        }
        if (c == 'M') {
            sc_top = con->scroll_top;
            sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
            if (con->cursor_y <= sc_top) {
                console_scroll_region_down(con, sc_top, sc_bot, cols);
                if (is_active && fb) {
                    for (r = sc_top; r < sc_bot; r++)
                        for (c2 = 0; c2 < cols; c2++) {
                            if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                            fb_putchar(con->buffer[r][c2], c2, r);
                        }
                    console_apply_colors(con, fb);
                }
            } else {
                con->cursor_y--;
                if (is_active && fb) { fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
            }
            con->esc_state = 0;
            return;
        }
        if (c == 'D') {
            sc_top = con->scroll_top;
            sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
            if (con->cursor_y + 1 >= sc_bot) {
                console_scroll_region_up(con, sc_top, sc_bot, cols);
                if (is_active && fb) {
                    if (sc_top == 0 && sc_bot == rows) {
                        fb_scroll();
                    } else {
                        for (r = sc_top; r < sc_bot; r++)
                            for (c2 = 0; c2 < cols; c2++) {
                                if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                                fb_putchar(con->buffer[r][c2], c2, r);
                            }
                    }
                    console_apply_colors(con, fb);
                }
            } else {
                con->cursor_y++;
                if (is_active && fb) { fb->cursor_y = con->cursor_y; if (!console_batch) fb_update_cursor(); }
            }
            con->esc_state = 0;
            return;
        }
        if (c == 'c') {
            con->ansi_fg = 7;
            con->ansi_bg = 0;
            con->ansi_bold = 0;
            con->ansi_reverse = 0;
            con->scroll_top = 0;
            con->scroll_bottom = 0;
            con->cursor_visible = 1;
            if (is_active && fb) console_apply_colors(con, fb);
            con->esc_state = 0;
            return;
        }
        if (c == '7') {
            con->saved_cursor_x = con->cursor_x;
            con->saved_cursor_y = con->cursor_y;
            con->esc_state = 0;
            return;
        }
        if (c == '8') {
            con->cursor_x = con->saved_cursor_x;
            con->cursor_y = con->saved_cursor_y;
            if (con->cursor_x >= cols) con->cursor_x = cols - 1;
            if (con->cursor_y >= rows) con->cursor_y = rows - 1;
            if (is_active && fb) {
                fb->cursor_x = con->cursor_x;
                fb->cursor_y = con->cursor_y;
                if (!console_batch) fb_update_cursor();
            }
            con->esc_state = 0;
            return;
        }
        con->esc_state = 0;
        return;
    }

    if (con->esc_state == 3) {
        con->esc_state = 0;
        return;
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

    if (c == 0x07 || c == 0x0E || c == 0x0F) {
        return;
    }

    console_ensure_nondefault_color(con);

    if (c == '\n') {
        con->cursor_x = 0;
        sc_top = con->scroll_top;
        sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
        con->cursor_y++;
        if (con->cursor_y >= sc_bot) {
            con->cursor_y = sc_bot - 1;
            console_scroll_region_up(con, sc_top, sc_bot, cols);
            if (is_active && fb) {
                if (sc_top == 0 && sc_bot == rows) {
                    fb_scroll();
                } else {
                    for (r = sc_top; r < sc_bot; r++)
                        for (c2 = 0; c2 < cols; c2++) {
                            if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                            fb_putchar(con->buffer[r][c2], c2, r);
                        }
                }
                console_apply_colors(con, fb);
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
            if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
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

    if (con->cursor_y < con->buffer_rows && con->cursor_x < CONSOLE_BUFFER_COLS) {
        con->buffer[con->cursor_y][con->cursor_x] = c;
        if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
    }

    if (is_active && fb) {
        fb_putchar(c, con->cursor_x, con->cursor_y);
    }

    con->cursor_x++;
    if (con->cursor_x >= cols) {
        con->line_wrapped[con->cursor_y] = 1;
        con->cursor_x = 0;
        sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
        sc_top = con->scroll_top;
        con->cursor_y++;
        if (con->cursor_y >= sc_bot) {
            con->cursor_y = sc_bot - 1;
            console_scroll_region_up(con, sc_top, sc_bot, cols);
            if (is_active && fb) {
                if (sc_top == 0 && sc_bot == rows) {
                    fb_scroll();
                } else {
                    for (r = sc_top; r < sc_bot; r++)
                        for (c2 = 0; c2 < cols; c2++) {
                            if (con->color_buffer) console_apply_attr(con->color_buffer[r][c2], fb);
                            fb_putchar(con->buffer[r][c2], c2, r);
                        }
                }
                console_apply_colors(con, fb);
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
    uint64_t flags;

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
    size_t i;
    uint64_t head;
    uint64_t next_head;
    int skip_serial;
    int batch_started;
    size_t off;
    size_t chunk;
    uint64_t flags;
    uint64_t rows;
    uint64_t cols;
    int is_active;
    int fb_ok;
    char c;
    uint64_t tab_stop;
    uint64_t t;
    uint64_t sc_top;
    uint64_t sc_bot;
    uint64_t sr;
    uint64_t sc;
    console_t *con;
    framebuffer_t *fb;
    int use_async_writer;

    if (!console_initialized) {
        for (i = 0; i < size; i++) terminal_putchar(data[i]);
        return;
    }

    use_async_writer = writer_thread_running && size > 512;

    target_console = console_num;
    if (!console_valid_index(target_console)) {
        target_console = current_console;
    }

    if (!consoles[target_console].allocated) {
        console_ensure_alloc(target_console);
        if (!consoles[target_console].allocated) return;
    }

    con = &consoles[target_console];

    if (use_async_writer && console_ensure_write_buffer(con) != 0) {
        use_async_writer = 0;
    }

    if (console_num == 0 && !skip_serial_async && !use_async_writer) {
        if (kprint_is_ready()) {
            kprint_serial_async(data, size);
        } else {
            serial_write_direct(data, size);
        }
    }

    if (use_async_writer) {
        i = 0;
        while (i < size) {
            flags = console_irqsave();
            spin_lock(&console_lock);
            while (i < size) {
                head = con->write_head;
                next_head = (head + 1) % con->write_buffer_size;
                if (next_head == con->write_tail) {
                    break;
                }
                con->write_buffer[head] = data[i];
                con->write_flags[head] = skip_serial_async ? 1 : 0;
                con->write_head = next_head;
                i++;
            }
            con->dirty = 1;
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            if (i < size) {
                console_grow_write_buffer(con);
                head = con->write_head;
                next_head = (head + 1) % con->write_buffer_size;
                if (next_head == con->write_tail) {
                    if (writer_thread) {
                        wake_task(writer_thread);
                    }
                    yield();
                }
            }
            if (pending_console_switch >= 0 && console_interrupts_enabled()) {
                console_process_pending();
            }
        }
        if (writer_thread) {
            wake_task(writer_thread);
        }
        return;
    }
    
    skip_serial = (target_console == 0);

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
        is_active = (target_console == current_console && !console_switch_in_progress &&
                     !con->graphics_mode);
        rows = fb ? fb->rows : 25;
        cols = fb ? fb->cols : 80;
        if (rows == 0) rows = 25;
        if (cols == 0) cols = 80;
        if (!is_active && con->buffer_rows > 0) rows = con->buffer_rows;
        console_grow_buffer(con, rows);
        if (rows > con->buffer_rows) rows = con->buffer_rows;
        if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
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
                    continue;
                }
                if (c == '(' || c == ')') {
                    con->esc_state = 3;
                    continue;
                }
                if (c == 'M') {
                    sc_top = con->scroll_top;
                    sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                    if (con->cursor_y <= sc_top) {
                        console_scroll_region_down(con, sc_top, sc_bot, cols);
                        if (fb_ok) {
                            for (sr = sc_top; sr < sc_bot; sr++)
                                for (sc = 0; sc < cols; sc++) {
                                    if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                    fb_putchar(con->buffer[sr][sc], sc, sr);
                                }
                            console_apply_colors(con, fb);
                        }
                    } else {
                        con->cursor_y--;
                        if (fb_ok) { fb->cursor_y = con->cursor_y; }
                    }
                    con->esc_state = 0;
                    continue;
                }
                if (c == 'D') {
                    sc_top = con->scroll_top;
                    sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                    if (con->cursor_y + 1 >= sc_bot) {
                        console_scroll_region_up(con, sc_top, sc_bot, cols);
                        if (fb_ok) {
                            for (sr = sc_top; sr < sc_bot; sr++)
                                for (sc = 0; sc < cols; sc++) {
                                    if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                    fb_putchar(con->buffer[sr][sc], sc, sr);
                                }
                            console_apply_colors(con, fb);
                        }
                    } else {
                        con->cursor_y++;
                        if (fb_ok) { fb->cursor_y = con->cursor_y; }
                    }
                    con->esc_state = 0;
                    continue;
                }
                if (c == 'c') {
                    con->ansi_fg = 7;
                    con->ansi_bg = 0;
                    con->ansi_bold = 0;
                    con->ansi_reverse = 0;
                    con->scroll_top = 0;
                    con->scroll_bottom = 0;
                    con->cursor_visible = 1;
                    if (fb_ok) console_apply_colors(con, fb);
                    con->esc_state = 0;
                    continue;
                }
                if (c == '7') {
                    con->saved_cursor_x = con->cursor_x;
                    con->saved_cursor_y = con->cursor_y;
                    con->esc_state = 0;
                    continue;
                }
                if (c == '8') {
                    con->cursor_x = con->saved_cursor_x;
                    con->cursor_y = con->saved_cursor_y;
                    if (con->cursor_x >= cols) con->cursor_x = cols - 1;
                    if (con->cursor_y >= rows) con->cursor_y = rows - 1;
                    if (fb_ok) {
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                    }
                    con->esc_state = 0;
                    continue;
                }
                con->esc_state = 0;
                continue;
            }

            if (con->esc_state == 3) {
                con->esc_state = 0;
                continue;
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
                if (con->alt_screen_pending) {
                    chunk = i + 1;
                    break;
                }
                continue;
            }

            if (c == '\033') {
                con->esc_state = 1;
                con->esc_len = 0;
                continue;
            }

            if (c == 0x07 || c == 0x0E || c == 0x0F) {
                continue;
            }

            if (c == '\n') {
                con->cursor_x = 0;
                sc_top = con->scroll_top;
                sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                con->cursor_y++;
                if (con->cursor_y >= sc_bot) {
                    con->cursor_y = sc_bot - 1;
                    console_scroll_region_up(con, sc_top, sc_bot, cols);
                    batch_scroll_count++;
                    if (batch_scroll_count > (int)rows) {
                        batch_fb_skip = 1;
                        fb_ok = 0;
                    }
                    if (fb_ok) {
                        if (sc_top == 0 && sc_bot == rows) {
                            fb_scroll();
                        } else {
                            for (sr = sc_top; sr < sc_bot; sr++)
                                for (sc = 0; sc < cols; sc++) {
                                    if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                    fb_putchar(con->buffer[sr][sc], sc, sr);
                                }
                        }
                        console_apply_colors(con, fb);
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
                    if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
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
                    if (con->cursor_y < con->buffer_rows && con->cursor_x < CONSOLE_BUFFER_COLS) {
                        con->buffer[con->cursor_y][con->cursor_x] = ' ';
                        if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
                    }
                    if (fb_ok) {
                        fb_putchar(' ', con->cursor_x, con->cursor_y);
                    }
                    con->cursor_x++;
                    if (con->cursor_x >= cols) {
                        con->cursor_x = 0;
                        sc_top = con->scroll_top;
                        sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                        con->cursor_y++;
                        if (con->cursor_y >= sc_bot) {
                            con->cursor_y = sc_bot - 1;
                            console_scroll_region_up(con, sc_top, sc_bot, cols);
                            batch_scroll_count++;
                            if (batch_scroll_count > (int)rows) {
                                batch_fb_skip = 1;
                                fb_ok = 0;
                            }
                            if (fb_ok) {
                                if (sc_top == 0 && sc_bot == rows) {
                                    fb_scroll();
                                } else {
                                    for (sr = sc_top; sr < sc_bot; sr++)
                                        for (sc = 0; sc < cols; sc++) {
                                            if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                            fb_putchar(con->buffer[sr][sc], sc, sr);
                                        }
                                }
                                console_apply_colors(con, fb);
                            }
                        }
                    }
                }
                continue;
            }

            if (con->cursor_y < con->buffer_rows && con->cursor_x < CONSOLE_BUFFER_COLS) {
                con->buffer[con->cursor_y][con->cursor_x] = c;
                if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
            }

            if (fb_ok) {
                fb_putchar(c, con->cursor_x, con->cursor_y);
            }

            con->cursor_x++;
            if (con->cursor_x >= cols) {
                con->cursor_x = 0;
                sc_top = con->scroll_top;
                sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                con->cursor_y++;
                if (con->cursor_y >= sc_bot) {
                    con->cursor_y = sc_bot - 1;
                    console_scroll_region_up(con, sc_top, sc_bot, cols);
                    batch_scroll_count++;
                    if (batch_scroll_count > (int)rows) {
                        batch_fb_skip = 1;
                        fb_ok = 0;
                    }
                    if (fb_ok) {
                        if (sc_top == 0 && sc_bot == rows) {
                            fb_scroll();
                        } else {
                            for (sr = sc_top; sr < sc_bot; sr++)
                                for (sc = 0; sc < cols; sc++) {
                                    if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                    fb_putchar(con->buffer[sr][sc], sc, sr);
                                }
                        }
                        console_apply_colors(con, fb);
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

        if (con->alt_screen_pending)
            console_process_alt_screen_pending(target_console);

        off += chunk;
        if (pending_console_switch >= 0 && console_interrupts_enabled()) {
            console_process_pending();
        }
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
            if (fb && (fb->font || fb->rows > 0) && target_console == current_console &&
                !consoles[target_console].graphics_mode) {
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
    if (pending_console_switch >= 0 && console_interrupts_enabled()) {
        console_process_pending();
    }
}

void console_writestring(const char *data) {
    while (*data) {
        console_putchar(*data++);
    }
}

void console_clear(int console_num) {
    uint64_t flags;
    int row;
    int col;
    console_t *con;
    framebuffer_t *fb;

    if (!console_valid_index(console_num)) return;

    flags = console_irqsave();
    spin_lock(&console_lock);
    
    con = &consoles[console_num];
    if (con->allocated && con->buffer) {
        for (row = 0; row < (int)con->buffer_rows; row++) {
            for (col = 0; col < CONSOLE_BUFFER_COLS; col++) {
                con->buffer[row][col] = ' ';
                if (con->color_buffer) con->color_buffer[row][col] = 0x70;
            }
            con->line_wrapped[row] = 0;
        }
    }
    con->cursor_x = 0;
    con->cursor_y = 0;
    
    if (console_num == current_console && !con->graphics_mode) {
        fb = fb_get();
        if (fb) {
            fb_clear();
        }
    }

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

void console_setcursor(int console_num, int x, int y) {
    uint64_t flags;
    uint64_t cols;
    uint64_t rows;
    console_t *con;
    framebuffer_t *fb;

    if (!console_valid_index(console_num)) return;

    flags = console_irqsave();
    spin_lock(&console_lock);
    
    con = &consoles[console_num];
    fb = fb_get();
    cols = fb ? fb->cols : 80;
    rows = fb ? fb->rows : 25;
    
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint64_t)x >= cols) x = cols - 1;
    if ((uint64_t)y >= rows) y = rows - 1;
    
    con->cursor_x = (uint64_t)x;
    con->cursor_y = (uint64_t)y;
    
    if (console_num == current_console && fb && !con->graphics_mode) {
        fb->cursor_x = con->cursor_x;
        fb->cursor_y = con->cursor_y;
        fb_update_cursor();
    }

    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

int console_getcursor(int console_num, int *x, int *y) {
    uint64_t flags;
    console_t *con;

    if (!console_valid_index(console_num)) return -1;

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
    uint64_t chunk_rows;
    uint64_t visible_rows_local;
    uint64_t flags;
    int i;
    uint64_t tail;
    uint64_t head;
    uint64_t available;
    char chunk[256];
    uint8_t chunk_flags[256];
    uint64_t chunk_size;
    uint64_t j;
    uint64_t rows;
    uint64_t cols;
    int is_active;
    int wt_fb_ok;
    char c;
    uint64_t tab_stop;
    uint64_t wt;
    uint64_t sc_top;
    uint64_t sc_bot;
    uint64_t sr;
    uint64_t sc;
    uint64_t serial_start;
    uint64_t serial_len;
    int burst_fb_skip;
    int need_redraw;
    uint64_t burst_budget;
    console_t *con;
    framebuffer_t *fb;

    writer_thread_running = 1;
    while (1) {
        work_done = 0;
        pending_switch_requested = 0;
        burst_budget = 0;

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

        for (i = 0; i < console_count; i++) {
            con = &consoles[i];
            if (!con->allocated) continue;
            while (con->write_tail != con->write_head) {

                if (pending_console_switch >= 0) {
                    pending_switch_requested = 1;
                    goto handle_pending;
                }
                flags = console_irqsave();
                spin_lock(&console_lock);
                tail = con->write_tail;
                head = con->write_head;
                available = (head >= tail) ? (head - tail) : (con->write_buffer_size - tail + head);
                
                if (available == 0) {
                    spin_unlock(&console_lock);
                    console_irqrestore(flags);
                    break;
                }
                
                chunk_size = (available > 256) ? 256 : available;
                burst_fb_skip = (available > 1024) ? 1 : 0;
                
                for (j = 0; j < chunk_size; j++) {
                    chunk[j] = con->write_buffer[(tail + j) % con->write_buffer_size];
                    chunk_flags[j] = con->write_flags[(tail + j) % con->write_buffer_size];
                }

                if (i == 0) {
                    serial_start = 0;
                    for (j = 0; j <= chunk_size; j++) {
                        if (j == chunk_size || chunk_flags[j]) {
                            serial_len = j - serial_start;
                            if (serial_len > 0) {
                                if (kprint_is_ready()) {
                                    kprint_serial_async(chunk + serial_start, serial_len);
                                } else {
                                    serial_write_direct(chunk + serial_start, serial_len);
                                }
                            }
                            serial_start = j + 1;
                        }
                    }
                }
                
                console_batch++;
                
                fb = fb_get();
                rows = fb ? fb->rows : 25;
                cols = fb ? fb->cols : 80;
                if (rows == 0) rows = 25;
                if (cols == 0) cols = 80;
                console_grow_buffer(con, rows);
                if (rows > con->buffer_rows) rows = con->buffer_rows;
                if (cols > CONSOLE_BUFFER_COLS) cols = CONSOLE_BUFFER_COLS;
                is_active = (i == current_console && !console_switch_in_progress &&
                             !con->graphics_mode);
                wt_fb_ok = is_active && fb && !burst_fb_skip;
                for (j = 0; j < chunk_size; j++) {
                    c = chunk[j];
                    
                    if (con->esc_state == 1) {
                        if (c == '[') {
                            con->esc_state = 2;
                            con->esc_len = 0;
                        } else if (c == '(' || c == ')') {
                            con->esc_state = 3;
                        } else if (c == 'M') {
                            sc_top = con->scroll_top;
                            sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                            if (con->cursor_y <= sc_top) {
                                console_scroll_region_down(con, sc_top, sc_bot, cols);
                                if (wt_fb_ok) {
                                    for (sr = sc_top; sr < sc_bot; sr++)
                                        for (sc = 0; sc < cols; sc++) {
                                            if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                            fb_putchar(con->buffer[sr][sc], sc, sr);
                                        }
                                    console_apply_colors(con, fb);
                                }
                            } else {
                                con->cursor_y--;
                                if (wt_fb_ok) fb->cursor_y = con->cursor_y;
                            }
                            con->esc_state = 0;
                        } else if (c == 'D') {
                            sc_top = con->scroll_top;
                            sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                            if (con->cursor_y + 1 >= sc_bot) {
                                console_scroll_region_up(con, sc_top, sc_bot, cols);
                                if (wt_fb_ok) {
                                    if (sc_top == 0 && sc_bot == rows) {
                                        fb_scroll();
                                    } else {
                                        for (sr = sc_top; sr < sc_bot; sr++)
                                            for (sc = 0; sc < cols; sc++) {
                                                if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                                fb_putchar(con->buffer[sr][sc], sc, sr);
                                            }
                                    }
                                    console_apply_colors(con, fb);
                                }
                            } else {
                                con->cursor_y++;
                                if (wt_fb_ok) fb->cursor_y = con->cursor_y;
                            }
                            con->esc_state = 0;
                        } else if (c == 'c') {
                            con->ansi_fg = 7;
                            con->ansi_bg = 0;
                            con->ansi_bold = 0;
                            con->ansi_reverse = 0;
                            con->scroll_top = 0;
                            con->scroll_bottom = 0;
                            con->cursor_visible = 1;
                            if (wt_fb_ok) console_apply_colors(con, fb);
                            con->esc_state = 0;
                        } else if (c == '7') {
                            con->saved_cursor_x = con->cursor_x;
                            con->saved_cursor_y = con->cursor_y;
                            con->esc_state = 0;
                        } else if (c == '8') {
                            con->cursor_x = con->saved_cursor_x;
                            con->cursor_y = con->saved_cursor_y;
                            if (con->cursor_x >= cols) con->cursor_x = cols - 1;
                            if (con->cursor_y >= rows) con->cursor_y = rows - 1;
                            if (wt_fb_ok) {
                                fb->cursor_x = con->cursor_x;
                                fb->cursor_y = con->cursor_y;
                            }
                            con->esc_state = 0;
                        } else {
                            con->esc_state = 0;
                        }
                        continue;
                    }

                    if (con->esc_state == 3) {
                        con->esc_state = 0;
                        continue;
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
                        if (con->alt_screen_pending) {
                            chunk_size = j + 1;
                            break;
                        }
                        continue;
                    }
                    
                    if (c == '\033') {
                        con->esc_state = 1;
                        con->esc_len = 0;
                        continue;
                    }

                    if (c == 0x07 || c == 0x0E || c == 0x0F) {
                        continue;
                    }
                    
                    if (c == '\n') {
                        con->cursor_x = 0;
                        sc_top = con->scroll_top;
                        sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                        con->cursor_y++;
                        if (con->cursor_y >= sc_bot) {
                            con->cursor_y = sc_bot - 1;
                            console_scroll_region_up(con, sc_top, sc_bot, cols);
                            if (wt_fb_ok) {
                                if (sc_top == 0 && sc_bot == rows) {
                                    fb_scroll();
                                } else {
                                    for (sr = sc_top; sr < sc_bot; sr++)
                                        for (sc = 0; sc < cols; sc++) {
                                            if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                            fb_putchar(con->buffer[sr][sc], sc, sr);
                                        }
                                }
                                console_apply_colors(con, fb);
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
                            if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
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
                            if (con->cursor_y < con->buffer_rows && con->cursor_x < CONSOLE_BUFFER_COLS) {
                                con->buffer[con->cursor_y][con->cursor_x] = ' ';
                                if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
                            }
                            if (wt_fb_ok) fb_putchar(' ', con->cursor_x, con->cursor_y);
                            con->cursor_x++;
                            if (con->cursor_x >= cols) {
                                con->cursor_x = 0;
                                sc_top = con->scroll_top;
                                sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                                con->cursor_y++;
                                if (con->cursor_y >= sc_bot) {
                                    con->cursor_y = sc_bot - 1;
                                    console_scroll_region_up(con, sc_top, sc_bot, cols);
                                    if (wt_fb_ok) {
                                        if (sc_top == 0 && sc_bot == rows) {
                                            fb_scroll();
                                        } else {
                                            for (sr = sc_top; sr < sc_bot; sr++)
                                                for (sc = 0; sc < cols; sc++) {
                                                    if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                                    fb_putchar(con->buffer[sr][sc], sc, sr);
                                                }
                                        }
                                        console_apply_colors(con, fb);
                                    }
                                }
                            }
                        }
                        continue;
                    }
                    
                    if (con->cursor_y < con->buffer_rows && con->cursor_x < CONSOLE_BUFFER_COLS) {
                        con->buffer[con->cursor_y][con->cursor_x] = c;
                        if (con->color_buffer) con->color_buffer[con->cursor_y][con->cursor_x] = console_current_attr(con);
                    }
                    if (wt_fb_ok) fb_putchar(c, con->cursor_x, con->cursor_y);
                    
                    con->cursor_x++;
                    if (con->cursor_x >= cols) {
                        con->cursor_x = 0;
                        sc_top = con->scroll_top;
                        sc_bot = (con->scroll_bottom > 0) ? con->scroll_bottom : rows;
                        con->cursor_y++;
                        if (con->cursor_y >= sc_bot) {
                            con->cursor_y = sc_bot - 1;
                            console_scroll_region_up(con, sc_top, sc_bot, cols);
                            if (wt_fb_ok) {
                                if (sc_top == 0 && sc_bot == rows) {
                                    fb_scroll();
                                } else {
                                    for (sr = sc_top; sr < sc_bot; sr++)
                                        for (sc = 0; sc < cols; sc++) {
                                            if (con->color_buffer) console_apply_attr(con->color_buffer[sr][sc], fb);
                                            fb_putchar(con->buffer[sr][sc], sc, sr);
                                        }
                                }
                                console_apply_colors(con, fb);
                            }
                        }
                    }
                    
                    if (wt_fb_ok) {
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                    }
                }
                
                con->write_tail = (tail + chunk_size) % con->write_buffer_size;
                if (burst_fb_skip && is_active) {
                    con->dirty = 2;
                } else if (con->write_tail == con->write_head) {
                    con->dirty = 0;
                }
                
                console_batch--;
                if (console_batch == 0 && is_active && fb) {
                    if (!burst_fb_skip) {
                        fb->cursor_x = con->cursor_x;
                        fb->cursor_y = con->cursor_y;
                        fb_update_cursor();
                    }
                }

                need_redraw = (is_active && con->dirty == 2 && con->write_tail == con->write_head) ? 1 : 0;
                
                spin_unlock(&console_lock);
                console_irqrestore(flags);

                if (con->alt_screen_pending)
                    console_process_alt_screen_pending(i);

                if (need_redraw) {
                    console_redraw_sync(i);
                    flags = console_irqsave();
                    spin_lock(&console_lock);
                    if (con->write_tail == con->write_head && con->dirty == 2) {
                        con->dirty = 0;
                    }
                    spin_unlock(&console_lock);
                    console_irqrestore(flags);
                } else if (!burst_fb_skip) {
                    fb_flush();
                }
                work_done = 1;
                if (pending_console_switch >= 0) {
                    pending_switch_requested = 1;
                    goto handle_pending;
                }
                if (burst_fb_skip) {
                    burst_budget += chunk_size;
                    if (burst_budget >= 1024) {
                        burst_budget = 0;
                        sleep_ms(1);
                    } else {
                        yield();
                    }
                } else {
                    yield();
                }
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
                sleep_ms(1);
            } else {
                yield();
            }
        } else {
            yield();
        }
    }
}

void console_writer_init(void) {
    extern void lock_scheduler(void);
    extern void unlock_scheduler(void);
    extern void add_task_to_runqueue(task_t* new_task);
    if (writer_thread_running) return;
    
    extern task_t* create_kernel_task(void (*entry)(void), task_state_t initial_state);
    
    writer_thread = create_kernel_task(console_writer_thread, TASK_READY);
    if (writer_thread) {
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
        for (i = 0; i < console_count; i++) {
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

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

static console_t *consoles;
static console_t console_fallback[1] KERNEL_INIT_OPTIONAL_BSS;
static int console_fallback_active = 1;
static int console_count = 0;
static int current_console = 0;
static int console_initialized = 0;
static int console_batch = 0;
static spinlock_t console_lock = {0};
static volatile int console_switching = 0;
static volatile int console_switch_in_progress = 0;
static volatile int pending_console_switch = -1;

static volatile int console_redraw_pending = 0;

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

static uint64_t console_calc_cols(void) {
    uint64_t cols;
    framebuffer_t *fb;

    fb = fb_get();
    cols = fb ? fb->cols : 80;
    if (cols == 0) cols = 80;
    return cols;
}

static char *console_char_row(console_t *con, uint64_t row) {
    return con->buffer + row * con->buffer_cols;
}

static uint8_t *console_color_row(console_t *con, uint64_t row) {
    return con->color_buffer + row * con->buffer_cols;
}

static int console_runtime_count(void) {
    int count;

    count = cmdline_get_consoles();
    if (count <= 0) count = 1;
    return count;
}

int console_get_count(void) {
    int count;

    count = console_count;
    if (count <= 0) count = console_runtime_count();
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
        console_fallback_active = 1;
        count = 1;
    } else {
        console_fallback_active = 0;
    }
    console_count = count;
    memset(consoles, 0, console_count * sizeof(console_t));
    return 0;
}

int console_fallback_reclaimable(void) {
    return !console_fallback_active;
}

static char *console_alloc_storage(uint64_t cells, uint64_t rows,
                                   uint8_t **wrapped) {
    char *buffer;

    buffer = (char *)slab_page_alloc(cells);
    if (!buffer) return NULL;
    *wrapped = (uint8_t *)kmalloc(rows);
    if (!*wrapped) {
        slab_page_free(buffer, cells);
        return NULL;
    }
    return buffer;
}

static void console_free_storage(char *buffer, uint64_t cells,
                                 uint8_t *wrapped) {
    slab_page_free(buffer, cells);
    kfree(wrapped);
}

static int console_ensure_alloc(int n) {
    uint64_t rows;
    uint64_t cols;
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
    cols = console_calc_cols();
    if (rows > SIZE_MAX / cols) return -1;
    con->buffer_rows = rows;
    con->buffer_cols = cols;
    con->buffer = console_alloc_storage(rows * cols, rows,
                                        &con->line_wrapped);
    if (!con->buffer) return -1;
    memset(con->buffer, ' ', rows * cols);
    memset(con->line_wrapped, 0, rows);
    con->allocated = 1;
    return 0;
}

static uint8_t console_current_attr(console_t *con);

#define CONSOLE_COLOR_PALETTE 1
#define CONSOLE_COLOR_RUNS 2

static void console_free_color(uint8_t *buffer, uint64_t cells,
                               uint8_t packed) {
    if (!buffer) return;
    if (packed)
        kfree(buffer);
    else
        slab_page_free(buffer, cells);
}

static uint16_t console_color_run_end(const uint8_t *packed,
                                      uint64_t run) {
    uint16_t end;

    memcpy(&end, packed + run * 3, sizeof(end));
    return end;
}

static void console_color_run_set(uint8_t *packed, uint64_t run,
                                  uint16_t end, uint8_t attr) {
    memcpy(packed + run * 3, &end, sizeof(end));
    packed[run * 3 + 2] = attr;
}

static uint8_t console_color_at(console_t *con, uint64_t row, uint64_t col) {
    uint64_t cell;
    uint64_t low;
    uint64_t high;
    uint64_t middle;
    uint8_t *packed;
    uint8_t index;

    if (!con || !con->color_buffer) return 0x70;
    if (!con->color_packed) return console_color_row(con, row)[col];
    cell = row * con->buffer_cols + col;
    packed = (uint8_t *)(void *)con->color_buffer;
    if (con->color_packed == CONSOLE_COLOR_RUNS) {
        low = 0;
        high = con->color_run_count;
        while (low < high) {
            middle = low + (high - low) / 2;
            if (cell < console_color_run_end(packed, middle))
                high = middle;
            else
                low = middle + 1;
        }
        if (low >= con->color_run_count) return 0x70;
        return packed[low * 3 + 2];
    }
    index = packed[16 + cell / 2];
    if (cell & 1) index >>= 4;
    else index &= 0x0F;
    if (index >= con->color_run_count) return 0x70;
    return packed[index];
}

static int console_expand_color_buffer(console_t *con) {
    uint8_t *packed;
    uint8_t *full;
    uint64_t cells;
    uint8_t attr;
    uint64_t i;
    uint64_t run;
    uint64_t start;
    uint64_t end;

    if (!con || !con->color_buffer || !con->color_packed) return 0;
    cells = con->buffer_rows * con->buffer_cols;
    packed = (uint8_t *)(void *)con->color_buffer;
    full = (uint8_t *)slab_page_alloc(cells);
    if (!full) return -1;
    if (con->color_packed == CONSOLE_COLOR_RUNS) {
        start = 0;
        for (run = 0; run < con->color_run_count; run++) {
            end = console_color_run_end(packed, run);
            if (end <= start || end > cells) {
                slab_page_free(full, cells);
                return -1;
            }
            memset(full + start, packed[run * 3 + 2], end - start);
            start = end;
        }
        if (start != cells) {
            slab_page_free(full, cells);
            return -1;
        }
    } else {
        for (i = 0; i < cells; i++) {
            attr = packed[16 + i / 2];
            if (i & 1) attr >>= 4;
            else attr &= 0x0F;
            full[i] = attr < con->color_run_count ? packed[attr] : 0x70;
        }
    }
    kfree(packed);
    con->color_buffer = full;
    con->color_run_count = 0;
    con->color_packed = 0;
    return 0;
}

static void console_pack_color_buffer(console_t *con) {
    uint8_t *full;
    uint8_t *packed;
    uint64_t cells;
    uint64_t i;
    uint64_t bytes;
    uint64_t palette_bytes;
    uint64_t run_bytes;
    uint64_t runs;
    uint64_t run;
    uint64_t palette_count;
    uint64_t palette_index;
    uint8_t attr;
    uint8_t palette[16];
    uint8_t run_attr;

    if (!con || !con->color_buffer || con->color_packed) return;
    cells = con->buffer_rows * con->buffer_cols;
    if (cells == 0) return;
    full = (uint8_t *)(void *)con->color_buffer;
    palette_count = 0;
    for (i = 0; i < cells; i++) {
        attr = full[i];
        for (palette_index = 0; palette_index < palette_count;
             palette_index++) {
            if (palette[palette_index] == attr) break;
        }
        if (palette_index == palette_count) {
            if (palette_count == 16) break;
            palette[palette_count++] = attr;
        }
    }
    palette_bytes = i == cells ? 16 + (cells + 1) / 2 : UINT64_MAX;
    runs = 1;
    for (i = 1; i < cells; i++) {
        if (full[i] != full[i - 1]) runs++;
    }
    run_bytes = cells <= UINT16_MAX ? runs * 3 : UINT64_MAX;
    bytes = palette_bytes < run_bytes ? palette_bytes : run_bytes;
    if (bytes >= cells) return;
    packed = (uint8_t *)kmalloc(bytes);
    if (!packed) return;
    if (palette_bytes <= run_bytes) {
        memset(packed, 0, bytes);
        memcpy(packed, palette, palette_count);
        for (i = 0; i < cells; i++) {
            attr = full[i];
            for (palette_index = 0; palette_index < palette_count;
                 palette_index++) {
                if (palette[palette_index] == attr) break;
            }
            if (i & 1)
                packed[16 + i / 2] |=
                    (uint8_t)((uint8_t)palette_index << 4);
            else
                packed[16 + i / 2] = (uint8_t)palette_index;
        }
        con->color_run_count = palette_count;
        con->color_packed = CONSOLE_COLOR_PALETTE;
    } else {
        run = 0;
        run_attr = full[0];
        for (i = 1; i <= cells; i++) {
            if (i < cells && full[i] == run_attr) continue;
            console_color_run_set(packed, run, (uint16_t)i, run_attr);
            run++;
            if (i < cells) run_attr = full[i];
        }
        con->color_run_count = runs;
        con->color_packed = CONSOLE_COLOR_RUNS;
    }
    slab_page_free(full, cells);
    con->color_buffer = packed;
}

static int console_ensure_color_buffer(console_t *con) {
    uint8_t *new_color;
    uint64_t cells;

    if (!con || !con->allocated) return -1;
    if (con->color_buffer) return console_expand_color_buffer(con);
    if (con->buffer_rows == 0) return -1;
    cells = con->buffer_rows * con->buffer_cols;
    new_color = (uint8_t *)slab_page_alloc(cells);
    if (!new_color) return -1;
    memset(new_color, 0x70, cells);
    con->color_buffer = new_color;
    con->color_run_count = 0;
    con->color_packed = 0;
    return 0;
}

static void console_ensure_nondefault_color(console_t *con) {
    if (!con) return;
    if (console_current_attr(con) != 0x70) {
        console_ensure_color_buffer(con);
    }
    if (con->color_packed) console_expand_color_buffer(con);
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
    uint64_t cells;
    uint8_t *new_wrapped;
    uint64_t flags;
    char *new_buf;
    uint8_t *new_color;

    flags = console_irqsave();
    spin_lock(&console_lock);
    if (con->alt_screen_active) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    if (console_expand_color_buffer(con) < 0) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    rows = con->buffer_rows;
    if (!rows || !con->buffer) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    cells = rows * con->buffer_cols;

    new_buf = console_alloc_storage(cells, rows, &new_wrapped);
    new_color = NULL;
    if (con->color_buffer) {
        new_color = (uint8_t *)slab_page_alloc(cells);
    }
    if (!new_buf || (con->color_buffer && !new_color)) {
        if (new_buf) console_free_storage(new_buf, cells, new_wrapped);
        if (new_color) slab_page_free(new_color, cells);
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    memset(new_buf, ' ', cells);
    if (new_color) memset(new_color, 0x70, cells);
    memset(new_wrapped, 0, rows);

    con->alt_saved_buffer = con->buffer;
    con->alt_saved_color = con->color_buffer;
    con->alt_saved_wrapped = con->line_wrapped;
    con->alt_saved_rows = rows;
    con->alt_saved_cols = con->buffer_cols;
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
    uint64_t flags;
    uint64_t old_cells;
    uint8_t *old_color;
    uint8_t *old_wrapped;
    char *old_buf;

    flags = console_irqsave();
    spin_lock(&console_lock);
    if (!con->alt_screen_active || !con->alt_saved_buffer) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    if (console_expand_color_buffer(con) < 0) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }

    old_buf = con->buffer;
    old_color = con->color_buffer;
    old_wrapped = con->line_wrapped;
    old_cells = con->buffer_rows * con->buffer_cols;

    con->buffer = con->alt_saved_buffer;
    con->color_buffer = con->alt_saved_color;
    con->line_wrapped = con->alt_saved_wrapped;
    con->buffer_rows = con->alt_saved_rows;
    con->buffer_cols = con->alt_saved_cols;
    con->cursor_x = con->alt_saved_cx;
    con->cursor_y = con->alt_saved_cy;
    con->scroll_offset = con->alt_saved_scroll;

    con->alt_saved_buffer = NULL;
    con->alt_saved_color = NULL;
    con->alt_saved_wrapped = NULL;
    con->alt_screen_active = 0;

    spin_unlock(&console_lock);
    console_irqrestore(flags);

    console_free_storage(old_buf, old_cells, old_wrapped);
    slab_page_free(old_color, old_cells);
}

static void console_process_alt_screen_pending(int console_num) {
    int pending;
    console_t *con;
    uint64_t flags;

    con = &consoles[console_num];
    flags = console_irqsave();
    spin_lock(&console_lock);
    pending = con->alt_screen_pending;
    if (pending == 0) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return;
    }
    con->alt_screen_pending = 0;
    spin_unlock(&console_lock);
    console_irqrestore(flags);

    if (pending == 1) {
        console_enter_alt_screen(con);
    } else if (pending == -1) {
        console_leave_alt_screen(con);
    }

    flags = console_irqsave();
    spin_lock(&console_lock);
    if (console_num == current_console && !con->graphics_mode) {
        console_fast_redraw_locked(console_num);
    }
    spin_unlock(&console_lock);
    console_irqrestore(flags);
}

static void console_grow_buffer(console_t *con, uint64_t needed_rows) {
    uint8_t *new_wrapped;
    uint64_t old_rows;
    uint64_t old_cols;
    uint64_t needed_cols;
    uint64_t target_rows;
    uint64_t copy_rows;
    uint64_t copy_cols;
    uint64_t row;
    char *new_buf;
    uint8_t *new_color;
    char *old_buf;
    uint8_t *old_wrapped;

    if (!con->allocated) return;
    if (console_expand_color_buffer(con) < 0) return;
    old_rows = con->buffer_rows;
    target_rows = needed_rows;
    if (target_rows < old_rows) target_rows = old_rows;
    needed_cols = console_calc_cols();
    if (needed_cols < con->buffer_cols) needed_cols = con->buffer_cols;
    if (target_rows <= con->buffer_rows && needed_cols <= con->buffer_cols) return;
    if (needed_cols == 0 || target_rows > SIZE_MAX / needed_cols) return;
    old_cols = con->buffer_cols;
    new_buf = console_alloc_storage(target_rows * needed_cols, target_rows,
                                    &new_wrapped);
    if (!new_buf) return;
    new_color = NULL;
    if (con->color_buffer) {
        new_color = (uint8_t *)slab_page_alloc(target_rows * needed_cols);
        if (!new_color) {
            console_free_storage(new_buf, target_rows * needed_cols,
                                 new_wrapped);
            return;
        }
    }
    memset(new_buf, ' ', target_rows * needed_cols);
    if (new_color) memset(new_color, 0x70, target_rows * needed_cols);
    memset(new_wrapped, 0, target_rows);
    copy_rows = old_rows;
    if (copy_rows > target_rows) copy_rows = target_rows;
    copy_cols = old_cols;
    if (copy_cols > needed_cols) copy_cols = needed_cols;
    if (con->buffer) {
        for (row = 0; row < copy_rows; row++) {
            memcpy(new_buf + row * needed_cols,
                   con->buffer + row * old_cols, copy_cols);
        }
    }
    if (con->color_buffer) {
        for (row = 0; row < copy_rows; row++) {
            memcpy(new_color + row * needed_cols,
                   con->color_buffer + row * old_cols, copy_cols);
        }
        slab_page_free(con->color_buffer, old_rows * old_cols);
    }
    if (con->line_wrapped) {
        memcpy(new_wrapped, con->line_wrapped, copy_rows);
    }
    old_buf = con->buffer;
    old_wrapped = con->line_wrapped;
    con->buffer = new_buf;
    con->color_buffer = new_color;
    con->line_wrapped = new_wrapped;
    con->buffer_rows = target_rows;
    con->buffer_cols = needed_cols;
    console_free_storage(old_buf, old_rows * old_cols, old_wrapped);
}

static void console_reclaim_default_color(console_t *con) {
    uint64_t rows;
    uint64_t cols;
    uint64_t r;
    uint64_t c;

    if (!con || !con->color_buffer) return;
    rows = con->buffer_rows;
    cols = con->buffer_cols;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            if (console_color_at(con, r, c) != 0x70) return;
        }
    }
    console_free_color(con->color_buffer, rows * cols,
                       con->color_packed);
    con->color_buffer = NULL;
    con->color_run_count = 0;
    con->color_packed = 0;
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
        console_pack_color_buffer(con);
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
                sz += con->buffer_rows * con->buffer_cols +
                      con->buffer_rows;
            }
            if (con->color_buffer) {
                b++;
                if (con->color_packed == CONSOLE_COLOR_RUNS)
                    sz += con->color_run_count * 3;
                else if (con->color_packed)
                    sz += 16 +
                          (con->buffer_rows * con->buffer_cols + 1) / 2;
                else
                    sz += con->buffer_rows * con->buffer_cols;
            }
        }
        spin_unlock(&console_lock);
        console_irqrestore(flags);
    }
    if (buffers) *buffers = b;
    if (bytes) *bytes = sz;
}

int console_get_cell(int console_num, uint64_t x, uint64_t y, char *ch, uint8_t *attr) {
    console_t *con;

    if (!console_valid_index(console_num)) return -1;
    con = &consoles[console_num];
    if (!con->allocated || !con->buffer) return -1;
    if (x >= con->buffer_cols || y >= con->buffer_rows) return -1;
    if (ch) *ch = console_char_row(con, y)[x];
    if (attr) {
        *attr = console_color_at(con, y, x);
    }
    return 0;
}

static uint64_t console_redraw_cursor_x = 0;
static uint64_t console_redraw_cursor_y = 0;
static uint64_t console_redraw_visible_rows = 0;
static uint64_t console_redraw_visible_cols = 0;
static uint64_t console_redraw_row = 0;
static int console_redraw_console = 0;

static int batch_scroll_count = 0;
static int batch_fb_skip = 0;

uint64_t console_ansi_color(uint8_t idx, int bright) {
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
    if (cols > con->buffer_cols) cols = con->buffer_cols;

    prev_attr = 0xFF;
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            c = console_char_row(con, row)[col];
            attr = console_color_at(con, row, col);
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
    uint64_t flags;
    uint64_t visible_rows;
    uint64_t visible_cols;
    framebuffer_t *fb = fb_get();
    console_t *con;

    if (!fb || (fb->rows == 0 && fb->cols == 0)) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        return;
    }

    rows = fb->rows;
    cols = fb->cols;
    if (rows == 0 || cols == 0) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        return;
    }

    visible_rows = rows;
    flags = console_irqsave();
    spin_lock(&console_lock);

    if (!console_valid_index(console_num)) {
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
        console_switching = 0;
        return;
    }

    if (console_redraw_pending) {
        console_redraw_pending = 0;
        console_switch_in_progress = 0;
    }

    con = &consoles[console_num];
    visible_cols = cols < con->buffer_cols ? cols : con->buffer_cols;
    console_redraw_cursor_x = con->cursor_x;
    console_redraw_cursor_y = con->cursor_y;

    if (con->allocated && visible_rows > con->buffer_rows) visible_rows = con->buffer_rows;

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
    char row_chars_inline[80];
    uint8_t row_attrs_inline[80];
    char *row_chars;
    uint8_t *row_attrs;
    uint64_t row_capacity;
    framebuffer_t *fb = fb_get();
    console_t *con;
    int redraw_console_cached;

    if (!console_redraw_pending) return;
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
    redraw_console_cached = console_redraw_console;
    fb_rows = fb->rows;
    spin_unlock(&console_lock);
    console_irqrestore(flags);

    row_capacity = visible_cols_cached;
    if (row_capacity < fb->cols) row_capacity = fb->cols;
    row_chars = row_chars_inline;
    row_attrs = row_attrs_inline;
    if (row_capacity > sizeof(row_chars_inline)) {
        row_chars = (char *)kmalloc(row_capacity);
        row_attrs = (uint8_t *)kmalloc(row_capacity);
        if (!row_chars || !row_attrs) {
            if (row_chars && row_chars != row_chars_inline) kfree(row_chars);
            if (row_attrs && row_attrs != row_attrs_inline) kfree(row_attrs);
            row_chars = row_chars_inline;
            row_attrs = row_attrs_inline;
            row_capacity = sizeof(row_chars_inline);
        }
    }

    end_row = current_row_cached + max_rows;
    if (end_row > fb_rows) {
        end_row = fb_rows;
    }

    prev_attr = 0xFF;
    rows_processed = 0;
    for (row = current_row_cached; row < end_row; row++) {
        flags = console_irqsave();
        spin_lock(&console_lock);
        con = console_valid_index(redraw_console_cached) ?
              &consoles[redraw_console_cached] : NULL;
        for (col = 0; col < row_capacity; col++) {
            if (con && con->allocated && con->buffer &&
                row < visible_rows_cached && col < visible_cols_cached &&
                row < con->buffer_rows) {
                row_chars[col] = console_char_row(con, row)[col];
                row_attrs[col] = console_color_at(con, row, col);
            } else {
                row_chars[col] = ' ';
                row_attrs[col] = 0x70;
            }
        }
        spin_unlock(&console_lock);
        console_irqrestore(flags);

        cols = fb->cols;
        for (col = 0; col < cols; col++) {
            if (col < row_capacity) {
                c = row_chars[col];
                attr = row_attrs[col];
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
    if (row_chars != row_chars_inline) kfree(row_chars);
    if (row_attrs != row_attrs_inline) kfree(row_attrs);
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
    for (i = 0; i < console_count; i++) {
        con = &consoles[i];
        if (!con->allocated) continue;
        console_expand_color_buffer(con);
        if (i == current_console && max_cols > con->buffer_cols)
            console_grow_buffer(con, max_rows);
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
                memmove(console_char_row(con, 0), console_char_row(con, shift),
                        (con->buffer_rows - shift) * con->buffer_cols);
                if (con->color_buffer)
                    memmove(console_color_row(con, 0),
                            console_color_row(con, shift),
                            (con->buffer_rows - shift) * con->buffer_cols);
                if (con->line_wrapped)
                    memmove(con->line_wrapped, con->line_wrapped + shift, con->buffer_rows - shift);
                for (r = con->buffer_rows - shift; r < con->buffer_rows; r++) {
                    memset(console_char_row(con, r), ' ', con->buffer_cols);
                    if (con->color_buffer)
                        memset(console_color_row(con, r), 0x70,
                               con->buffer_cols);
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
    uint64_t shift;
    uint64_t r;
    int cursor_found;
    uint64_t total_chars_before_cursor;
    uint64_t chars_counted;
    uint64_t new_cursor_chars;
    uint64_t buf_rows;
    uint64_t old_stride;
    int have_colors;
    char *new_buf;
    uint8_t *new_color_buf;
    uint8_t *old_color_buffer;
    uint8_t *old_wrapped;
    char *old_buffer;
    uint64_t old_cells;

    if (!con->allocated || !con->buffer) return;
    if (old_cols == 0) old_cols = 1;
    if (new_cols == 0) new_cols = 1;
    if (new_rows == 0) new_rows = 1;
    old_stride = con->buffer_cols;
    if (old_cols > old_stride) old_cols = old_stride;

    if (console_expand_color_buffer(con) < 0) return;
    console_grow_buffer(con, new_rows);
    buf_rows = con->buffer_rows;
    have_colors = (con->color_buffer != NULL);

    if (buf_rows > SIZE_MAX / new_cols) return;
    if (buf_rows > SIZE_MAX / old_cols) return;
    linebuf_cap = buf_rows * old_cols;
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

    new_buf = console_alloc_storage(buf_rows * new_cols, buf_rows,
                                    &new_wrapped);
    if (!new_buf) {
        kfree(colorbuf);
        kfree(linebuf);
        return;
    }

    new_color_buf = NULL;
    if (have_colors) {
        new_color_buf = (uint8_t *)slab_page_alloc(buf_rows * new_cols);
        if (!new_color_buf) {
            console_free_storage(new_buf, buf_rows * new_cols,
                                 new_wrapped);
            kfree(colorbuf);
            kfree(linebuf);
            return;
        }
    }

    memset(new_buf, ' ', buf_rows * new_cols);
    if (new_color_buf)
        memset(new_color_buf, 0x70, buf_rows * new_cols);
    memset(new_wrapped, 0, buf_rows);

    total_chars_before_cursor = 0;
    cursor_found = 0;
    src_row = 0;

    for (src_row = 0; src_row < buf_rows; src_row++) {
        if (src_row < con->cursor_y) {
            row_end = old_cols;
            while (row_end > 0 && console_char_row(con, src_row)[row_end - 1] == ' ') row_end--;
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
            while (row_end > 0 && console_char_row(con, src_row)[row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = old_cols;

            for (col = 0; col < row_end && linebuf_len < linebuf_cap; col++) {
                linebuf[linebuf_len] = console_char_row(con, src_row)[col];
                if (colorbuf)
                    colorbuf[linebuf_len] = console_color_row(con, src_row)[col];
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
                    memmove(new_buf, new_buf + new_cols,
                            (buf_rows - 1) * new_cols);
                    if (new_color_buf)
                        memmove(new_color_buf, new_color_buf + new_cols,
                                (buf_rows - 1) * new_cols);
                    memmove(new_wrapped, new_wrapped + 1, buf_rows - 1);
                    memset(new_buf + (buf_rows - 1) * new_cols, ' ',
                           new_cols);
                    if (new_color_buf)
                        memset(new_color_buf + (buf_rows - 1) * new_cols,
                               0x70, new_cols);
                    new_wrapped[buf_rows - 1] = 0;
                    out_row = buf_rows - 1;
                }

                chars_left = linebuf_len - lpos;
                chunk = (chars_left > new_cols) ? new_cols : chars_left;

                for (col = 0; col < chunk; col++) {
                    new_buf[out_row * new_cols + col] = linebuf[lpos + col];
                    if (new_color_buf && colorbuf)
                        new_color_buf[out_row * new_cols + col] =
                            colorbuf[lpos + col];
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

    old_buffer = con->buffer;
    old_color_buffer = con->color_buffer;
    old_wrapped = con->line_wrapped;
    old_cells = con->buffer_rows * con->buffer_cols;
    con->buffer = new_buf;
    con->color_buffer = new_color_buf;
    con->line_wrapped = new_wrapped;
    con->buffer_cols = new_cols;
    new_buf = NULL;
    new_color_buf = NULL;
    new_wrapped = NULL;
    console_free_storage(old_buffer, old_cells, old_wrapped);
    slab_page_free(old_color_buffer, old_cells);

    if (cursor_found) {
        chars_counted = 0;
        con->cursor_x = 0;
        con->cursor_y = 0;
        new_cursor_chars = total_chars_before_cursor;

        for (src_row = 0; src_row < buf_rows; src_row++) {
            row_end = new_cols;
            while (row_end > 0 && console_char_row(con, src_row)[row_end - 1] == ' ') row_end--;
            if (con->line_wrapped[src_row]) row_end = new_cols;

            if (chars_counted + row_end >= new_cursor_chars) {
                con->cursor_y = src_row;
                con->cursor_x = new_cursor_chars - chars_counted;
                if (con->cursor_x >= new_cols) con->cursor_x = new_cols - 1;
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
        shift = con->cursor_y - (new_rows - 1);
        if (shift > buf_rows) shift = buf_rows;
        memmove(con->buffer, con->buffer + shift * con->buffer_cols,
                (buf_rows - shift) * con->buffer_cols);
        if (con->color_buffer)
            memmove(con->color_buffer,
                    con->color_buffer + shift * con->buffer_cols,
                    (buf_rows - shift) * con->buffer_cols);
        memmove(con->line_wrapped, con->line_wrapped + shift,
                (buf_rows - shift) * sizeof(con->line_wrapped[0]));
        for (r = buf_rows - shift; r < buf_rows; r++) {
            memset(console_char_row(con, r), ' ', con->buffer_cols);
            if (con->color_buffer)
                memset(console_color_row(con, r), 0x70,
                       con->buffer_cols);
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

    if (new_color_buf)
        slab_page_free(new_color_buf, buf_rows * new_cols);
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

void KERNEL_EARLY_INIT console_init(void) {
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
        con->color_run_count = 0;
        con->color_packed = 0;
        con->line_wrapped = NULL;
        con->buffer_rows = 0;
        con->buffer_cols = 0;
        con->allocated = 0;
        con->cursor_x = 0;
        con->cursor_y = 0;
        con->scroll_offset = 0;
        con->esc_state = 0;
        con->esc_len = 0;
        con->esc.inline_data[0] = '\0';
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
        con->alt_saved_cols = 0;
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
    __atomic_store_n(&pending_console_switch, -1, __ATOMIC_RELEASE);
    console_redraw_pending = 0;
    console_batch = 0;
    
    console_initialized = 1;
}

void KERNEL_EARLY_INIT console_reinit(void) {
    console_initialized = 0;
    console_init();
}

void console_redraw_current(void) {
    if (!console_initialized) return;
    if (console_switch_in_progress) return;
    if (console_get_graphics_mode(current_console)) return;
    console_redraw_sync(current_console);
}

void console_force_redraw(void) {
    if (!console_initialized) return;
    if (console_get_graphics_mode(current_console)) return;
    console_switch_in_progress = 0;
    console_redraw_pending = 0;
    console_redraw_sync(current_console);
}

static int console_switch_internal_impl(int console_num, int from_interrupt,
                                        int defer_on_busy) {
    uint64_t rows;
    uint64_t cols;
    uint64_t flags;
    int lock_acquired;
    uint64_t new_cx;
    uint64_t new_cy;
    framebuffer_t *fb;
    console_t *new_con;
    
    if (!console_valid_index(console_num)) return -1;
    if (!console_initialized) return -1;
    if (console_num == current_console) return 0;

    if (console_switching) {
        if (defer_on_busy)
            __atomic_store_n(&pending_console_switch, console_num,
                             __ATOMIC_RELEASE);
        return 1;
    }

    flags = console_irqsave();
    lock_acquired = spin_trylock(&console_lock);
    if (!lock_acquired) {
        console_irqrestore(flags);
        if (defer_on_busy)
            __atomic_store_n(&pending_console_switch, console_num,
                             __ATOMIC_RELEASE);
        return 1;
    }
    
    console_switching = 1;
    console_switch_in_progress = 1;
    
    if (!consoles[console_num].allocated) {
        if (from_interrupt) {
            console_switching = 0;
            console_switch_in_progress = 0;
            __atomic_store_n(&pending_console_switch, console_num,
                             __ATOMIC_RELEASE);
            spin_unlock(&console_lock);
            console_irqrestore(flags);
            return 1;
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
            return -1;
        }
    }
    
    fb = fb_get();
    if (fb) {
        consoles[current_console].cursor_x = fb->cursor_x;
        consoles[current_console].cursor_y = fb->cursor_y;
    }
    
    new_con = &consoles[console_num];
    console_expand_color_buffer(new_con);
    
    rows = fb ? fb->rows : 25;
    cols = fb ? fb->cols : 80;
    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (from_interrupt &&
        (new_con->buffer_rows < rows || new_con->buffer_cols < cols)) {
        console_switching = 0;
        console_switch_in_progress = 0;
        __atomic_store_n(&pending_console_switch, console_num,
                         __ATOMIC_RELEASE);
        spin_unlock(&console_lock);
        console_irqrestore(flags);
        return 1;
    }
    console_grow_buffer(new_con, rows);
    if (cols > new_con->buffer_cols) cols = new_con->buffer_cols;
    
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
        return 0;
    }

    console_redraw_prepare(current_console);
    if (!from_interrupt) console_redraw_sync(current_console);
    return 0;
}

static void console_switch_internal(int console_num) {
    if (!tty_vt_switch_request(console_num)) return;
    console_switch_internal_impl(console_num, 0, 1);
    if (console_num == current_console)
        tty_vt_switch_complete(console_num);
}

void console_switch(int console_num) {
    if (!console_valid_index(console_num)) return;
    if (!console_initialized) return;
    if (console_num == current_console) return;

    if (!console_interrupts_enabled()) {
        if (console_switching) {
            __atomic_store_n(&pending_console_switch, console_num,
                             __ATOMIC_RELEASE);
            return;
        }
        __atomic_store_n(&pending_console_switch, console_num,
                         __ATOMIC_RELEASE);
        return;
    }
    
    console_switch_internal(console_num);
}

int console_switch_committed(int console_num) {
    uint64_t flags;
    int result;

    if (!console_valid_index(console_num)) return -1;
    if (!console_initialized) return -1;
    if (console_num == current_console) return 0;

    flags = console_irqsave();
    __atomic_store_n(&pending_console_switch, -1, __ATOMIC_RELEASE);
    console_irqrestore(flags);
    for (;;) {
        result = console_switch_internal_impl(console_num, 0, 0);
        if (result == 0)
            return console_num == current_console ? 0 : -1;
        if (result < 0 || !current_task)
            return -1;
        if (!console_interrupts_enabled()) return 1;
        yield();
    }
}

void console_switch_via_interrupt(int console_num) {
    uint64_t flags;

    if (!console_valid_index(console_num)) return;
    if (!console_initialized) return;
    if (console_num == current_console) return;

    flags = console_irqsave();
    __atomic_store_n(&pending_console_switch, console_num, __ATOMIC_RELEASE);
    console_irqrestore(flags);
}

void console_process_pending(void) {
    int pending;
    uint64_t flags;
    static volatile int in_processing = 0;
    
    if (!console_initialized) return;
    if (!console_interrupts_enabled()) return;
    
    if (__atomic_exchange_n(&in_processing, 1, __ATOMIC_ACQUIRE)) return;

    while (1) {
        if (console_switching) break;
        flags = console_irqsave();
        pending = __atomic_exchange_n(&pending_console_switch, -1,
                                      __ATOMIC_ACQ_REL);
        console_irqrestore(flags);

        if (console_valid_index(pending)) {
            console_switch_internal(pending);
            continue;
        }
        break;
    }

    __atomic_store_n(&in_processing, 0, __ATOMIC_RELEASE);
}


int console_get_current(void) {
    return current_console;
}

static void console_write_internal(int console_num, const char *data,
                                   size_t size, int skip_serial_async);

void console_putchar(char c) {
    console_write_internal(current_console, &c, 1, 0);
}

static int parse_csi_params(const char *buf, int len, int *params, int max_params) {
    int count;
    int val;
    int has_digit;
    int i;

    count = 0;
    val = 0;
    has_digit = 0;
    for (i = 0; i < len && count < max_params; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            if (val <= (INT32_MAX - (buf[i] - '0')) / 10)
                val = val * 10 + (buf[i] - '0');
            else
                val = INT32_MAX;
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
    uint64_t move_rows;

    if (top >= bottom) return;
    if (bottom > con->buffer_rows) bottom = con->buffer_rows;
    copy_cols = cols < con->buffer_cols ? cols : con->buffer_cols;
    move_rows = bottom - top - 1;
    if (copy_cols == con->buffer_cols) {
        memmove(console_char_row(con, top), console_char_row(con, top + 1),
                move_rows * copy_cols);
        if (con->color_buffer)
            memmove(console_color_row(con, top),
                    console_color_row(con, top + 1), move_rows * copy_cols);
        memmove(con->line_wrapped + top, con->line_wrapped + top + 1,
                move_rows);
    } else {
        for (row = top; row < bottom - 1; row++) {
            memcpy(console_char_row(con, row),
                   console_char_row(con, row + 1), copy_cols);
            if (con->color_buffer)
                memcpy(console_color_row(con, row),
                       console_color_row(con, row + 1), copy_cols);
            con->line_wrapped[row] = con->line_wrapped[row + 1];
        }
    }
    if (bottom > 0) {
        memset(console_char_row(con, bottom - 1), ' ', copy_cols);
        if (con->color_buffer)
            memset(console_color_row(con, bottom - 1), console_current_attr(con), copy_cols);
        con->line_wrapped[bottom - 1] = 0;
    }
}

static void console_scroll_region_down(console_t *con, uint64_t top, uint64_t bottom, uint64_t cols) {
    uint64_t row;
    uint64_t copy_cols;
    uint64_t move_rows;

    if (top >= bottom) return;
    if (bottom > con->buffer_rows) bottom = con->buffer_rows;
    copy_cols = cols < con->buffer_cols ? cols : con->buffer_cols;
    move_rows = bottom - top - 1;
    if (copy_cols == con->buffer_cols) {
        memmove(console_char_row(con, top + 1), console_char_row(con, top),
                move_rows * copy_cols);
        if (con->color_buffer)
            memmove(console_color_row(con, top + 1),
                    console_color_row(con, top), move_rows * copy_cols);
        memmove(con->line_wrapped + top + 1, con->line_wrapped + top,
                move_rows);
    } else {
        for (row = bottom - 1; row > top; row--) {
            memcpy(console_char_row(con, row),
                   console_char_row(con, row - 1), copy_cols);
            if (con->color_buffer)
                memcpy(console_color_row(con, row),
                       console_color_row(con, row - 1), copy_cols);
            con->line_wrapped[row] = con->line_wrapped[row - 1];
        }
    }
    memset(console_char_row(con, top), ' ', copy_cols);
    if (con->color_buffer)
        memset(console_color_row(con, top), console_current_attr(con), copy_cols);
    con->line_wrapped[top] = 0;
}

static char *console_esc_data(console_t *con) {
    if (con->esc_len >= (int)sizeof(con->esc.inline_data) - 1)
        return con->esc.dynamic.data;
    return con->esc.inline_data;
}

static void console_esc_reset(console_t *con) {
    if (con->esc_len >= (int)sizeof(con->esc.inline_data) - 1 &&
        con->esc.dynamic.data)
        kfree(con->esc.dynamic.data);
    con->esc_len = 0;
    con->esc.inline_data[0] = '\0';
}

static int console_esc_append(console_t *con, char c) {
    char *grown;
    char *data;
    char inline_copy[32];
    int capacity;

    if (con->esc_len == INT32_MAX) return 0;
    if (con->esc_len < (int)sizeof(con->esc.inline_data) - 2) {
        con->esc.inline_data[con->esc_len++] = c;
        con->esc.inline_data[con->esc_len] = '\0';
        return 1;
    }
    if (con->esc_len == (int)sizeof(con->esc.inline_data) - 2) {
        memcpy(inline_copy, con->esc.inline_data, (size_t)con->esc_len);
        capacity = (int)sizeof(con->esc.inline_data) * 2;
        grown = (char *)kmalloc((size_t)capacity);
        if (!grown) return 0;
        memcpy(grown, inline_copy, (size_t)con->esc_len);
        con->esc.dynamic.data = grown;
        con->esc.dynamic.capacity = capacity;
    } else {
        capacity = con->esc.dynamic.capacity;
        grown = con->esc.dynamic.data;
    }
    while (capacity <= con->esc_len + 1) {
        if (capacity > INT32_MAX / 2) {
            capacity = con->esc_len + 2;
            break;
        }
        capacity *= 2;
    }
    if (capacity != con->esc.dynamic.capacity) {
        grown = (char *)krealloc(grown, (size_t)capacity);
        if (!grown) return 0;
        con->esc.dynamic.data = grown;
        con->esc.dynamic.capacity = capacity;
    }
    data = con->esc.dynamic.data;
    data[con->esc_len++] = c;
    data[con->esc_len] = '\0';
    return 1;
}

static void console_handle_csi(int console_num, console_t *con, framebuffer_t *fb, uint64_t rows, uint64_t cols, int is_active) {
    char cmd;
    char *esc_data;
    int param_start;
    int is_private;
    int inline_params[8];
    int *params;
    int param_capacity;
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
    esc_data = console_esc_data(con);
    param_capacity = 1;
    for (n = 0; n < con->esc_len - 1; n++) {
        if (esc_data[n] == ';') {
            if (param_capacity == INT32_MAX) return;
            param_capacity++;
        }
    }
    params = inline_params;
    if (param_capacity > (int)(sizeof(inline_params) / sizeof(inline_params[0]))) {
        if ((size_t)param_capacity > SIZE_MAX / sizeof(int)) return;
        params = (int *)kmalloc((size_t)param_capacity * sizeof(int));
        if (!params) return;
    }

    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    console_grow_buffer(con, rows);
    if (rows > con->buffer_rows) rows = con->buffer_rows;
    if (cols > con->buffer_cols) cols = con->buffer_cols;
    console_ensure_nondefault_color(con);

    cmd = esc_data[con->esc_len - 1];
    
    param_start = 0;
    is_private = 0;
    if (esc_data[0] == '?') {
        is_private = 1;
        param_start = 1;
    }
    
    memset(params, 0, (size_t)param_capacity * sizeof(int));
    nparams = parse_csi_params(esc_data + param_start,
                               con->esc_len - 1 - param_start,
                               params, param_capacity);

    if (is_private) {
        if (cmd == 'h') {
            if (nparams >= 1 && params[0] == 25) {
                con->cursor_visible = 1;
                if (is_active && fb) fb_set_cursor_hidden(0);
            }
            if (nparams >= 1 && params[0] == 1049) {
                con->alt_screen_pending = 1;
            }
        } else if (cmd == 'l') {
            if (nparams >= 1 && params[0] == 25) {
                con->cursor_visible = 0;
                if (is_active && fb) fb_set_cursor_hidden(1);
            }
            if (nparams >= 1 && params[0] == 1049) {
                con->alt_screen_pending = -1;
            }
        }
        if (params != inline_params) kfree(params);
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
        break;
    case 'A':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if (con->cursor_y >= (uint64_t)n) con->cursor_y -= n;
        else con->cursor_y = 0;
        break;
    case 'B':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_y += n;
        if (con->cursor_y >= rows) con->cursor_y = rows - 1;
        break;
    case 'C':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x += n;
        if (con->cursor_x >= cols) con->cursor_x = cols - 1;
        break;
    case 'D':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if (con->cursor_x >= (uint64_t)n) con->cursor_x -= n;
        else con->cursor_x = 0;
        break;
    case 'E':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x = 0;
        con->cursor_y += n;
        if (con->cursor_y >= rows) con->cursor_y = rows - 1;
        break;
    case 'F':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        con->cursor_x = 0;
        if (con->cursor_y >= (uint64_t)n) con->cursor_y -= n;
        else con->cursor_y = 0;
        break;
    case 'G':
        col = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        if ((uint64_t)col >= cols) col = cols - 1;
        con->cursor_x = col;
        break;
    case 'd':
        row = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        if ((uint64_t)row >= rows) row = rows - 1;
        con->cursor_y = row;
        break;
    case 'J':
        mode = (nparams >= 1) ? params[0] : 0;
        if (mode == 2 || mode == 3) {
            if (con->allocated && con->buffer) {
                for (r = 0; r < con->buffer_rows; r++) {
                    for (c2 = 0; c2 < con->buffer_cols; c2++) {
                        console_char_row(con, r)[c2] = ' ';
                        if (con->color_buffer) console_color_row(con, r)[c2] = console_current_attr(con);
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
            for (c2 = con->cursor_x; c2 < cols && c2 < con->buffer_cols; c2++) {
                console_char_row(con, con->cursor_y)[c2] = ' ';
                if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
            }
            for (r = con->cursor_y + 1; r < rows && r < con->buffer_rows; r++)
                for (c2 = 0; c2 < cols && c2 < con->buffer_cols; c2++) {
                    console_char_row(con, r)[c2] = ' ';
                    if (con->color_buffer) console_color_row(con, r)[c2] = console_current_attr(con);
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
                for (c2 = 0; c2 < cols && c2 < con->buffer_cols; c2++) {
                    console_char_row(con, r)[c2] = ' ';
                    if (con->color_buffer) console_color_row(con, r)[c2] = console_current_attr(con);
                }
            for (c2 = 0; c2 <= con->cursor_x && c2 < con->buffer_cols; c2++) {
                console_char_row(con, con->cursor_y)[c2] = ' ';
                if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
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
            for (c2 = con->cursor_x; c2 < cols && c2 < con->buffer_cols; c2++) {
                console_char_row(con, con->cursor_y)[c2] = ' ';
                if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        } else if (mode == 1) {
            for (c2 = 0; c2 <= con->cursor_x && c2 < con->buffer_cols; c2++) {
                console_char_row(con, con->cursor_y)[c2] = ' ';
                if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        } else if (mode == 2) {
            for (c2 = 0; c2 < cols && c2 < con->buffer_cols; c2++) {
                console_char_row(con, con->cursor_y)[c2] = ' ';
                if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
                if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
            }
        }
        break;
    case 'X':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        for (c2 = con->cursor_x; c2 < con->cursor_x + (uint64_t)n && c2 < cols && c2 < con->buffer_cols; c2++) {
            console_char_row(con, con->cursor_y)[c2] = ' ';
            if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
            if (is_active && fb) fb_putchar(' ', c2, con->cursor_y);
        }
        break;
    case '@':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if ((uint64_t)n > cols - con->cursor_x) n = cols - con->cursor_x;
        for (c2 = cols - 1; c2 >= con->cursor_x + (uint64_t)n && c2 < con->buffer_cols; c2--) {
            console_char_row(con, con->cursor_y)[c2] = console_char_row(con, con->cursor_y)[c2 - n];
            if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_color_row(con, con->cursor_y)[c2 - n];
        }
        for (c2 = con->cursor_x; c2 < con->cursor_x + (uint64_t)n && c2 < con->buffer_cols; c2++) {
            console_char_row(con, con->cursor_y)[c2] = ' ';
            if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
        }
        if (is_active && fb) {
            for (c2 = 0; c2 < cols; c2++) {
                if (con->color_buffer) console_apply_attr(console_color_row(con, con->cursor_y)[c2], fb);
                fb_putchar(console_char_row(con, con->cursor_y)[c2], c2, con->cursor_y);
            }
            console_apply_colors(con, fb);
        }
        break;
    case 'P':
        n = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
        if ((uint64_t)n > cols - con->cursor_x) n = cols - con->cursor_x;
        for (c2 = con->cursor_x; c2 + (uint64_t)n < cols && c2 < con->buffer_cols; c2++) {
            console_char_row(con, con->cursor_y)[c2] = console_char_row(con, con->cursor_y)[c2 + n];
            if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_color_row(con, con->cursor_y)[c2 + n];
        }
        for (c2 = cols - (uint64_t)n; c2 < cols && c2 < con->buffer_cols; c2++) {
            console_char_row(con, con->cursor_y)[c2] = ' ';
            if (con->color_buffer) console_color_row(con, con->cursor_y)[c2] = console_current_attr(con);
        }
        if (is_active && fb) {
            for (c2 = 0; c2 < cols; c2++) {
                if (con->color_buffer) console_apply_attr(console_color_row(con, con->cursor_y)[c2], fb);
                fb_putchar(console_char_row(con, con->cursor_y)[c2], c2, con->cursor_y);
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
                    if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                    fb_putchar(console_char_row(con, r)[c2], c2, r);
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
                if (fb_avoid_vram_reads())
                    console_fast_redraw_locked(console_num);
                else
                    for (count = 0; count < n; count++) fb_scroll();
            } else {
                for (r = top; r < bot; r++)
                    for (c2 = 0; c2 < cols; c2++) {
                        if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                        fb_putchar(console_char_row(con, r)[c2], c2, r);
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
                if (fb_avoid_vram_reads())
                    console_fast_redraw_locked(console_num);
                else
                    for (count = 0; count < n; count++) fb_scroll();
            } else {
                for (r = top; r < bot; r++)
                    for (c2 = 0; c2 < cols; c2++) {
                        if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                        fb_putchar(console_char_row(con, r)[c2], c2, r);
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
                    if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                    fb_putchar(console_char_row(con, r)[c2], c2, r);
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
    if (params != inline_params) kfree(params);
}

static void __attribute__((unused)) console_putchar_to_nolock(int console_num, char c) {
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
            console_esc_reset(con);
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
                            if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                            fb_putchar(console_char_row(con, r)[c2], c2, r);
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
                        if (fb_avoid_vram_reads())
                            console_fast_redraw_locked(console_num);
                        else
                            fb_scroll();
                    } else {
                        for (r = sc_top; r < sc_bot; r++)
                            for (c2 = 0; c2 < cols; c2++) {
                                if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                                fb_putchar(console_char_row(con, r)[c2], c2, r);
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
            if (!console_esc_append(con, c)) {
                con->esc_state = 0;
                console_esc_reset(con);
            }
            return;
        }
        if (console_esc_append(con, c))
            console_handle_csi(console_num, con, fb, rows, cols, is_active);
        con->esc_state = 0;
        console_esc_reset(con);
        if (is_active && fb && !console_batch) {
            fb->cursor_x = con->cursor_x;
            fb->cursor_y = con->cursor_y;
            fb_update_cursor();
        }
        return;
    }

    if (c == '\033') {
        con->esc_state = 1;
        console_esc_reset(con);
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
                    if (fb_avoid_vram_reads())
                        console_fast_redraw_locked(console_num);
                    else
                        fb_scroll();
                } else {
                    for (r = sc_top; r < sc_bot; r++)
                        for (c2 = 0; c2 < cols; c2++) {
                            if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                            fb_putchar(console_char_row(con, r)[c2], c2, r);
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
            if (is_active && fb) {
                fb->cursor_x = con->cursor_x;
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

    if (con->cursor_y < con->buffer_rows && con->cursor_x < con->buffer_cols) {
        console_char_row(con, con->cursor_y)[con->cursor_x] = c;
        if (con->color_buffer) console_color_row(con, con->cursor_y)[con->cursor_x] = console_current_attr(con);
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
                    if (fb_avoid_vram_reads())
                        console_fast_redraw_locked(console_num);
                    else
                        fb_scroll();
                } else {
                    for (r = sc_top; r < sc_bot; r++)
                        for (c2 = 0; c2 < cols; c2++) {
                            if (con->color_buffer) console_apply_attr(console_color_row(con, r)[c2], fb);
                            fb_putchar(console_char_row(con, r)[c2], c2, r);
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
    console_write_internal(console_num, &c, 1, 0);
}

void console_write(const char *data, size_t size) {
    console_write_internal(current_console, data, size, 0);
}

void console_write_to(int console_num, const char *data, size_t size) {
    console_write_internal(console_num, data, size, 0);
}

void console_write_to_fb_only(int console_num, const char *data, size_t size) {
    console_write_internal(console_num, data, size, 1);
}

static void console_write_internal(int console_num, const char *data, size_t size, int skip_serial_async) {
    int target_console;
    size_t i;
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

    if (!console_initialized) {
        for (i = 0; i < size; i++) terminal_putchar(data[i]);
        return;
    }

    target_console = console_num;
    if (!console_valid_index(target_console)) {
        target_console = current_console;
    }

    if (!consoles[target_console].allocated) {
        console_ensure_alloc(target_console);
        if (!consoles[target_console].allocated) return;
    }

    con = &consoles[target_console];

    if (console_num == 0 && !skip_serial_async) {
        if (kprint_is_ready()) {
            kprint_serial_async(data, size);
        } else {
            serial_write_direct(data, size);
        }
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
        if (cols > con->buffer_cols) cols = con->buffer_cols;
        fb_ok = is_active && fb && !batch_fb_skip;

        for (i = 0; i < chunk; i++) {
            c = data[off + i];
            if (target_console == 0 && !skip_serial_async && !skip_serial && !kprint_is_ready()) {
                serial_putchar(c);
            }

            if (con->esc_state == 1) {
                if (c == '[') {
                    con->esc_state = 2;
                    console_esc_reset(con);
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
                                    if (con->color_buffer) console_apply_attr(console_color_row(con, sr)[sc], fb);
                                    fb_putchar(console_char_row(con, sr)[sc], sc, sr);
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
                                    if (con->color_buffer) console_apply_attr(console_color_row(con, sr)[sc], fb);
                                    fb_putchar(console_char_row(con, sr)[sc], sc, sr);
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
                    if (!console_esc_append(con, c)) {
                        con->esc_state = 0;
                        console_esc_reset(con);
                    }
                    continue;
                }
                if (console_esc_append(con, c))
                    console_handle_csi(target_console, con, fb, rows, cols, fb_ok);
                con->esc_state = 0;
                console_esc_reset(con);
                if (con->alt_screen_pending) {
                    chunk = i + 1;
                    break;
                }
                continue;
            }

            if (c == '\033') {
                con->esc_state = 1;
                console_esc_reset(con);
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
                        if (sc_top == 0 && sc_bot == rows &&
                            fb_avoid_vram_reads()) {
                            batch_fb_skip = 1;
                            fb_ok = 0;
                        } else if (sc_top == 0 && sc_bot == rows) {
                            fb_scroll();
                        } else {
                            for (sr = sc_top; sr < sc_bot; sr++)
                                for (sc = 0; sc < cols; sc++) {
                                    if (con->color_buffer) console_apply_attr(console_color_row(con, sr)[sc], fb);
                                    fb_putchar(console_char_row(con, sr)[sc], sc, sr);
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
                    if (fb_ok) {
                        fb->cursor_x = con->cursor_x;
                    }
                }
                continue;
            }

            if (c == '\t') {
                tab_stop = 8 - (con->cursor_x % 8);
                for (t = 0; t < tab_stop; t++) {
                    if (con->cursor_y < con->buffer_rows && con->cursor_x < con->buffer_cols) {
                        console_char_row(con, con->cursor_y)[con->cursor_x] = ' ';
                        if (con->color_buffer) console_color_row(con, con->cursor_y)[con->cursor_x] = console_current_attr(con);
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
                                if (sc_top == 0 && sc_bot == rows &&
                                    fb_avoid_vram_reads()) {
                                    batch_fb_skip = 1;
                                    fb_ok = 0;
                                } else if (sc_top == 0 && sc_bot == rows) {
                                    fb_scroll();
                                } else {
                                    for (sr = sc_top; sr < sc_bot; sr++)
                                        for (sc = 0; sc < cols; sc++) {
                                            if (con->color_buffer) console_apply_attr(console_color_row(con, sr)[sc], fb);
                                            fb_putchar(console_char_row(con, sr)[sc], sc, sr);
                                        }
                                }
                                console_apply_colors(con, fb);
                            }
                        }
                    }
                }
                continue;
            }

            if (con->cursor_y < con->buffer_rows && con->cursor_x < con->buffer_cols) {
                console_char_row(con, con->cursor_y)[con->cursor_x] = c;
                if (con->color_buffer) console_color_row(con, con->cursor_y)[con->cursor_x] = console_current_attr(con);
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
                        if (sc_top == 0 && sc_bot == rows &&
                            fb_avoid_vram_reads()) {
                            batch_fb_skip = 1;
                            fb_ok = 0;
                        } else if (sc_top == 0 && sc_bot == rows) {
                            fb_scroll();
                        } else {
                            for (sr = sc_top; sr < sc_bot; sr++)
                                for (sc = 0; sc < cols; sc++) {
                                    if (con->color_buffer) console_apply_attr(console_color_row(con, sr)[sc], fb);
                                    fb_putchar(console_char_row(con, sr)[sc], sc, sr);
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
        if (__atomic_load_n(&pending_console_switch, __ATOMIC_ACQUIRE) >= 0 &&
            console_interrupts_enabled()) {
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
    if (__atomic_load_n(&pending_console_switch, __ATOMIC_ACQUIRE) >= 0 &&
        console_interrupts_enabled()) {
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
    uint64_t row;
    uint64_t col;
    console_t *con;
    framebuffer_t *fb;

    if (!console_valid_index(console_num)) return;

    flags = console_irqsave();
    spin_lock(&console_lock);
    
    con = &consoles[console_num];
    console_expand_color_buffer(con);
    if (con->allocated && con->buffer) {
        for (row = 0; row < con->buffer_rows; row++) {
            for (col = 0; col < con->buffer_cols; col++) {
                console_char_row(con, row)[col] = ' ';
                if (con->color_buffer) console_color_row(con, row)[col] = 0x70;
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

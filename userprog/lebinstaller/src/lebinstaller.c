#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <crypt.h>
#include <lebirun.h>

#define SECTOR_SIZE  512
#define BUF_SIZE     4096
#define MAX_DISKS    8
#define MAX_PARTS    16
#define MAX_PATH     256
#define MAX_LINE     128
#define MBR_SIG      0xAA55

#define KEY_UP      1000
#define KEY_DOWN    1001
#define KEY_ENTER   1002
#define KEY_ESC     1003
#define KEY_TAB     1004
#define KEY_BKSP    1005
#define KEY_LEFT    1006
#define KEY_RIGHT   1007

#define CLR_NORMAL  "\033[0m"
#define CLR_TITLE   "\033[1;33;40m"
#define CLR_MENU    "\033[0;37;44m"
#define CLR_SELECT  "\033[0;34;47m"
#define CLR_BORDER  "\033[1;37;44m"
#define CLR_INPUT   "\033[0;30;47m"
#define CLR_BTN     "\033[1;37;44m"
#define CLR_BTN_SEL "\033[0;30;47m"
#define CLR_BAR     "\033[0;30;46m"
#define CLR_PROG    "\033[1;37;42m"
#define CLR_PROG_BG "\033[0;37;40m"
#define CLR_ERR     "\033[1;31;47m"
#define CLR_OK      "\033[1;32;44m"
#define CLR_DIM     "\033[0;36;44m"

int vfs_open(const char *path, int flags);
int vfs_close_fd(int fd);
int vfs_read_fd(int fd, void *buf, unsigned int count);
int vfs_write_fd(int fd, const void *buf, unsigned int count);
int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index);
int vfs_stat(int fd, uint64_t *size, uint64_t *type);
int vfs_create(const char *path, unsigned int perms);
int vfs_mkdir(const char *path, unsigned int perms);
int vfs_unlink(const char *path);
int vfs_mounts(void);
unsigned int getticks(void);

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    uint8_t     bootstrap[446];
    mbr_entry_t parts[4];
    uint16_t    signature;
} __attribute__((packed)) mbr_t;

typedef struct {
    int      valid;
    int      number;
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t  mbr_type;
    char     devpath[32];
} part_info_t;

typedef struct {
    char     devname[16];
    char     devpath[32];
    uint64_t disk_sectors;
    int      part_count;
    part_info_t parts[MAX_PARTS];
} disk_info_t;

static disk_info_t disks[MAX_DISKS];
static int disk_count;
static int term_rows;
static int term_cols;
static struct termios orig_termios;

static const char *timezones[] = {
    "GMT-12", "GMT-11", "GMT-10", "GMT-9", "GMT-8", "GMT-7",
    "GMT-6",  "GMT-5",  "GMT-4",  "GMT-3", "GMT-2", "GMT-1",
    "GMT+0",
    "GMT+1",  "GMT+2",  "GMT+3",  "GMT+4", "GMT+5", "GMT+6",
    "GMT+7",  "GMT+8",  "GMT+9",  "GMT+10", "GMT+11", "GMT+12",
    "GMT+13", "GMT+14",
    NULL
};

static const char *tz_values[] = {
    "GMT+12", "GMT+11", "GMT+10", "GMT+9", "GMT+8", "GMT+7",
    "GMT+6",  "GMT+5",  "GMT+4",  "GMT+3", "GMT+2", "GMT+1",
    "GMT0",
    "GMT-1",  "GMT-2",  "GMT-3",  "GMT-4", "GMT-5", "GMT-6",
    "GMT-7",  "GMT-8",  "GMT-9",  "GMT-10", "GMT-11", "GMT-12",
    "GMT-13", "GMT-14",
    NULL
};

static void tui_get_size(void)
{
    struct winsize ws;

    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        term_rows = ws.ws_row;
        term_cols = ws.ws_col;
    } else {
        term_rows = 25;
        term_cols = 80;
    }
}

static void tui_raw_mode(void)
{
    struct termios raw;

    tcgetattr(0, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
}

static void tui_restore_mode(void)
{
    tcsetattr(0, TCSANOW, &orig_termios);
}

static void tui_goto(int row, int col)
{
    printf("\033[%d;%dH", row, col);
}

static void tui_hide_cursor(void)
{
    printf("\033[?25l");
}

static void tui_show_cursor(void)
{
    printf("\033[?25h");
}

static int tui_read_key(void)
{
    char c;
    char seq[4];
    unsigned int start;

    if (read(0, &c, 1) <= 0) return -1;

    if (c == '\033') {
        start = getticks();
        while (getticks() - start < 2) {
            if (read_nb(0, &seq[0], 1) > 0) goto got_bracket;
        }
        return KEY_ESC;
got_bracket:
        if (seq[0] != '[') return -1;
        start = getticks();
        while (getticks() - start < 2) {
            if (read_nb(0, &seq[1], 1) > 0) goto got_code;
        }
        return -1;
got_code:
        if (seq[1] == 'A') return KEY_UP;
        if (seq[1] == 'B') return KEY_DOWN;
        if (seq[1] == 'C') return KEY_RIGHT;
        if (seq[1] == 'D') return KEY_LEFT;
        if (seq[1] >= '0' && seq[1] <= '9') {
            start = getticks();
            while (getticks() - start < 2) {
                if (read_nb(0, &seq[2], 1) > 0) break;
            }
        }
        return -1;
    }
    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == '\t') return KEY_TAB;
    if (c == 127 || c == '\b') return KEY_BKSP;
    return (int)(unsigned char)c;
}

static void tui_fill_bg(void)
{
    int r;

    printf("%s", CLR_MENU);
    for (r = 1; r <= term_rows; r++) {
        tui_goto(r, 1);
        printf("\033[2K");
    }
    fflush(stdout);
}

static void tui_draw_box(int y, int x, int h, int w, const char *title)
{
    int i;
    int r;
    int tw;

    tui_goto(y, x);
    printf("%s+", CLR_BORDER);
    for (i = 0; i < w - 2; i++) putchar('-');
    putchar('+');

    if (title) {
        tw = (int)strlen(title);
        tui_goto(y, x + (w - tw - 2) / 2);
        printf("%s %s ", CLR_TITLE, title);
    }

    for (r = 1; r < h - 1; r++) {
        tui_goto(y + r, x);
        printf("%s|", CLR_BORDER);
        printf("%s", CLR_MENU);
        for (i = 0; i < w - 2; i++) putchar(' ');
        printf("%s|", CLR_BORDER);
        tui_goto(y + r, x + w);
        printf("\033[0;30;40m ");
    }

    tui_goto(y + h - 1, x);
    printf("%s+", CLR_BORDER);
    for (i = 0; i < w - 2; i++) putchar('-');
    putchar('+');
    tui_goto(y + h - 1, x + w);
    printf("\033[0;30;40m ");

    tui_goto(y + h, x + 1);
    printf("\033[0;30;40m");
    for (i = 0; i < w; i++) putchar(' ');
}

static void tui_draw_titlebar(void)
{
    int i;
    const char *title = " Lebirun OS Installer";

    tui_goto(1, 1);
    printf("%s", CLR_BAR);
    printf("%s", title);
    for (i = (int)strlen(title); i < term_cols; i++) putchar(' ');
}

static void tui_draw_helpbar(const char *text)
{
    int i;
    int len;

    tui_goto(term_rows, 1);
    printf("%s", CLR_BAR);
    len = (int)strlen(text);
    printf("%s", text);
    for (i = len; i < term_cols; i++) putchar(' ');
}

static void tui_center_text(int row, int bx, int bw, const char *color, const char *text)
{
    int len;
    int col;

    len = (int)strlen(text);
    col = bx + (bw - len) / 2;
    if (col < bx + 1) col = bx + 1;
    tui_goto(row, col);
    printf("%s%s", color, text);
}

static void tui_draw_screen(const char *title, const char *help)
{
    tui_fill_bg();
    tui_draw_titlebar();
    tui_draw_helpbar(help);
    (void)title;
}

static int tui_menu(const char *title, const char **items, int count, const char *help)
{
    int sel;
    int bw;
    int bh;
    int bx;
    int by;
    int i;
    int key;
    int maxw;
    int len;
    int view_h;
    int scroll;
    int sb_pos;
    int sb_h;

    sel = 0;
    scroll = 0;
    maxw = (int)strlen(title) + 4;
    for (i = 0; i < count; i++) {
        len = (int)strlen(items[i]);
        if (len + 8 > maxw) maxw = len + 8;
    }
    bw = maxw + 4;
    if (bw > term_cols - 4) bw = term_cols - 4;
    bh = count + 4;
    if (bh > term_rows - 4) bh = term_rows - 4;
    bx = (term_cols - bw) / 2 + 1;
    by = (term_rows - bh) / 2;
    if (by < 3) by = 3;
    view_h = bh - 4;

    tui_draw_screen(title, help);
    tui_draw_box(by, bx, bh, bw, title);

    for (;;) {
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + view_h) scroll = sel - view_h + 1;

        for (i = 0; i < view_h; i++) {
            int idx = scroll + i;
            tui_goto(by + 2 + i, bx + 2);
            if (idx < count) {
                if (idx == sel) {
                    printf("%s", CLR_SELECT);
                } else {
                    printf("%s", CLR_MENU);
                }
                printf(" %-*.*s ", bw - 8, bw - 8, items[idx]);
            } else {
                printf("%s%-*s", CLR_MENU, bw - 6, "");
            }
        }

        if (count > view_h) {
            sb_h = view_h;
            sb_pos = (sel * (sb_h - 1)) / (count - 1);
            for (i = 0; i < sb_h; i++) {
                tui_goto(by + 2 + i, bx + bw - 3);
                if (i == sb_pos)
                    printf("%s#%s", CLR_SELECT, CLR_BORDER);
                else
                    printf("%s:%s", CLR_DIM, CLR_BORDER);
            }
        }

        printf("%s", CLR_NORMAL);
        fflush(stdout);

        key = tui_read_key();
        if (key == KEY_UP) {
            if (sel > 0) sel--;
        } else if (key == KEY_DOWN) {
            if (sel < count - 1) sel++;
        } else if (key == KEY_ENTER) {
            return sel;
        } else if (key == KEY_ESC) {
            return -1;
        }
    }
}

static int tui_confirm(const char *title, const char *msg)
{
    int sel;
    int bw;
    int bh;
    int bx;
    int by;
    int key;
    int mlen;
    int tlen;

    sel = 0;
    mlen = (int)strlen(msg);
    tlen = (int)strlen(title);
    bw = mlen + 6;
    if (tlen + 6 > bw) bw = tlen + 6;
    if (bw < 30) bw = 30;
    if (bw > term_cols - 4) bw = term_cols - 4;
    bh = 8;
    bx = (term_cols - bw) / 2 + 1;
    by = (term_rows - bh) / 2;
    if (by < 3) by = 3;

    tui_draw_screen(title, " <Tab> Switch  <Enter> Select");
    tui_draw_box(by, bx, bh, bw, title);
    tui_center_text(by + 2, bx, bw, CLR_MENU, msg);

    for (;;) {
        tui_goto(by + 5, bx + bw / 2 - 9);
        if (sel == 0)
            printf("%s< Yes  >%s  %s[  No  ]%s", CLR_BTN_SEL, CLR_MENU, CLR_BTN, CLR_NORMAL);
        else
            printf("%s[ Yes  ]%s  %s<  No  >%s", CLR_BTN, CLR_MENU, CLR_BTN_SEL, CLR_NORMAL);

        fflush(stdout);

        key = tui_read_key();
        if (key == KEY_TAB || key == KEY_LEFT || key == KEY_RIGHT) {
            sel = !sel;
        } else if (key == KEY_ENTER) {
            return (sel == 0) ? 1 : 0;
        } else if (key == KEY_ESC) {
            return 0;
        } else if (key == 'y' || key == 'Y') {
            return 1;
        } else if (key == 'n' || key == 'N') {
            return 0;
        }
    }
}

static void tui_msgbox(const char *title, const char *msg)
{
    int bw;
    int bh;
    int bx;
    int by;
    int key;
    int mlen;
    int tlen;

    mlen = (int)strlen(msg);
    tlen = (int)strlen(title);
    bw = mlen + 6;
    if (tlen + 6 > bw) bw = tlen + 6;
    if (bw < 24) bw = 24;
    if (bw > term_cols - 4) bw = term_cols - 4;
    bh = 7;
    bx = (term_cols - bw) / 2 + 1;
    by = (term_rows - bh) / 2;
    if (by < 3) by = 3;

    tui_draw_screen(title, " <Enter> OK");
    tui_draw_box(by, bx, bh, bw, title);
    tui_center_text(by + 2, bx, bw, CLR_MENU, msg);
    tui_goto(by + 4, bx + bw / 2 - 3);
    printf("%s< OK >%s", CLR_BTN_SEL, CLR_NORMAL);
    fflush(stdout);

    for (;;) {
        key = tui_read_key();
        if (key == KEY_ENTER || key == KEY_ESC) return;
    }
}

static int tui_input(const char *title, const char *prompt, char *buf, int maxlen, int hidden)
{
    int bw;
    int bh;
    int bx;
    int by;
    int key;
    int len;
    int plen;
    int tlen;
    int fw;
    int i;

    len = 0;
    buf[0] = '\0';
    plen = (int)strlen(prompt);
    tlen = (int)strlen(title);
    bw = 50;
    if (plen + 6 > bw) bw = plen + 6;
    if (tlen + 6 > bw) bw = tlen + 6;
    if (bw > term_cols - 4) bw = term_cols - 4;
    bh = 8;
    bx = (term_cols - bw) / 2 + 1;
    by = (term_rows - bh) / 2;
    if (by < 3) by = 3;
    fw = bw - 6;
    if (maxlen - 1 < fw) fw = maxlen - 1;

    tui_draw_screen(title, " <Enter> Confirm  <Esc> Cancel");
    tui_draw_box(by, bx, bh, bw, title);

    tui_goto(by + 2, bx + 2);
    printf("%s%s", CLR_MENU, prompt);

    tui_goto(by + 6, bx + bw / 2 - 3);
    printf("%s< OK >%s", CLR_BTN, CLR_NORMAL);

    for (;;) {
        tui_goto(by + 4, bx + 3);
        printf("%s", CLR_INPUT);
        if (hidden) {
            for (i = 0; i < len && i < fw; i++) putchar('*');
        } else {
            for (i = 0; i < len && i < fw; i++) putchar(buf[i]);
        }
        for (i = len; i < fw; i++) putchar(' ');

        printf("%s", CLR_NORMAL);
        tui_goto(by + 4, bx + 3 + len);
        tui_show_cursor();
        fflush(stdout);

        key = tui_read_key();
        tui_hide_cursor();

        if (key == KEY_ENTER) {
            buf[len] = '\0';
            return len;
        } else if (key == KEY_ESC) {
            buf[0] = '\0';
            return -1;
        } else if (key == KEY_BKSP) {
            if (len > 0) len--;
        } else if (key >= 32 && key < 127 && len < fw) {
            buf[len++] = (char)key;
        }
        buf[len] = '\0';
    }
}

static int prog_drawn;
static int prog_bx;
static int prog_by;
static int prog_bw;
static int log_by;
static int log_bh;
#define LOG_MAX 8
static char log_lines[LOG_MAX][64];
static int log_count;

static void tui_progress(const char *title, const char *msg, int pct)
{
    int bar_w;
    int filled;
    int i;
    int mw;
    int prog_h;

    prog_bw = 56;
    if (prog_bw > term_cols - 4) prog_bw = term_cols - 4;
    prog_h = 8;
    log_bh = 10;
    if (log_bh > term_rows - prog_h - 6) log_bh = term_rows - prog_h - 6;
    if (log_bh < 4) log_bh = 4;
    prog_bx = (term_cols - prog_bw) / 2 + 1;
    prog_by = 3;
    log_by = prog_by + prog_h;
    bar_w = prog_bw - 8;
    mw = prog_bw - 6;

    if (!prog_drawn) {
        tui_draw_screen(title, " Please wait...");
        tui_draw_box(prog_by, prog_bx, prog_h, prog_bw, "Installing");
        tui_draw_box(log_by, prog_bx, log_bh, prog_bw, "Log");
        fflush(stdout);
        log_count = 0;
        prog_drawn = 1;
    }

    tui_goto(prog_by + 2, prog_bx + 3);
    printf("%s%-*.*s", CLR_MENU, mw, mw, msg);

    filled = (pct * bar_w) / 100;
    if (filled > bar_w) filled = bar_w;

    tui_goto(prog_by + 4, prog_bx + 4);
    printf("%s", CLR_PROG);
    for (i = 0; i < filled; i++) putchar(' ');
    printf("%s", CLR_PROG_BG);
    for (i = filled; i < bar_w; i++) putchar(' ');

    tui_goto(prog_by + 5, prog_bx + prog_bw / 2 - 2);
    printf("%s%3d%%", CLR_MENU, pct);

    printf("%s", CLR_NORMAL);
    fflush(stdout);
}

static void tui_progress_reset(void)
{
    prog_drawn = 0;
}

static void tui_log(const char *msg)
{
    int log_area;
    int i;
    int mw;

    log_area = log_bh - 2;
    if (log_area > LOG_MAX) log_area = LOG_MAX;
    if (log_area < 1) return;
    mw = prog_bw - 6;
    if (mw > 63) mw = 63;

    if (log_count < log_area) {
        strncpy(log_lines[log_count], msg, mw);
        log_lines[log_count][mw] = '\0';
        log_count++;
    } else {
        for (i = 0; i < log_area - 1; i++)
            strcpy(log_lines[i], log_lines[i + 1]);
        strncpy(log_lines[log_area - 1], msg, mw);
        log_lines[log_area - 1][mw] = '\0';
    }

    for (i = 0; i < log_count && i < log_area; i++) {
        tui_goto(log_by + 1 + i, prog_bx + 3);
        printf("%s%-*s", CLR_DIM, mw, log_lines[i]);
    }

    printf("%s", CLR_NORMAL);
    fflush(stdout);
}

static int inst_disk_read(const char *devpath, uint32_t lba, uint32_t count, void *buf)
{
    int fd;
    uint32_t total;
    int ret;
    off_t offset;

    fd = vfs_open(devpath, 0);
    if (fd < 0) return -1;

    total = count * SECTOR_SIZE;
    offset = (off_t)lba * SECTOR_SIZE;

    if (offset > 0) {
        if (lseek(fd, offset, SEEK_SET) < 0) {
            vfs_close_fd(fd);
            return -1;
        }
    }

    ret = vfs_read_fd(fd, buf, total);
    vfs_close_fd(fd);
    return (ret < 0) ? -1 : ret;
}

static void inst_format_size(uint64_t sectors, char *buf, int bufsz)
{
    uint64_t bytes;
    uint64_t mb;
    uint64_t gb;

    bytes = sectors * SECTOR_SIZE;
    mb = bytes / (1024 * 1024);
    gb = mb / 1024;

    if (gb >= 1)
        snprintf(buf, bufsz, "%llu GiB", (unsigned long long)gb);
    else
        snprintf(buf, bufsz, "%llu MiB", (unsigned long long)mb);
}

static int inst_is_whole_disk(const char *name)
{
    int i;

    if (name[0] != 's' || name[1] != 'd') return 0;
    if (name[2] < 'a' || name[2] > 'z') return 0;
    for (i = 3; name[i]; i++) {
        if (name[i] >= '0' && name[i] <= '9') return 0;
    }
    return 1;
}

static void inst_scan_disk(disk_info_t *disk)
{
    static uint8_t sector0[SECTOR_SIZE];
    mbr_t *mbr;
    int ret;
    int i;
    int stat_fd;

    memset(disk->parts, 0, sizeof(disk->parts));
    disk->part_count = 0;
    disk->disk_sectors = 0;

    stat_fd = vfs_open(disk->devpath, 0);
    if (stat_fd >= 0) {
        uint64_t stat_size;
        uint64_t stat_type;
        stat_size = 0;
        stat_type = 0;
        if (vfs_stat(stat_fd, &stat_size, &stat_type) == 0) {
            disk->disk_sectors = stat_size / SECTOR_SIZE;
        }
        vfs_close_fd(stat_fd);
    }

    ret = inst_disk_read(disk->devpath, 0, 1, sector0);
    if (ret < SECTOR_SIZE) return;

    mbr = (mbr_t *)sector0;
    if (mbr->signature != MBR_SIG) return;

    for (i = 0; i < 4; i++) {
        if (mbr->parts[i].type == 0) continue;
        if (mbr->parts[i].sector_count == 0) continue;
        disk->parts[disk->part_count].valid = 1;
        disk->parts[disk->part_count].number = i + 1;
        disk->parts[disk->part_count].start_lba = mbr->parts[i].lba_start;
        disk->parts[disk->part_count].sector_count = mbr->parts[i].sector_count;
        disk->parts[disk->part_count].mbr_type = mbr->parts[i].type;
        snprintf(disk->parts[disk->part_count].devpath,
                 sizeof(disk->parts[disk->part_count].devpath),
                 "%s%d", disk->devpath, i + 1);
        disk->part_count++;
    }
}

static int inst_enumerate_disks(void)
{
    int fd;
    char name[64];
    unsigned int type;
    unsigned int idx;

    disk_count = 0;

    fd = vfs_open("/dev", 0);
    if (fd < 0) return -1;

    for (idx = 0; disk_count < MAX_DISKS; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (!inst_is_whole_disk(name)) continue;

        strncpy(disks[disk_count].devname, name, sizeof(disks[disk_count].devname) - 1);
        snprintf(disks[disk_count].devpath, sizeof(disks[disk_count].devpath),
                 "/dev/%s", name);
        inst_scan_disk(&disks[disk_count]);
        disk_count++;
    }
    vfs_close_fd(fd);
    return disk_count;
}

static int inst_copy_file_vfs(const char *src, const char *dst)
{
    int fd_in;
    int fd_out;
    static char buf[BUF_SIZE];
    int r;

    fd_in = vfs_open(src, 0);
    if (fd_in < 0) return -1;

    vfs_create(dst, 0644);
    fd_out = vfs_open(dst, 2);
    if (fd_out < 0) {
        fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out < 0) {
            vfs_close_fd(fd_in);
            return -1;
        }
    }

    while ((r = vfs_read_fd(fd_in, buf, BUF_SIZE)) > 0) {
        if (vfs_write_fd(fd_out, buf, r) != r) {
            vfs_close_fd(fd_in);
            vfs_close_fd(fd_out);
            return -1;
        }
    }

    vfs_close_fd(fd_in);
    vfs_close_fd(fd_out);
    return 0;
}

static int inst_count_dir_entries(const char *path)
{
    int fd;
    char name[256];
    unsigned int type;
    unsigned int idx;
    int count;

    count = 0;
    fd = vfs_open(path, 0);
    if (fd < 0) return 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        count++;
    }
    vfs_close_fd(fd);
    return count;
}

static int copy_total;
static int copy_done;

static int inst_copy_dir_recursive(const char *src, const char *dst, const char *skip)
{
    int fd;
    char name[256];
    unsigned int type;
    unsigned int idx;
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];
    int errors;
    int pct;

    vfs_mkdir(dst, 0755);

    fd = vfs_open(src, 0);
    if (fd < 0) return -1;

    errors = 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src, name);

        if (skip && strcmp(src_path, skip) == 0) continue;
        if (strcmp(name, "lebinstaller") == 0) continue;

        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, name);

        if (type == 2) {
            if (inst_copy_dir_recursive(src_path, dst_path, skip) < 0)
                errors++;
        } else {
            if (inst_copy_file_vfs(src_path, dst_path) < 0) {
                errors++;
            }
            copy_done++;
            if (copy_total > 0) {
                pct = (copy_done * 100) / copy_total;
                if (pct > 99) pct = 99;
                tui_progress("Installing", src_path, pct);
                tui_log(src_path);
            }
        }
    }
    vfs_close_fd(fd);
    return (errors > 0) ? -1 : 0;
}

static int inst_count_rootfs(void)
{
    static const char *dirs[] = {
        "bin", "boot", "dev", "etc", "home", "lib", "proc",
        "root", "sbin", "tmp", "usr", "var", NULL
    };
    int total;
    int i;
    char path[MAX_PATH];

    total = 0;
    for (i = 0; dirs[i]; i++) {
        if (strcmp(dirs[i], "dev") == 0 || strcmp(dirs[i], "proc") == 0)
            continue;
        snprintf(path, sizeof(path), "/%s", dirs[i]);
        total += inst_count_dir_entries(path);
    }
    return total;
}

static int inst_mount_partition(const char *devpath, const char *mountpoint)
{
    int ret;
    int pid;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int nfd;
        char *argv[6];
        nfd = open("/dev/null", O_WRONLY);
        if (nfd >= 0) { dup2(nfd, 1); dup2(nfd, 2); close(nfd); }
        argv[0] = "mount";
        argv[1] = "-t";
        argv[2] = "ext4";
        argv[3] = (char *)devpath;
        argv[4] = (char *)mountpoint;
        argv[5] = NULL;
        execv("/sbin/mount", argv);
        execv("/bin/mount", argv);
        execv("/bin/lebu", argv);
        _exit(127);
    }

    waitpid(pid, &ret, 0);
    return ret;
}

static int inst_umount_partition(const char *mountpoint)
{
    int pid;
    int ret;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int nfd;
        char *argv[3];
        nfd = open("/dev/null", O_WRONLY);
        if (nfd >= 0) { dup2(nfd, 1); dup2(nfd, 2); close(nfd); }
        argv[0] = "umount";
        argv[1] = (char *)mountpoint;
        argv[2] = NULL;
        execv("/sbin/umount", argv);
        execv("/bin/umount", argv);
        execv("/bin/lebu", argv);
        _exit(127);
    }

    waitpid(pid, &ret, 0);
    return ret;
}

static int inst_format_ext4(const char *devpath)
{
    int pid;
    int ret;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, 1);
            dup2(null_fd, 2);
            close(null_fd);
        }
        char *argv[3];
        argv[0] = "lformat.ext4";
        argv[1] = (char *)devpath;
        argv[2] = NULL;
        execv("/sbin/lformat.ext4", argv);
        execv("/bin/lformat.ext4", argv);
        execv("/bin/lebu", argv);
        _exit(127);
    }

    waitpid(pid, &ret, 0);
    return ret;
}

static int inst_copy_rootfs(const char *mountpoint)
{
    static const char *dirs[] = {
        "bin", "boot", "dev", "etc", "home", "lib", "proc",
        "root", "sbin", "tmp", "usr", "var", NULL
    };
    static const char *root_files[] = {
        "init", NULL
    };
    char src[MAX_PATH];
    char dst[MAX_PATH];
    int i;
    int errors;

    errors = 0;

    for (i = 0; root_files[i]; i++) {
        snprintf(src, sizeof(src), "/%s", root_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", mountpoint, root_files[i]);
        if (inst_copy_file_vfs(src, dst) < 0) {
            errors++;
        }
    }

    for (i = 0; dirs[i]; i++) {
        snprintf(src, sizeof(src), "/%s", dirs[i]);
        snprintf(dst, sizeof(dst), "%s/%s", mountpoint, dirs[i]);

        if (strcmp(dirs[i], "dev") == 0 || strcmp(dirs[i], "proc") == 0) {
            vfs_mkdir(dst, 0755);
            continue;
        }
        if (inst_copy_dir_recursive(src, dst, mountpoint) < 0) {
            errors++;
        }
    }

    return (errors > 0) ? -1 : 0;
}

static int inst_install_grub_mbr(const char *disk_dev)
{
    static uint8_t mbr_buf[SECTOR_SIZE];
    static uint8_t boot_buf[SECTOR_SIZE];
    static uint8_t core_buf[BUF_SIZE];
    int fd_disk;
    int fd_boot;
    int fd_core;
    int r;
    int off;

    fd_disk = vfs_open(disk_dev, 2);
    if (fd_disk < 0) return -1;

    r = vfs_read_fd(fd_disk, mbr_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        vfs_close_fd(fd_disk);
        return -1;
    }

    fd_boot = vfs_open("/boot/grub/i386-pc/boot.img", 0);
    if (fd_boot < 0) {
        vfs_close_fd(fd_disk);
        return -1;
    }
    vfs_read_fd(fd_boot, boot_buf, SECTOR_SIZE);
    vfs_close_fd(fd_boot);

    memcpy(mbr_buf, boot_buf, 440);
    mbr_buf[0x1FE] = 0x55;
    mbr_buf[0x1FF] = 0xAA;

    vfs_close_fd(fd_disk);
    fd_disk = vfs_open(disk_dev, 2);
    if (fd_disk < 0) return -1;

    r = vfs_write_fd(fd_disk, mbr_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        vfs_close_fd(fd_disk);
        return -1;
    }

    fd_core = vfs_open("/boot/grub/i386-pc/core.img", 0);
    if (fd_core < 0) {
        vfs_close_fd(fd_disk);
        return -1;
    }

    off = 0;
    while ((r = vfs_read_fd(fd_core, core_buf, BUF_SIZE)) > 0) {
        if (vfs_write_fd(fd_disk, core_buf, r) != r) {
            vfs_close_fd(fd_core);
            vfs_close_fd(fd_disk);
            return -1;
        }
        off += r;
    }

    vfs_close_fd(fd_core);
    vfs_close_fd(fd_disk);
    return 0;
}

static int inst_install_boot(const char *mountpoint, const char *disk_dev, const char *part_dev)
{
    char boot_dir[MAX_PATH];
    char grub_dir[MAX_PATH];
    char cfg_path[MAX_PATH];
    int fd;
    char grub_cfg[512];
    int written;

    snprintf(grub_cfg, sizeof(grub_cfg),
        "set timeout=5\n"
        "set default=0\n"
        "\n"
        "menuentry \"Lebirun\" {\n"
        "\tmultiboot2 /boot/lebirun.kernel root=%s\n"
        "\tboot\n"
        "}\n", part_dev);

    snprintf(boot_dir, sizeof(boot_dir), "%s/boot", mountpoint);
    vfs_mkdir(boot_dir, 0755);
    snprintf(grub_dir, sizeof(grub_dir), "%s/boot/grub", mountpoint);
    vfs_mkdir(grub_dir, 0755);

    snprintf(cfg_path, sizeof(cfg_path), "%s/boot/grub/grub.cfg", mountpoint);
    vfs_create(cfg_path, 0644);
    fd = vfs_open(cfg_path, 2);
    if (fd < 0) {
        fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd >= 0) {
        written = vfs_write_fd(fd, grub_cfg, strlen(grub_cfg));
        if (written < 0)
            write(fd, grub_cfg, strlen(grub_cfg));
        vfs_close_fd(fd);
    }

    if (inst_install_grub_mbr(disk_dev) < 0) {
        return -1;
    }

    return 0;
}

#define SALT_LEN 8

static void inst_generate_salt(char *salt, int len)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789./";
    unsigned int ticks;
    unsigned int seed;
    int i;

    ticks = getticks();
    seed = ticks ^ (ticks >> 16) ^ (unsigned int)getpid();

    for (i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;
        salt[i] = charset[(seed >> 16) % (sizeof(charset) - 1)];
    }
    salt[len] = '\0';
}

static const char *inst_hash_password(const char *password)
{
    static char salt[3 + SALT_LEN + 2];
    const char *hashed;

    memcpy(salt, "$5$", 3);
    inst_generate_salt(salt + 3, SALT_LEN);
    salt[3 + SALT_LEN] = '$';
    salt[3 + SALT_LEN + 1] = '\0';

    hashed = crypt(password, salt);
    return hashed;
}

static int inst_create_user(const char *mountpoint, const char *username, const char *password, int uid)
{
    char path[MAX_PATH];
    char line[256];
    int fd;
    int wlen;
    const char *hashed;

    snprintf(path, sizeof(path), "%s/home/%s", mountpoint, username);
    vfs_mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/etc/passwd", mountpoint);
    fd = vfs_open(path, 2);
    if (fd < 0) return -1;

    {
        uint64_t fsize;
        uint64_t ftype;
        vfs_stat(fd, &fsize, &ftype);
        lseek(fd, (off_t)fsize, SEEK_SET);
    }

    snprintf(line, sizeof(line), "%s:x:%d:%d:%s:/home/%s:/bin/lsh\n",
             username, uid, uid, username, username);
    wlen = (int)strlen(line);
    vfs_write_fd(fd, line, wlen);
    vfs_close_fd(fd);

    hashed = inst_hash_password(password);

    snprintf(path, sizeof(path), "%s/etc/shadow", mountpoint);
    fd = vfs_open(path, 2);
    if (fd < 0) {
        vfs_create(path, 0600);
        fd = vfs_open(path, 2);
        if (fd < 0) return -1;
    }

    {
        uint64_t fsize;
        uint64_t ftype;
        vfs_stat(fd, &fsize, &ftype);
        lseek(fd, (off_t)fsize, SEEK_SET);
    }

    snprintf(line, sizeof(line), "%s:%s:0:0:99999:7:::\n", username, hashed ? hashed : "!");
    wlen = (int)strlen(line);
    vfs_write_fd(fd, line, wlen);
    vfs_close_fd(fd);

    return 0;
}

static int inst_write_timezone(const char *mountpoint, const char *tz)
{
    char path[MAX_PATH];
    int fd;
    int len;

    snprintf(path, sizeof(path), "%s/etc/timezone", mountpoint);
    vfs_create(path, 0644);
    fd = vfs_open(path, 2);
    if (fd < 0) return -1;
    len = (int)strlen(tz);
    vfs_write_fd(fd, tz, len);
    vfs_write_fd(fd, "\n", 1);
    vfs_close_fd(fd);

    snprintf(path, sizeof(path), "%s/etc/environment", mountpoint);
    {
        char envline[128];
        vfs_create(path, 0644);
        fd = vfs_open(path, 2);
        if (fd < 0) return -1;
        snprintf(envline, sizeof(envline), "TZ=%s\n", tz);
        vfs_write_fd(fd, envline, (int)strlen(envline));
        vfs_close_fd(fd);
    }

    return 0;
}

static void cleanup_exit(void)
{
    tui_show_cursor();
    printf("\033[?1049l");
    fflush(stdout);
    tui_restore_mode();
}

#define STEP_DISK    0
#define STEP_PART    1
#define STEP_FORMAT  2
#define STEP_USER    3
#define STEP_ROOTPW  4
#define STEP_TZ      5
#define STEP_INSTALL 6
#define STEP_COUNT   7

#define STEP_NONE    0
#define STEP_DONE    1

static int step_disk(int *disk_idx)
{
    char *disk_items[MAX_DISKS];
    char disk_labels[MAX_DISKS][64];
    char sizebuf[32];
    int i;
    int choice;

    for (i = 0; i < disk_count; i++) {
        inst_format_size(disks[i].disk_sectors, sizebuf, sizeof(sizebuf));
        snprintf(disk_labels[i], sizeof(disk_labels[i]),
                 "%-10s  %8s  %d partition(s)",
                 disks[i].devpath, sizebuf, disks[i].part_count);
        disk_items[i] = disk_labels[i];
    }

    choice = tui_menu("Select Disk", (const char **)disk_items, disk_count,
                       " \x18\x19 Move  <Enter> Select  <Esc> Back");
    if (choice < 0) return -1;

    if (disks[choice].part_count == 0) {
        tui_msgbox("No Partitions",
                   "No partitions on this disk. Use ldiskutil first.");
        return -1;
    }

    *disk_idx = choice;
    return 0;
}

static int step_partition(int disk_idx, int *part_idx)
{
    disk_info_t *d;
    char *part_items[MAX_PARTS];
    char part_labels[MAX_PARTS][64];
    char sizebuf[32];
    int i;
    int choice;
    const char *type_name;

    d = &disks[disk_idx];
    for (i = 0; i < d->part_count; i++) {
        inst_format_size(d->parts[i].sector_count, sizebuf, sizeof(sizebuf));
        switch (d->parts[i].mbr_type) {
        case 0x83: type_name = "Linux"; break;
        case 0x82: type_name = "Swap"; break;
        case 0x0B: case 0x0C: type_name = "FAT32"; break;
        case 0x07: type_name = "NTFS"; break;
        default: type_name = "Other"; break;
        }
        snprintf(part_labels[i], sizeof(part_labels[i]),
                 "%-14s  %8s  %s",
                 d->parts[i].devpath, sizebuf, type_name);
        part_items[i] = part_labels[i];
    }

    choice = tui_menu("Select Partition", (const char **)part_items,
                       d->part_count,
                       " \x18\x19 Move  <Enter> Select  <Esc> Back");
    if (choice < 0) return -1;

    *part_idx = choice;
    return 0;
}

static int step_format(int *do_format)
{
    *do_format = tui_confirm("Format", "Format partition as ext4?");
    return 0;
}

#define MAX_USERS 8

typedef struct {
    char username[64];
    char password[64];
} user_entry_t;

static user_entry_t users[MAX_USERS];
static int user_count;
static char root_password[64];

static int step_user_setup(void)
{
    char *menu_items[MAX_USERS + 2];
    char menu_labels[MAX_USERS + 2][48];
    int choice;
    int i;
    char password2[64];

    for (;;) {
        for (i = 0; i < user_count; i++) {
            snprintf(menu_labels[i], sizeof(menu_labels[i]), "  %s", users[i].username);
            menu_items[i] = menu_labels[i];
        }
        if (user_count < MAX_USERS) {
            snprintf(menu_labels[user_count], sizeof(menu_labels[user_count]), "  Add new user...");
            menu_items[user_count] = menu_labels[user_count];
        }
        snprintf(menu_labels[user_count + (user_count < MAX_USERS ? 1 : 0)],
                 sizeof(menu_labels[0]), "  Done");
        menu_items[user_count + (user_count < MAX_USERS ? 1 : 0)] =
            menu_labels[user_count + (user_count < MAX_USERS ? 1 : 0)];

        choice = tui_menu("User Accounts", (const char **)menu_items,
                           user_count + (user_count < MAX_USERS ? 2 : 1),
                           " \x18\x19 Move  <Enter> Select  <Esc> Back");
        if (choice < 0) return 0;

        if (choice == user_count + (user_count < MAX_USERS ? 1 : 0)) {
            return 0;
        }

        if (user_count < MAX_USERS && choice == user_count) {
            if (tui_input("New User", "Enter username:", users[user_count].username,
                          sizeof(users[user_count].username), 0) < 0)
                continue;
            if (users[user_count].username[0] == '\0') {
                tui_msgbox("Error", "Username cannot be empty.");
                continue;
            }
            for (;;) {
                if (tui_input("New User", "Enter password:",
                              users[user_count].password,
                              sizeof(users[user_count].password), 1) < 0)
                    break;
                if (users[user_count].password[0] == '\0') {
                    tui_msgbox("Error", "Password cannot be empty.");
                    continue;
                }
                if (tui_input("New User", "Confirm password:",
                              password2, sizeof(password2), 1) < 0)
                    break;
                if (strcmp(users[user_count].password, password2) != 0) {
                    tui_msgbox("Error", "Passwords do not match.");
                    continue;
                }
                memset(password2, 0, sizeof(password2));
                user_count++;
                break;
            }
        } else if (choice < user_count) {
            if (tui_confirm("Remove User", users[choice].username)) {
                memset(users[choice].password, 0, sizeof(users[choice].password));
                for (i = choice; i < user_count - 1; i++)
                    users[i] = users[i + 1];
                user_count--;
            }
        }
    }
}

static int step_rootpw(void)
{
    char pw1[64];
    char pw2[64];

    for (;;) {
        if (tui_input("Root Password", "Enter root password:", pw1, sizeof(pw1), 1) < 0)
            return -1;
        if (pw1[0] == '\0') {
            tui_msgbox("Error", "Password cannot be empty.");
            continue;
        }
        if (tui_input("Root Password", "Confirm root password:", pw2, sizeof(pw2), 1) < 0)
            return -1;
        if (strcmp(pw1, pw2) != 0) {
            tui_msgbox("Error", "Passwords do not match.");
            continue;
        }
        strncpy(root_password, pw1, sizeof(root_password) - 1);
        root_password[sizeof(root_password) - 1] = '\0';
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        return 0;
    }
}

static int step_timezone(int *tz_idx)
{
    int tz_count;
    int choice;

    tz_count = 0;
    while (timezones[tz_count]) tz_count++;

    choice = tui_menu("Select Timezone", timezones, tz_count,
                       " \x18\x19 Move  <Enter> Select  <Esc> Back");
    if (choice < 0) return -1;

    *tz_idx = choice;
    return 0;
}

static int step_do_install(int disk_idx, int part_idx, int do_format,
                           int tz_idx)
{
    disk_info_t *d;
    part_info_t *p;
    char mountpoint[MAX_PATH];
    char donemsg[128];
    char logbuf[64];
    int i;

    d = &disks[disk_idx];
    p = &d->parts[part_idx];

    tui_progress_reset();
    tui_progress("Installing", "Preparing...", 0);
    usleep(50000);

    if (do_format) {
        int fret;
        char fmsg[128];
        tui_progress("Installing", "Formatting partition...", 0);
        snprintf(fmsg, sizeof(fmsg), "Formatting %s as ext4...", p->devpath);
        tui_log(fmsg);
        fret = inst_format_ext4(p->devpath);
        if (fret != 0) {
            snprintf(fmsg, sizeof(fmsg), "Failed to format partition (status=%d, path=%s).", fret, p->devpath);
            tui_msgbox("Error", fmsg);
            return -1;
        }
        tui_log("Format complete.");
    }

    snprintf(mountpoint, sizeof(mountpoint), "/tmp/lebinstall");
    vfs_mkdir("/tmp", 0755);
    vfs_mkdir(mountpoint, 0755);
    inst_umount_partition(mountpoint);

    tui_progress("Installing", "Mounting partition...", 5);
    snprintf(logbuf, sizeof(logbuf), "Mounting %s...", p->devpath);
    tui_log(logbuf);
    if (inst_mount_partition(p->devpath, mountpoint) != 0) {
        tui_msgbox("Error", "Failed to mount partition.");
        return -1;
    }
    tui_log("Partition mounted.");

    tui_progress("Installing", "Counting files...", 10);
    tui_log("Counting files to copy...");
    copy_total = inst_count_rootfs();
    if (copy_total < 1) copy_total = 100;
    copy_done = 0;

    tui_log("Copying rootfs...");
    if (inst_copy_rootfs(mountpoint) < 0) {
        tui_log("Warning: some files could not be copied.");
    }
    tui_log("Rootfs copy complete.");

    tui_progress("Installing", "Installing bootloader...", 96);
    tui_log("Installing GRUB bootloader...");
    if (inst_install_boot(mountpoint, d->devpath, p->devpath) < 0) {
        tui_log("Warning: bootloader had errors.");
    } else {
        tui_log("Bootloader installed.");
    }

    for (i = 0; i < user_count; i++) {
        tui_progress("Installing", "Creating user accounts...", 97);
        snprintf(logbuf, sizeof(logbuf), "Creating user: %s", users[i].username);
        tui_log(logbuf);
        inst_create_user(mountpoint, users[i].username, users[i].password, 1000 + i);
    }

    if (root_password[0] != '\0') {
        char shadow_path[MAX_PATH];
        char line[256];
        static char shadow_buf[4096];
        static char new_shadow[4096];
        const char *hashed;
        int shadow_fd;
        int rlen;
        int wlen;
        int new_len;
        int line_start;
        int line_end;
        int j;

        tui_progress("Installing", "Setting root password...", 98);
        hashed = inst_hash_password(root_password);
        if (hashed) {
            snprintf(shadow_path, sizeof(shadow_path), "%s/etc/shadow", mountpoint);
            shadow_fd = vfs_open(shadow_path, 0);
            if (shadow_fd >= 0) {
                rlen = vfs_read_fd(shadow_fd, shadow_buf, sizeof(shadow_buf) - 1);
                vfs_close_fd(shadow_fd);
                if (rlen > 0) {
                    shadow_buf[rlen] = '\0';
                    new_len = 0;
                    j = 0;
                    while (j < rlen) {
                        line_start = j;
                        while (j < rlen && shadow_buf[j] != '\n') j++;
                        line_end = j;
                        if (j < rlen) j++;
                        if (line_end - line_start < (int)sizeof(line) - 1) {
                            memcpy(line, shadow_buf + line_start, line_end - line_start);
                            line[line_end - line_start] = '\0';
                        } else {
                            line[0] = '\0';
                        }
                        if (strncmp(line, "root:", 5) == 0) {
                            wlen = snprintf(new_shadow + new_len,
                                sizeof(new_shadow) - new_len,
                                "root:%s:0:0:99999:7:::\n", hashed);
                        } else {
                            wlen = snprintf(new_shadow + new_len,
                                sizeof(new_shadow) - new_len,
                                "%s\n", line);
                        }
                        new_len += wlen;
                        if (new_len >= (int)sizeof(new_shadow) - 2) break;
                    }
                    shadow_fd = vfs_open(shadow_path, 2);
                    if (shadow_fd >= 0) {
                        vfs_write_fd(shadow_fd, new_shadow, new_len);
                        vfs_close_fd(shadow_fd);
                    }
                }
            }
        }
        memset(root_password, 0, sizeof(root_password));
        tui_log("Root password updated.");
    }

    tui_progress("Installing", "Setting timezone...", 99);
    snprintf(logbuf, sizeof(logbuf), "Timezone: %s", tz_values[tz_idx]);
    tui_log(logbuf);
    inst_write_timezone(mountpoint, tz_values[tz_idx]);

    tui_log("Unmounting...");
    inst_umount_partition(mountpoint);

    tui_progress("Installing", "Installation complete!", 100);
    tui_log("Done!");

    snprintf(donemsg, sizeof(donemsg),
             "Lebirun installed to %s. Reboot to start.",
             p->devpath);
    tui_msgbox("Complete", donemsg);

    return 0;
}

int main(int argc, char **argv)
{
    static const char *step_names[] = {
        "Select Disk",
        "Select Partition",
        "Format Partition",
        "User Accounts",
        "Root Password",
        "Select Timezone",
        "Install"
    };
    int status[STEP_COUNT];
    char labels[STEP_COUNT][56];
    char *items[STEP_COUNT];
    int disk_idx;
    int part_idx;
    int do_format;
    int tz_idx;
    int sel;
    int i;
    int ret;
    char ubuf[32];

    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IOFBF, 8192);

    tui_get_size();
    tui_raw_mode();
    printf("\033[?1049h");
    fflush(stdout);
    tui_hide_cursor();

    if (getuid() != 0) {
        tui_msgbox("Error", "This installer must be run as root.");
        cleanup_exit();
        return 1;
    }

    tui_progress_reset();
    tui_progress("Scanning", "Scanning for disks...", 0);

    if (inst_enumerate_disks() <= 0) {
        tui_msgbox("Error", "No disks found.");
        cleanup_exit();
        return 1;
    }

    for (i = 0; i < STEP_COUNT; i++) status[i] = STEP_NONE;
    disk_idx = -1;
    part_idx = -1;
    do_format = 0;
    tz_idx = 0;
    user_count = 0;
    root_password[0] = '\0';

    for (;;) {
        for (i = 0; i < STEP_COUNT; i++) {
            if (i == STEP_INSTALL) {
                if (status[STEP_DISK] == STEP_DONE && status[STEP_PART] == STEP_DONE)
                    snprintf(labels[i], sizeof(labels[i]), "  [>] %s", step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s  (need disk & partition)", step_names[i]);
            } else if (status[i] == STEP_DONE) {
                if (i == STEP_DISK)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], disks[disk_idx].devpath);
                else if (i == STEP_PART)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], disks[disk_idx].parts[part_idx].devpath);
                else if (i == STEP_FORMAT)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], do_format ? "ext4" : "No");
                else if (i == STEP_USER) {
                    snprintf(ubuf, sizeof(ubuf), "%d user(s)", user_count);
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], ubuf);
                } else if (i == STEP_TZ)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], timezones[tz_idx]);
                else if (i == STEP_ROOTPW)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (set)", step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s", step_names[i]);
            } else {
                if (i == STEP_FORMAT || i == STEP_USER || i == STEP_TZ || i == STEP_ROOTPW)
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s  (optional)", step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s", step_names[i]);
            }
            items[i] = labels[i];
        }

        sel = tui_menu("Installation Steps", (const char **)items, STEP_COUNT,
                        " \x18\x19 Move  <Enter> Select  <Esc> Quit");
        if (sel < 0) {
            if (tui_confirm("Quit", "Exit the installer?")) {
                cleanup_exit();
                return 0;
            }
            continue;
        }

        switch (sel) {
        case STEP_DISK:
            ret = step_disk(&disk_idx);
            if (ret == 0) {
                status[STEP_DISK] = STEP_DONE;
                status[STEP_PART] = STEP_NONE;
                part_idx = -1;
            }
            break;

        case STEP_PART:
            if (status[STEP_DISK] != STEP_DONE) {
                tui_msgbox("Error", "Select a disk first.");
                break;
            }
            ret = step_partition(disk_idx, &part_idx);
            if (ret == 0)
                status[STEP_PART] = STEP_DONE;
            break;

        case STEP_FORMAT:
            ret = step_format(&do_format);
            if (ret == 0)
                status[STEP_FORMAT] = STEP_DONE;
            break;

        case STEP_USER:
            step_user_setup();
            if (user_count > 0)
                status[STEP_USER] = STEP_DONE;
            else
                status[STEP_USER] = STEP_NONE;
            break;

        case STEP_ROOTPW:
            ret = step_rootpw();
            if (ret == 0)
                status[STEP_ROOTPW] = STEP_DONE;
            break;

        case STEP_TZ:
            ret = step_timezone(&tz_idx);
            if (ret == 0)
                status[STEP_TZ] = STEP_DONE;
            break;

        case STEP_INSTALL:
            if (status[STEP_DISK] != STEP_DONE || status[STEP_PART] != STEP_DONE) {
                tui_msgbox("Error", "Select a disk and partition first.");
                break;
            }
            if (!tui_confirm("Confirm", "Proceed with installation?"))
                break;
            ret = step_do_install(disk_idx, part_idx, do_format, tz_idx);
            if (ret == 0) {
                for (i = 0; i < user_count; i++)
                    memset(users[i].password, 0, sizeof(users[i].password));
                memset(root_password, 0, sizeof(root_password));
                cleanup_exit();
                return 0;
            }
            break;
        }
    }
}

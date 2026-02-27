#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

#define FILE_FLAG_READ   0x01
#define FILE_FLAG_WRITE  0x02
#define FILE_FLAG_EOF    0x04
#define FILE_FLAG_ERROR  0x08
#define FILE_FLAG_MYBUF  0x10
#define FILE_FLAG_APPEND 0x20

#define STDIO_BUFSIZE 1024
#define MAX_OPEN_FILES 64

struct _IO_FILE {
    int fd;
    int flags;
    int ungetc_buf;
    int has_ungetc;
    unsigned char *buf;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_len;
    long file_pos;
};

static struct _IO_FILE _stdin_file  = { .fd = 0, .flags = FILE_FLAG_READ };
static struct _IO_FILE _stdout_file = { .fd = 1, .flags = FILE_FLAG_WRITE };
static struct _IO_FILE _stderr_file = { .fd = 2, .flags = FILE_FLAG_WRITE };

FILE *const stdin  = (FILE *)&_stdin_file;
FILE *const stdout = (FILE *)&_stdout_file;
FILE *const stderr = (FILE *)&_stderr_file;

static struct _IO_FILE *open_files[MAX_OPEN_FILES];

static struct _IO_FILE *alloc_file(void) {
    struct _IO_FILE *f;
    int i;

    f = (struct _IO_FILE *)malloc(sizeof(struct _IO_FILE));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->fd = -1;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i]) {
            open_files[i] = f;
            return f;
        }
    }
    free(f);
    return NULL;
}

static void free_file(struct _IO_FILE *f) {
    int i;

    if (!f) return;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i] == f) {
            open_files[i] = NULL;
            break;
        }
    }
    if (f->buf && (f->flags & FILE_FLAG_MYBUF)) {
        free(f->buf);
    }
    free(f);
}

int getchar(void) {
    return fgetc(stdin);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fflush(FILE *stream) {
    struct _IO_FILE *f;

    if (!stream) return 0;
    f = (struct _IO_FILE *)stream;
    if (f->fd < 0) return EOF;
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    int flags;
    int oflags;
    int fd;
    struct _IO_FILE *f;

    if (!path || !mode) return NULL;
    
    flags = 0;
    oflags = 0;
    
    switch (mode[0]) {
        case 'r':
            flags = FILE_FLAG_READ;
            oflags = O_RDONLY;
            if (mode[1] == '+' || (mode[1] && mode[2] == '+')) {
                flags |= FILE_FLAG_WRITE;
                oflags = O_RDWR;
            }
            break;
        case 'w':
            flags = FILE_FLAG_WRITE;
            oflags = O_WRONLY | O_CREAT | O_TRUNC;
            if (mode[1] == '+' || (mode[1] && mode[2] == '+')) {
                flags |= FILE_FLAG_READ;
                oflags = O_RDWR | O_CREAT | O_TRUNC;
            }
            break;
        case 'a':
            flags = FILE_FLAG_WRITE | FILE_FLAG_APPEND;
            oflags = O_WRONLY | O_CREAT | O_APPEND;
            if (mode[1] == '+' || (mode[1] && mode[2] == '+')) {
                flags |= FILE_FLAG_READ;
                oflags = O_RDWR | O_CREAT | O_APPEND;
            }
            break;
        default:
            return NULL;
    }
    
    fd = open(path, oflags);
    if (fd < 0) return NULL;
    
    f = alloc_file();
    if (!f) {
        close(fd);
        return NULL;
    }
    
    f->fd = fd;
    f->flags = flags;
    f->file_pos = 0;
    f->buf = (unsigned char *)malloc(STDIO_BUFSIZE);
    if (f->buf) {
        f->flags |= FILE_FLAG_MYBUF;
        f->buf_size = STDIO_BUFSIZE;
    }
    
    return (FILE *)f;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (stream) fclose(stream);
    if (!path) return NULL;
    return fopen(path, mode);
}

FILE *fdopen(int fd, const char *mode) {
    int flags;
    struct _IO_FILE *f;

    if (fd < 0 || !mode) return NULL;
    
    flags = 0;
    switch (mode[0]) {
        case 'r': flags = FILE_FLAG_READ; break;
        case 'w': flags = FILE_FLAG_WRITE; break;
        case 'a': flags = FILE_FLAG_WRITE | FILE_FLAG_APPEND; break;
        default: return NULL;
    }
    if (mode[1] == '+') flags |= (FILE_FLAG_READ | FILE_FLAG_WRITE);
    
    f = alloc_file();
    if (!f) return NULL;
    
    f->fd = fd;
    f->flags = flags;
    return (FILE *)f;
}

int fclose(FILE *stream) {
    struct _IO_FILE *f;
    int ret;

    if (!stream) return EOF;
    f = (struct _IO_FILE *)stream;
    
    if (f == &_stdin_file || f == &_stdout_file || f == &_stderr_file) {
        return 0;
    }
    
    fflush(stream);
    ret = 0;
    if (f->fd >= 0) {
        ret = close(f->fd);
    }
    free_file(f);
    return ret;
}

int fileno(FILE *stream) {
    struct _IO_FILE *f;

    if (!stream) return -1;
    f = (struct _IO_FILE *)stream;
    return f->fd;
}

int feof(FILE *stream) {
    struct _IO_FILE *f;

    if (!stream) return 0;
    f = (struct _IO_FILE *)stream;
    return (f->flags & FILE_FLAG_EOF) ? 1 : 0;
}

int ferror(FILE *stream) {
    struct _IO_FILE *f;

    if (!stream) return 0;
    f = (struct _IO_FILE *)stream;
    return (f->flags & FILE_FLAG_ERROR) ? 1 : 0;
}

void clearerr(FILE *stream) {
    struct _IO_FILE *f;

    if (!stream) return;
    f = (struct _IO_FILE *)stream;
    f->flags &= ~(FILE_FLAG_EOF | FILE_FLAG_ERROR);
}

int fseek(FILE *stream, long offset, int whence) {
    struct _IO_FILE *f;
    off_t ret;

    if (!stream) return -1;
    f = (struct _IO_FILE *)stream;
    if (f->fd < 0) return -1;
    
    f->has_ungetc = 0;
    f->buf_pos = 0;
    f->buf_len = 0;
    
    ret = lseek(f->fd, offset, whence);
    if (ret < 0) {
        f->flags |= FILE_FLAG_ERROR;
        return -1;
    }
    f->file_pos = ret;
    f->flags &= ~FILE_FLAG_EOF;
    return 0;
}

int fseeko(FILE *stream, off_t offset, int whence) {
    return fseek(stream, (long)offset, whence);
}

long ftell(FILE *stream) {
    struct _IO_FILE *f;
    off_t pos;

    if (!stream) return -1;
    f = (struct _IO_FILE *)stream;
    if (f->fd < 0) return -1;
    
    pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    
    pos -= f->buf_len - f->buf_pos;
    if (f->has_ungetc) pos--;
    
    return (long)pos;
}

off_t ftello(FILE *stream) {
    return (off_t)ftell(stream);
}

void rewind(FILE *stream) {
    if (stream) {
        fseek(stream, 0, SEEK_SET);
        clearerr(stream);
    }
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    struct _IO_FILE *f;
    unsigned char *dest;
    size_t total;
    size_t done;
    size_t avail;
    size_t tocopy;
    ssize_t n;

    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    if (nmemb > (size_t)-1 / size) return 0;
    f = (struct _IO_FILE *)stream;
    if (f->fd < 0 || !(f->flags & FILE_FLAG_READ)) return 0;
    
    dest = (unsigned char *)ptr;
    total = size * nmemb;
    done = 0;
    
    if (f->has_ungetc && done < total) {
        *dest++ = (unsigned char)f->ungetc_buf;
        f->has_ungetc = 0;
        done++;
    }
    
    while (done < total) {
        if (f->buf && f->buf_pos < f->buf_len) {
            avail = f->buf_len - f->buf_pos;
            tocopy = (total - done < avail) ? (total - done) : avail;
            memcpy(dest, f->buf + f->buf_pos, tocopy);
            f->buf_pos += tocopy;
            dest += tocopy;
            done += tocopy;
        } else {
            if (f->buf && f->buf_size > 0) {

                n = read(f->fd, f->buf, f->buf_size);
                if (n <= 0) {
                    if (n == 0) f->flags |= FILE_FLAG_EOF;
                    else f->flags |= FILE_FLAG_ERROR;
                    break;
                }
                f->buf_len = (size_t)n;
                f->buf_pos = 0;
            } else {
                n = read(f->fd, dest, total - done);
                if (n <= 0) {
                    if (n == 0) f->flags |= FILE_FLAG_EOF;
                    else f->flags |= FILE_FLAG_ERROR;
                    break;
                }
                dest += n;
                done += n;
            }
        }
    }
    
    return done / size;
}

size_t fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fread(ptr, size, nmemb, stream);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    struct _IO_FILE *f;
    int fd;
    size_t total;
    ssize_t w;

    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    if (nmemb > (size_t)-1 / size) return 0;
    f = (struct _IO_FILE *)stream;
    
    if (f == &_stdin_file) fd = 0;
    else if (f == &_stdout_file) fd = 1;
    else if (f == &_stderr_file) fd = 2;
    else {
        if (f->fd < 0 || !(f->flags & FILE_FLAG_WRITE)) return 0;
        fd = f->fd;
    }
    
    total = size * nmemb;
    w = write(fd, ptr, total);
    if (w <= 0) {
        f->flags |= FILE_FLAG_ERROR;
        return 0;
    }
    return (size_t)w / size;
}

size_t fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

int fgetc(FILE *stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) != 1) return EOF;
    return c;
}

int fgetc_unlocked(FILE *stream) {
    return fgetc(stream);
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int getc_unlocked(FILE *stream) {
    return fgetc(stream);
}

int ungetc(int c, FILE *stream) {
    struct _IO_FILE *f;

    if (!stream || c == EOF) return EOF;
    f = (struct _IO_FILE *)stream;
    if (f->has_ungetc) return EOF;
    f->ungetc_buf = c;
    f->has_ungetc = 1;
    f->flags &= ~FILE_FLAG_EOF;
    return c;
}

int fputc(int c, FILE *stream) {
    unsigned char ch;

    ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, stream) != 1) return EOF;
    return ch;
}

int fputc_unlocked(int c, FILE *stream) {
    return fputc(c, stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putc_unlocked(int c, FILE *stream) {
    return fputc(c, stream);
}

char *fgets(char *s, int n, FILE *stream) {
    int i;
    int c;

    if (!s || n <= 0 || !stream) return NULL;
    
    i = 0;
    while (i < n - 1) {
        c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

char *fgets_unlocked(char *s, int n, FILE *stream) {
    return fgets(s, n, stream);
}

int fputs(const char *s, FILE *stream) {
    size_t len;

    if (!s || !stream) return EOF;
    len = strlen(s);
    if (fwrite(s, 1, len, stream) != len) return EOF;
    return 0;
}

int fputs_unlocked(const char *s, FILE *stream) {
    return fputs(s, stream);
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void setlinebuf(FILE *stream) {
    setvbuf(stream, NULL, _IOLBF, 0);
}

void setbuffer(FILE *stream, char *buf, size_t size) {
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, size);
}

int feof_unlocked(FILE *stream) { return feof(stream); }
int ferror_unlocked(FILE *stream) { return ferror(stream); }
int fileno_unlocked(FILE *stream) { return fileno(stream); }
void clearerr_unlocked(FILE *stream) { clearerr(stream); }
int fflush_unlocked(FILE *stream) { return fflush(stream); }

void flockfile(FILE *stream) { (void)stream; }
int ftrylockfile(FILE *stream) { (void)stream; return 0; }
void funlockfile(FILE *stream) { (void)stream; }

int getchar_unlocked(void) { return getchar(); }
int putchar_unlocked(int c) { return putchar(c); }

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    int ret;

    if (!stream || !format) return -1;
    va_start(ap, format);
    ret = vfprintf(stream, format, ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[1024];
    int len;
    size_t w;

    if (!stream || !format) return -1;
    len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        w = fwrite(buf, 1, (size_t)len, stream);
        return (int)w;
    }
    return len;
}

int printf(const char *format, ...) {
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vfprintf(stdout, format, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    char *out;
    char *end;
    const char *p;
    int width, precision, left_align, zero_pad, long_arg;
    char tmp[32];
    int tmplen;

    if (!str || size == 0 || !format) return 0;

    out = str;
    end = str + size - 1;
    p = format;

    while (*p && out < end) {
        if (*p == '%') {
            p++;
            width = 0;
            precision = -1;
            left_align = 0;
            zero_pad = 0;
            long_arg = 0;

            while (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') {
                if (*p == '-') left_align = 1;
                else if (*p == '0') zero_pad = 1;
                p++;
            }

            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }

            if (*p == '.') {
                p++;
                precision = 0;
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }

            if (*p == 'l') {
                long_arg = 1;
                p++;
                if (*p == 'l') { p++; }
            } else if (*p == 'z' || *p == 'h') {
                p++;
            }

            tmplen = 0;
            (void)left_align; (void)zero_pad; (void)precision;

            switch (*p) {
                case 'd':
                case 'i': {
                    long val = long_arg ? va_arg(ap, long) : va_arg(ap, int);
                    int neg = val < 0;
                    if (neg) val = -val;
                    if (val == 0) tmp[tmplen++] = '0';
                    else while (val) { tmp[tmplen++] = '0' + (val % 10); val /= 10; }
                    while (tmplen < width) tmp[tmplen++] = zero_pad ? '0' : ' ';
                    if (neg && out < end) *out++ = '-';
                    while (tmplen > 0 && out < end) *out++ = tmp[--tmplen];
                    break;
                }
                case 'u': {
                    unsigned long val = long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                    if (val == 0) tmp[tmplen++] = '0';
                    else while (val) { tmp[tmplen++] = '0' + (val % 10); val /= 10; }
                    while (tmplen < width) tmp[tmplen++] = zero_pad ? '0' : ' ';
                    while (tmplen > 0 && out < end) *out++ = tmp[--tmplen];
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned long val = long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                    const char *hex = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                    if (val == 0) tmp[tmplen++] = '0';
                    else while (val) { tmp[tmplen++] = hex[val & 0xF]; val >>= 4; }
                    while (tmplen < width) tmp[tmplen++] = zero_pad ? '0' : ' ';
                    while (tmplen > 0 && out < end) *out++ = tmp[--tmplen];
                    break;
                }
                case 'p': {
                    unsigned long val = (unsigned long)(uintptr_t)va_arg(ap, void*);
                    if (out < end) *out++ = '0';
                    if (out < end) *out++ = 'x';
                    if (val == 0) tmp[tmplen++] = '0';
                    else while (val) { tmp[tmplen++] = "0123456789abcdef"[val & 0xF]; val >>= 4; }
                    while (tmplen > 0 && out < end) *out++ = tmp[--tmplen];
                    break;
                }
                case 's': {
                    const char *s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    while (*s && out < end) *out++ = *s++;
                    break;
                }
                case 'c':
                    if (out < end) *out++ = (char)va_arg(ap, int);
                    break;
                case '%':
                    if (out < end) *out++ = '%';
                    break;
                case 'g':
                case 'f':
                case 'e': {
                    double val = va_arg(ap, double);
                    long ipart = (long)val;
                    int neg = ipart < 0;
                    double frac;
                    int i, d;
                    if (neg) ipart = -ipart;
                    if (ipart == 0) tmp[tmplen++] = '0';
                    else while (ipart) { tmp[tmplen++] = '0' + (ipart % 10); ipart /= 10; }
                    if (neg && out < end) *out++ = '-';
                    while (tmplen > 0 && out < end) *out++ = tmp[--tmplen];
                    if (out < end) *out++ = '.';
                    frac = val - (long)val;
                    if (frac < 0) frac = -frac;
                    for (i = 0; i < 6 && out < end; i++) {
                        frac *= 10;
                        d = (int)frac;
                        *out++ = '0' + d;
                        frac -= d;
                    }
                    break;
                }
                default:
                    if (out < end) *out++ = '%';
                    if (out < end) *out++ = *p;
                    break;
            }
        } else {
            *out++ = *p;
        }
        p++;
    }
    *out = '\0';
    return (int)(out - str);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsnprintf(str, (size_t)-1, format, ap);
    va_end(ap);
    return ret;
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, (size_t)-1, format, ap);
}

static const char *const sys_errlist[] = {
    [0] = "Success",
    [1] = "Operation not permitted",
    [2] = "No such file or directory",
    [3] = "No such process",
    [4] = "Interrupted system call",
    [5] = "Input/output error",
    [6] = "No such device or address",
    [7] = "Argument list too long",
    [8] = "Exec format error",
    [9] = "Bad file descriptor",
    [10] = "No child processes",
    [11] = "Resource temporarily unavailable",
    [12] = "Cannot allocate memory",
    [13] = "Permission denied",
    [14] = "Bad address",
    [15] = "Block device required",
    [16] = "Device or resource busy",
    [17] = "File exists",
    [18] = "Invalid cross-device link",
    [19] = "No such device",
    [20] = "Not a directory",
    [21] = "Is a directory",
    [22] = "Invalid argument",
    [23] = "Too many open files in system",
    [24] = "Too many open files",
    [25] = "Inappropriate ioctl for device",
    [26] = "Text file busy",
    [27] = "File too large",
    [28] = "No space left on device",
    [29] = "Illegal seek",
    [30] = "Read-only file system",
    [31] = "Too many links",
    [32] = "Broken pipe",
    [33] = "Numerical argument out of domain",
    [34] = "Numerical result out of range",
    [35] = "Resource deadlock avoided",
    [36] = "File name too long",
    [37] = "No locks available",
    [38] = "Function not implemented",
    [39] = "Directory not empty",
    [40] = "Too many levels of symbolic links",
};

#define SYS_NERR (sizeof(sys_errlist) / sizeof(sys_errlist[0]))

char *strerror(int errnum) {
    static char buf[64];
    if (errnum >= 0 && (size_t)errnum < SYS_NERR && sys_errlist[errnum]) {
        return (char *)sys_errlist[errnum];
    }
    snprintf(buf, sizeof(buf), "Unknown error %d", errnum);
    return buf;
}

int strerror_r(int errnum, char *buf, size_t buflen) {
    const char *msg;
    size_t len;

    msg = strerror(errnum);
    len = strlen(msg);
    if (len >= buflen) {
        if (buflen > 0) {
            memcpy(buf, msg, buflen - 1);
            buf[buflen - 1] = '\0';
        }
        return -1;
    }
    memcpy(buf, msg, len + 1);
    return 0;
}

void perror(const char *s) {
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

static const char *const sys_siglist[] = {
    [0] = "Unknown signal 0",
    [1] = "Hangup",
    [2] = "Interrupt",
    [3] = "Quit",
    [4] = "Illegal instruction",
    [5] = "Trace/breakpoint trap",
    [6] = "Aborted",
    [7] = "Bus error",
    [8] = "Floating point exception",
    [9] = "Killed",
    [10] = "User defined signal 1",
    [11] = "Segmentation fault",
    [12] = "User defined signal 2",
    [13] = "Broken pipe",
    [14] = "Alarm clock",
    [15] = "Terminated",
    [16] = "Stack fault",
    [17] = "Child exited",
    [18] = "Continued",
    [19] = "Stopped (signal)",
    [20] = "Stopped",
    [21] = "Stopped (tty input)",
    [22] = "Stopped (tty output)",
    [23] = "Urgent I/O condition",
    [24] = "CPU time limit exceeded",
    [25] = "File size limit exceeded",
    [26] = "Virtual timer expired",
    [27] = "Profiling timer expired",
    [28] = "Window changed",
    [29] = "I/O possible",
    [30] = "Power failure",
    [31] = "Bad system call",
};

#define SYS_NSIG (sizeof(sys_siglist) / sizeof(sys_siglist[0]))

char *strsignal(int signum) {
    static char buf[64];
    if (signum >= 0 && (size_t)signum < SYS_NSIG && sys_siglist[signum]) {
        return (char *)sys_siglist[signum];
    }
    snprintf(buf, sizeof(buf), "Unknown signal %d", signum);
    return buf;
}

int remove(const char *path) {
    return unlink(path);
}

int fgetpos(FILE *stream, fpos_t *pos) {
    long p;

    if (!stream || !pos) return -1;
    p = ftell(stream);
    if (p < 0) return -1;
    memset(pos, 0, sizeof(*pos));
    memcpy(pos, &p, sizeof(*pos) < sizeof(p) ? sizeof(*pos) : sizeof(p));
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos) {
    long p;

    if (!stream || !pos) return -1;
    p = 0;
    memcpy(&p, pos, sizeof(*pos) < sizeof(p) ? sizeof(*pos) : sizeof(p));
    return fseek(stream, p, SEEK_SET);
}

FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { (void)s; return NULL; }
char *tempnam(const char *dir, const char *pfx) { (void)dir; (void)pfx; return NULL; }

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    size_t pos;
    int c;
    
    if (!lineptr || !n || !stream) return -1;
    
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr) return -1;
    }
    
    pos = 0;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            char *newbuf;
            *n *= 2;
            newbuf = (char *)realloc(*lineptr, *n);
            if (!newbuf) return -1;
            *lineptr = newbuf;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }
    
    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
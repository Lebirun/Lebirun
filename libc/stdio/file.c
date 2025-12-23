#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#define FILE_BUF_SIZE 1024
#define MAX_OPEN_FILES 64

#define _FILE_READ    0x01
#define _FILE_WRITE   0x02
#define _FILE_APPEND  0x04
#define _FILE_EOF     0x08
#define _FILE_ERROR   0x10
#define _FILE_MYBUF   0x20
#define _FILE_UNBUF   0x40
#define _FILE_LINEBUF 0x80

struct _FILE {
    int fd;
    int flags;
    int ungetc_buf;
    int has_ungetc;
    unsigned char *buf;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_end;
    int buf_mode;
};

static unsigned char _stdin_buf[FILE_BUF_SIZE];
static unsigned char _stdout_buf[FILE_BUF_SIZE];
static unsigned char _stderr_buf[1];

static struct _FILE _stdin = {
    .fd = 0, .flags = _FILE_READ, .ungetc_buf = EOF, .has_ungetc = 0,
    .buf = _stdin_buf, .buf_size = FILE_BUF_SIZE, .buf_pos = 0, .buf_end = 0, .buf_mode = _IOFBF
};
static struct _FILE _stdout = {
    .fd = 1, .flags = _FILE_WRITE, .ungetc_buf = EOF, .has_ungetc = 0,
    .buf = _stdout_buf, .buf_size = FILE_BUF_SIZE, .buf_pos = 0, .buf_end = 0, .buf_mode = _IOLBF
};
static struct _FILE _stderr = {
    .fd = 2, .flags = _FILE_WRITE | _FILE_UNBUF, .ungetc_buf = EOF, .has_ungetc = 0,
    .buf = _stderr_buf, .buf_size = 1, .buf_pos = 0, .buf_end = 0, .buf_mode = _IONBF
};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

static FILE *open_files[MAX_OPEN_FILES];
static int num_open_files = 0;

int fflush(FILE *stream) {
    if (!stream) {
        for (int i = 0; i < num_open_files; i++) {
            if (open_files[i]) fflush(open_files[i]);
        }
        fflush(stdout);
        fflush(stderr);
        return 0;
    }
    if ((stream->flags & _FILE_WRITE) && stream->buf_pos > 0) {
        ssize_t written = write(stream->fd, stream->buf, stream->buf_pos);
        if (written < 0) {
            stream->flags |= _FILE_ERROR;
            return EOF;
        }
        stream->buf_pos = 0;
    }
    return 0;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    if (!stream) return -1;
    fflush(stream);
    if (stream->flags & _FILE_MYBUF) {
        free(stream->buf);
        stream->flags &= ~_FILE_MYBUF;
    }
    stream->buf_mode = mode;
    if (mode == _IONBF) {
        stream->flags |= _FILE_UNBUF;
        stream->buf = NULL;
        stream->buf_size = 0;
    } else {
        stream->flags &= ~_FILE_UNBUF;
        if (buf) {
            stream->buf = (unsigned char *)buf;
            stream->buf_size = size;
        } else if (size > 0) {
            stream->buf = malloc(size);
            if (!stream->buf) return -1;
            stream->buf_size = size;
            stream->flags |= _FILE_MYBUF;
        }
    }
    stream->buf_pos = 0;
    stream->buf_end = 0;
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void setbuffer(FILE *stream, char *buf, size_t size) {
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, size);
}

void setlinebuf(FILE *stream) {
    setvbuf(stream, NULL, _IOLBF, 0);
}

static int parse_mode(const char *mode, int *flags) {
    *flags = 0;
    int oflags = 0;
    switch (*mode++) {
        case 'r': *flags = _FILE_READ; oflags = O_RDONLY; break;
        case 'w': *flags = _FILE_WRITE; oflags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case 'a': *flags = _FILE_WRITE | _FILE_APPEND; oflags = O_WRONLY | O_CREAT | O_APPEND; break;
        default: return -1;
    }
    while (*mode) {
        if (*mode == '+') {
            *flags |= _FILE_READ | _FILE_WRITE;
            oflags = (oflags & ~O_ACCMODE) | O_RDWR;
        } else if (*mode == 'b') {
        }
        mode++;
    }
    return oflags;
}

FILE *fopen(const char *pathname, const char *mode) {
    int flags;
    int oflags = parse_mode(mode, &flags);
    if (oflags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(pathname, oflags, 0666);
    if (fd < 0) return NULL;
    FILE *f = malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        return NULL;
    }
    memset(f, 0, sizeof(FILE));
    f->fd = fd;
    f->flags = flags;
    f->ungetc_buf = EOF;
    f->has_ungetc = 0;
    f->buf = malloc(FILE_BUF_SIZE);
    if (!f->buf) {
        close(fd);
        free(f);
        return NULL;
    }
    f->buf_size = FILE_BUF_SIZE;
    f->flags |= _FILE_MYBUF;
    f->buf_mode = _IOFBF;
    if (num_open_files < MAX_OPEN_FILES) {
        open_files[num_open_files++] = f;
    }
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    int flags;
    parse_mode(mode, &flags);
    FILE *f = malloc(sizeof(FILE));
    if (!f) return NULL;
    memset(f, 0, sizeof(FILE));
    f->fd = fd;
    f->flags = flags;
    f->ungetc_buf = EOF;
    f->has_ungetc = 0;
    f->buf = malloc(FILE_BUF_SIZE);
    if (!f->buf) {
        free(f);
        return NULL;
    }
    f->buf_size = FILE_BUF_SIZE;
    f->flags |= _FILE_MYBUF;
    f->buf_mode = _IOFBF;
    if (num_open_files < MAX_OPEN_FILES) {
        open_files[num_open_files++] = f;
    }
    return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
    if (!stream) return NULL;
    fflush(stream);
    close(stream->fd);
    int flags;
    int oflags = parse_mode(mode, &flags);
    if (oflags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(pathname, oflags, 0666);
    if (fd < 0) return NULL;
    stream->fd = fd;
    stream->flags = flags;
    if (stream->flags & _FILE_MYBUF) {
        stream->flags |= _FILE_MYBUF;
    }
    stream->buf_pos = 0;
    stream->buf_end = 0;
    stream->has_ungetc = 0;
    stream->ungetc_buf = EOF;
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    fflush(stream);
    int ret = close(stream->fd);
    if (stream->flags & _FILE_MYBUF) {
        free(stream->buf);
    }
    for (int i = 0; i < num_open_files; i++) {
        if (open_files[i] == stream) {
            open_files[i] = open_files[--num_open_files];
            break;
        }
    }
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return ret;
}

static int fill_buffer(FILE *stream) {
    if (stream->flags & _FILE_EOF) return EOF;
    if (!stream->buf || stream->buf_size == 0) {
        char c;
        ssize_t r = read(stream->fd, &c, 1);
        if (r <= 0) {
            stream->flags |= (r == 0) ? _FILE_EOF : _FILE_ERROR;
            return EOF;
        }
        return (unsigned char)c;
    }
    ssize_t r = read(stream->fd, stream->buf, stream->buf_size);
    if (r <= 0) {
        stream->flags |= (r == 0) ? _FILE_EOF : _FILE_ERROR;
        return EOF;
    }
    stream->buf_pos = 0;
    stream->buf_end = r;
    return 0;
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    if (stream->has_ungetc) {
        stream->has_ungetc = 0;
        return stream->ungetc_buf;
    }
    if (stream->buf_pos >= stream->buf_end) {
        if (fill_buffer(stream) == EOF) return EOF;
    }
    if (!stream->buf || stream->buf_size == 0) {
        char c;
        ssize_t r = read(stream->fd, &c, 1);
        if (r <= 0) {
            stream->flags |= (r == 0) ? _FILE_EOF : _FILE_ERROR;
            return EOF;
        }
        return (unsigned char)c;
    }
    return (unsigned char)stream->buf[stream->buf_pos++];
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) return EOF;
    stream->has_ungetc = 1;
    stream->ungetc_buf = c;
    stream->flags &= ~_FILE_EOF;
    return c;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t newsize = *n * 2;
            char *newbuf = realloc(*lineptr, newsize);
            if (!newbuf) return -1;
            *lineptr = newbuf;
            *n = newsize;
        }
        (*lineptr)[pos++] = c;
        if (c == delim) break;
    }
    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return pos;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    if (stream->flags & _FILE_UNBUF) {
        ssize_t ret = write(stream->fd, &ch, 1);
        return (ret == 1) ? c : EOF;
    }
    if (!stream->buf) {
        ssize_t ret = write(stream->fd, &ch, 1);
        return (ret == 1) ? c : EOF;
    }
    stream->buf[stream->buf_pos++] = ch;
    int should_flush = 0;
    if (stream->buf_pos >= stream->buf_size) should_flush = 1;
    if (stream->buf_mode == _IOLBF && ch == '\n') should_flush = 1;
    if (should_flush) {
        if (fflush(stream) == EOF) return EOF;
    }
    return c;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    while (*s) {
        if (fputc(*s++, stream) == EOF) return EOF;
    }
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    unsigned char *p = ptr;
    size_t total = size * nmemb;
    size_t done = 0;
    while (done < total) {
        int c = fgetc(stream);
        if (c == EOF) break;
        p[done++] = c;
    }
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    const unsigned char *p = ptr;
    size_t total = size * nmemb;
    size_t done = 0;
    while (done < total) {
        if (fputc(p[done], stream) == EOF) break;
        done++;
    }
    return done / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    fflush(stream);
    stream->buf_pos = 0;
    stream->buf_end = 0;
    stream->has_ungetc = 0;
    stream->flags &= ~_FILE_EOF;
    return lseek(stream->fd, offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *stream) {
    if (!stream) return -1;
    long pos = lseek(stream->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    if (stream->flags & _FILE_READ) {
        pos -= (stream->buf_end - stream->buf_pos);
        if (stream->has_ungetc) pos--;
    } else if (stream->flags & _FILE_WRITE) {
        pos += stream->buf_pos;
    }
    return pos;
}

void rewind(FILE *stream) {
    if (stream) {
        fseek(stream, 0, SEEK_SET);
        stream->flags &= ~_FILE_ERROR;
    }
}

int fgetpos(FILE *stream, fpos_t *pos) {
    if (!stream || !pos) return -1;
    long p = ftell(stream);
    if (p < 0) return -1;
    *pos = p;
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos) {
    if (!stream || !pos) return -1;
    return fseek(stream, *pos, SEEK_SET);
}

int feof(FILE *stream) {
    return stream ? (stream->flags & _FILE_EOF) : 0;
}

int ferror(FILE *stream) {
    return stream ? (stream->flags & _FILE_ERROR) : 0;
}

void clearerr(FILE *stream) {
    if (stream) {
        stream->flags &= ~(_FILE_EOF | _FILE_ERROR);
    }
}

int fileno(FILE *stream) {
    return stream ? stream->fd : -1;
}

int getc_unlocked(FILE *stream) {
    return fgetc(stream);
}

int putc_unlocked(int c, FILE *stream) {
    return fputc(c, stream);
}

int fflush_unlocked(FILE *stream) {
    return fflush(stream);
}

size_t fread_unlocked(void *ptr, size_t size, size_t n, FILE *stream) {
    return fread(ptr, size, n, stream);
}

size_t fwrite_unlocked(const void *ptr, size_t size, size_t n, FILE *stream) {
    return fwrite(ptr, size, n, stream);
}

int getchar_unlocked(void) {
    return fgetc(stdin);
}

int putchar_unlocked(int c) {
    return fputc(c, stdout);
}

int fgetc_unlocked(FILE *stream) {
    return fgetc(stream);
}

int fputc_unlocked(int c, FILE *stream) {
    return fputc(c, stream);
}

void flockfile(FILE *stream) {
    (void)stream;
}

void funlockfile(FILE *stream) {
    (void)stream;
}

int ftrylockfile(FILE *stream) {
    (void)stream;
    return 0;
}

#if !defined(__is_libk)
#include <unistd.h>

int remove(const char *pathname) {
    return unlink(pathname);
}

int rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -1;
}

static int tmpfile_counter = 0;

FILE *tmpfile(void) {
    char name[L_tmpnam];
    int i = 0;
    const char *prefix = "/tmp/tmp";
    while (prefix[i]) { name[i] = prefix[i]; i++; }
    int cnt = tmpfile_counter++;
    char digits[12];
    int d = 0;
    do {
        digits[d++] = '0' + (cnt % 10);
        cnt /= 10;
    } while (cnt);
    while (d > 0) name[i++] = digits[--d];
    name[i] = '\0';
    FILE *f = fopen(name, "w+");
    if (f) unlink(name);
    return f;
}

char *tmpnam(char *s) {
    static char buf[L_tmpnam];
    char *p = s ? s : buf;
    int i = 0;
    const char *prefix = "/tmp/tmp";
    while (prefix[i]) { p[i] = prefix[i]; i++; }
    int cnt = tmpfile_counter++;
    char digits[12];
    int d = 0;
    do {
        digits[d++] = '0' + (cnt % 10);
        cnt /= 10;
    } while (cnt);
    while (d > 0) p[i++] = digits[--d];
    p[i] = '\0';
    return p;
}

FILE *popen(const char *command, const char *type) {
    (void)command;
    (void)type;
    return NULL;
}

int pclose(FILE *stream) {
    (void)stream;
    return -1;
}

#else

int remove(const char *pathname) {
    (void)pathname;
    return -1;
}

int rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -1;
}

FILE *tmpfile(void) {
    return NULL;
}

char *tmpnam(char *s) {
    (void)s;
    return NULL;
}

FILE *popen(const char *command, const char *type) {
    (void)command;
    (void)type;
    return NULL;
}

int pclose(FILE *stream) {
    (void)stream;
    return -1;
}

#endif

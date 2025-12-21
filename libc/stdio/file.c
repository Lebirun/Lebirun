#include <stdio.h>
#include <unistd.h>

struct _FILE {
    int fd;
};

static struct _FILE _stdin = { .fd = 0 };
static struct _FILE _stdout = { .fd = 1 };
static struct _FILE _stderr = { .fd = 2 };

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    int ret = write(stream->fd, &ch, 1);
    return (ret == 1) ? c : EOF;
}

int fputs(const char *s, FILE *stream) {
    int len = 0;
    while (s[len]) len++;
    int ret = write(stream->fd, s, (size_t)len);
    return (ret >= 0) ? ret : EOF;
}

int fgetc(FILE *stream) {
    char c;
    int ret = read(stream->fd, &c, 1);
    return (ret == 1) ? (unsigned char)c : EOF;
}

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

int getchar(void) {
    char c;
    int ret = read(0, &c, 1);
    if (ret <= 0) return EOF;
    return (unsigned char)c;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    int fd = 1;
    if (stream == stderr) fd = 2;
    ssize_t w = write(fd, ptr, total);
    if (w <= 0) return 0;
    return (size_t)w / size;
}

size_t fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    const char *p = format;
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == 'u') {
                unsigned int u = va_arg(ap, unsigned int);
                char buf[16];
                int i = 0;
                if (u == 0) {
                    putchar('0');
                } else {
                    while (u) {
                        buf[i++] = '0' + (u % 10);
                        u /= 10;
                    }
                    while (i > 0) {
                        putchar(buf[--i]);
                    }
                }
            } else if (*p == 's') {
                const char *s = va_arg(ap, const char*);
                while (*s) putchar(*s++);
            } else if (*p == 'd') {
                int d = va_arg(ap, int);
                if (d < 0) {
                    putchar('-');
                    d = -d;
                }
                char buf[16];
                int i = 0;
                if (d == 0) {
                    putchar('0');
                } else {
                    while (d) {
                        buf[i++] = '0' + (d % 10);
                        d /= 10;
                    }
                    while (i > 0) {
                        putchar(buf[--i]);
                    }
                }
            } else {
                putchar('%');
                putchar(*p);
            }
        } else {
            putchar(*p);
        }
        p++;
    }
    va_end(ap);
    return 0;
}

int puts(const char *s) {
    printf("%s\n", s);
    return 0;
}

FILE *const stdin = (FILE*)0;
FILE *const stdout = (FILE*)1;
FILE *const stderr = (FILE*)2;
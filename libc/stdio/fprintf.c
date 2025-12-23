#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

int vfprintf(FILE *stream, const char *format, va_list ap) {
    if (!stream || !format) return -1;
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        fwrite(buf, 1, (size_t)len, stream);
    }
    return len;
}

int fprintf(FILE *stream, const char *format, ...) {
    if (!stream || !format) return -1;
    va_list ap;
    va_start(ap, format);
    int len = vfprintf(stream, format, ap);
    va_end(ap);
    return len;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int vsprintf(char *str, const char *format, va_list ap) {
    if (!str || !format) return -1;
    return vsnprintf(str, (size_t)-1, format, ap);
}

int vasprintf(char **strp, const char *format, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, format, ap);
    if (len < 0) {
        va_end(ap2);
        return -1;
    }
    *strp = malloc(len + 1);
    if (!*strp) {
        va_end(ap2);
        return -1;
    }
    int ret = vsnprintf(*strp, len + 1, format, ap2);
    va_end(ap2);
    return ret;
}

int asprintf(char **strp, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vasprintf(strp, format, ap);
    va_end(ap);
    return ret;
}

int dprintf(int fd, const char *format, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (len > 0) {
        write(fd, buf, len);
    }
    return len;
}

int vdprintf(int fd, const char *format, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        write(fd, buf, len);
    }
    return len;
}

int fscanf(FILE *stream, const char *format, ...) {
    char buf[4096];
    size_t pos = 0;
    int c;
    while (pos < sizeof(buf) - 1 && (c = fgetc(stream)) != EOF) {
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    va_list ap;
    va_start(ap, format);
    int ret = vsscanf(buf, format, ap);
    va_end(ap);
    return ret;
}

int scanf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    char buf[4096];
    size_t pos = 0;
    int c;
    while (pos < sizeof(buf) - 1 && (c = fgetc(stdin)) != EOF) {
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    int ret = vsscanf(buf, format, ap);
    va_end(ap);
    return ret;
}

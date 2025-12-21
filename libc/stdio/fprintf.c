#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

int fprintf(FILE *stream, const char *format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    
    int fd = 1;
    if (stream == stderr) fd = 2;
    
    if (len > 0) write(fd, buf, (size_t)len);
    return len;
}

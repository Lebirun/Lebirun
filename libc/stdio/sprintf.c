#include <stdio.h>
#include <stdarg.h>

int sprintf(char* buf, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vsnprintf(buf, (size_t)-1, format, ap);
    va_end(ap);
    return result;
}

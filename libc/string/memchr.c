#include <string.h>
#include <stddef.h>

void *memchr(const void *s, int c, size_t n) {
    if (!s) return NULL;
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

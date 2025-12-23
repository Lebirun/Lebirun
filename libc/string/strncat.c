#include <string.h>
#include <stddef.h>

char *strncat(char *dest, const char *src, size_t n) {
    if (!dest || !src) return dest;
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d = *src++)) d++;
    *d = '\0';
    return dest;
}

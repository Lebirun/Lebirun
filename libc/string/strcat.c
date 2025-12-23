#include <string.h>
#include <stddef.h>

char *strcat(char *dest, const char *src) {
    if (!dest || !src) return dest;
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

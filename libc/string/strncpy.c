#include <string.h>

char* strncpy(char* restrict dest, const char* restrict src, size_t n) {
    if (!dest || !src) return dest;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

#include <string.h>
#include <stddef.h>

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r++) return count;
        }
        count++;
        s++;
    }
    return count;
}

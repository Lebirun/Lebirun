#include <string.h>
#include <stddef.h>

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a++) { found = 1; break; }
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

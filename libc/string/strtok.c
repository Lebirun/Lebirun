#include <string.h>
#include <stddef.h>

static char *strtok_next = NULL;

char *strtok(char *str, const char *delim) {
    if (!delim) return NULL;
    if (str) strtok_next = str;
    if (!strtok_next) return NULL;
    
    while (*strtok_next) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) {
            if (*strtok_next == *d++) { is_delim = 1; break; }
        }
        if (!is_delim) break;
        strtok_next++;
    }
    
    if (!*strtok_next) return NULL;
    
    char *token = strtok_next;
    
    while (*strtok_next) {
        const char *d = delim;
        while (*d) {
            if (*strtok_next == *d) {
                *strtok_next++ = '\0';
                return token;
            }
            d++;
        }
        strtok_next++;
    }
    
    return token;
}

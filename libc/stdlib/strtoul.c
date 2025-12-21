#include <stdlib.h>
#include <stddef.h>

unsigned long strtoul(const char *s, char **endptr, int base) {
    unsigned long n = 0;
    
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '+') s++;
    
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else base = 8;
        } else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    
    while (1) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        
        if (digit >= base) break;
        n = n * base + digit;
        s++;
    }
    
    if (endptr) *endptr = (char *)s;
    return n;
}

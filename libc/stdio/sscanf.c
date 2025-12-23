#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static const char *skip_whitespace(const char *s) {
    while (*s && is_space(*s))
        s++;
    return s;
}

int vsscanf(const char *str, const char *format, va_list ap) {
    int count = 0;
    const char *s = str;
    const char *fmt = format;
    
    while (*fmt && *s) {
        if (is_space(*fmt)) {
            fmt++;
            s = skip_whitespace(s);
            continue;
        }
        
        if (*fmt != '%') {
            if (*s != *fmt)
                return count;
            s++;
            fmt++;
            continue;
        }
        
        fmt++;
        
        if (*fmt == '%') {
            if (*s != '%')
                return count;
            s++;
            fmt++;
            continue;
        }
        
        int suppress = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        
        int length = 0;
        if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') {
                length = 1;
                fmt++;
            } else {
                length = 2;
            }
        } else if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                length = 4;
                fmt++;
            } else {
                length = 3;
            }
        }
        
        char spec = *fmt++;
        
        switch (spec) {
            case 'd':
            case 'i': {
                s = skip_whitespace(s);
                if (!*s) return count;
                
                int neg = 0;
                if (*s == '-') { neg = 1; s++; }
                else if (*s == '+') { s++; }
                
                if (!(*s >= '0' && *s <= '9')) return count;
                
                long val = 0;
                int digits = 0;
                while (*s >= '0' && *s <= '9') {
                    if (width && digits >= width) break;
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (neg) val = -val;
                
                if (!suppress) {
                    if (length == 1) *va_arg(ap, signed char *) = (signed char)val;
                    else if (length == 2) *va_arg(ap, short *) = (short)val;
                    else if (length == 3) *va_arg(ap, long *) = val;
                    else if (length == 4) *va_arg(ap, long long *) = (long long)val;
                    else *va_arg(ap, int *) = (int)val;
                    count++;
                }
                break;
            }
            case 'u': {
                s = skip_whitespace(s);
                if (!*s) return count;
                
                if (*s == '+') s++;
                
                if (!(*s >= '0' && *s <= '9')) return count;
                
                unsigned long val = 0;
                int digits = 0;
                while (*s >= '0' && *s <= '9') {
                    if (width && digits >= width) break;
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                
                if (!suppress) {
                    if (length == 1) *va_arg(ap, unsigned char *) = (unsigned char)val;
                    else if (length == 2) *va_arg(ap, unsigned short *) = (unsigned short)val;
                    else if (length == 3) *va_arg(ap, unsigned long *) = val;
                    else if (length == 4) *va_arg(ap, unsigned long long *) = (unsigned long long)val;
                    else *va_arg(ap, unsigned int *) = (unsigned int)val;
                    count++;
                }
                break;
            }
            case 'x':
            case 'X': {
                s = skip_whitespace(s);
                if (!*s) return count;
                
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                    s += 2;
                
                unsigned long val = 0;
                int digits = 0;
                int valid = 0;
                while (1) {
                    if (width && digits >= width) break;
                    char c = *s;
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else break;
                    val = val * 16 + d;
                    s++;
                    digits++;
                    valid = 1;
                }
                if (!valid) return count;
                
                if (!suppress) {
                    if (length == 1) *va_arg(ap, unsigned char *) = (unsigned char)val;
                    else if (length == 2) *va_arg(ap, unsigned short *) = (unsigned short)val;
                    else if (length == 3) *va_arg(ap, unsigned long *) = val;
                    else if (length == 4) *va_arg(ap, unsigned long long *) = (unsigned long long)val;
                    else *va_arg(ap, unsigned int *) = (unsigned int)val;
                    count++;
                }
                break;
            }
            case 'o': {
                s = skip_whitespace(s);
                if (!*s) return count;
                
                unsigned long val = 0;
                int digits = 0;
                int valid = 0;
                while (*s >= '0' && *s <= '7') {
                    if (width && digits >= width) break;
                    val = val * 8 + (*s - '0');
                    s++;
                    digits++;
                    valid = 1;
                }
                if (!valid) return count;
                
                if (!suppress) {
                    if (length == 1) *va_arg(ap, unsigned char *) = (unsigned char)val;
                    else if (length == 2) *va_arg(ap, unsigned short *) = (unsigned short)val;
                    else if (length == 3) *va_arg(ap, unsigned long *) = val;
                    else if (length == 4) *va_arg(ap, unsigned long long *) = (unsigned long long)val;
                    else *va_arg(ap, unsigned int *) = (unsigned int)val;
                    count++;
                }
                break;
            }
            case 's': {
                s = skip_whitespace(s);
                if (!*s) return count;
                
                if (!suppress) {
                    char *dest = va_arg(ap, char *);
                    int i = 0;
                    while (*s && !is_space(*s)) {
                        if (width && i >= width) break;
                        dest[i++] = *s++;
                    }
                    dest[i] = '\0';
                    count++;
                } else {
                    int i = 0;
                    while (*s && !is_space(*s)) {
                        if (width && i >= width) break;
                        s++;
                        i++;
                    }
                }
                break;
            }
            case 'c': {
                int w = width ? width : 1;
                if (!suppress) {
                    char *dest = va_arg(ap, char *);
                    for (int i = 0; i < w && *s; i++)
                        dest[i] = *s++;
                    count++;
                } else {
                    for (int i = 0; i < w && *s; i++)
                        s++;
                }
                break;
            }
            case 'n': {
                if (!suppress) {
                    *va_arg(ap, int *) = (int)(s - str);
                }
                break;
            }
            case '[': {
                int invert = 0;
                if (*fmt == '^') {
                    invert = 1;
                    fmt++;
                }
                
                char set[256] = {0};
                int first = 1;
                while (*fmt && (*fmt != ']' || first)) {
                    if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                        for (unsigned char c = (unsigned char)*fmt; c <= (unsigned char)fmt[2]; c++)
                            set[c] = 1;
                        fmt += 3;
                    } else {
                        set[(unsigned char)*fmt] = 1;
                        fmt++;
                    }
                    first = 0;
                }
                if (*fmt == ']') fmt++;
                
                if (!suppress) {
                    char *dest = va_arg(ap, char *);
                    int i = 0;
                    while (*s) {
                        if (width && i >= width) break;
                        int match = set[(unsigned char)*s];
                        if (invert) match = !match;
                        if (!match) break;
                        dest[i++] = *s++;
                    }
                    if (i == 0) return count;
                    dest[i] = '\0';
                    count++;
                } else {
                    int i = 0;
                    while (*s) {
                        if (width && i >= width) break;
                        int match = set[(unsigned char)*s];
                        if (invert) match = !match;
                        if (!match) break;
                        s++;
                        i++;
                    }
                    if (i == 0) return count;
                }
                break;
            }
            case 'p': {
                s = skip_whitespace(s);
                if (!*s) return count;
                
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                    s += 2;
                
                uintptr_t val = 0;
                int valid = 0;
                while (1) {
                    char c = *s;
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else break;
                    val = val * 16 + d;
                    s++;
                    valid = 1;
                }
                if (!valid) return count;
                
                if (!suppress) {
                    *va_arg(ap, void **) = (void *)val;
                    count++;
                }
                break;
            }
            default:
                return count;
        }
    }
    
    return count;
}

int sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vsscanf(str, format, ap);
    va_end(ap);
    return result;
}

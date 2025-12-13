#include <stdio.h>
#include <unistd.h>
#include <string.h>

int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char *s) {
    int len = strlen(s);
    write(STDOUT_FILENO, s, len);
    write(STDOUT_FILENO, "\n", 1);
    return len + 1;
}

int getchar(void) {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    return (n > 0) ? (unsigned char)c : -1;
}

static void print_int(int n) {
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    if (n >= 10) print_int(n / 10);
    putchar('0' + (n % 10));
}

static void print_hex(unsigned int n) {
    if (n >= 16) print_hex(n / 16);
    int d = n % 16;
    putchar(d < 10 ? '0' + d : 'a' + d - 10);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    while (*s) { putchar(*s++); count++; }
                    break;
                }
                case 'd': {
                    int n = va_arg(ap, int);
                    print_int(n);
                    count++;
                    break;
                }
                case 'x': {
                    unsigned int n = va_arg(ap, unsigned int);
                    print_hex(n);
                    count++;
                    break;
                }
                case 'c': {
                    int c = va_arg(ap, int);
                    putchar(c);
                    count++;
                    break;
                }
                case '%':
                    putchar('%');
                    count++;
                    break;
                default:
                    putchar('%');
                    putchar(*fmt);
                    count += 2;
                    break;
            }
        } else {
            putchar(*fmt);
            count++;
        }
        fmt++;
    }

    va_end(ap);
    return count;
}

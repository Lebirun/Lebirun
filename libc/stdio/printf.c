#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool print(const char* data, size_t length) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < length; i++) {
        if (putchar(bytes[i]) == EOF)
            return false;
    }
    return true;
}

static void reverse(char* str, size_t len) {
    if (len == 0) return;
    size_t i = 0, j = len - 1;
    while (i < j) {
        char tmp = str[i];
        str[i] = str[j];
        str[j] = tmp;
        i++;
        j--;
    }
}

static size_t utoa64(uint64_t value, char* buf, int base, bool uppercase) {
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return 0;
    }
    char* p = buf;
    char a = uppercase ? 'A' : 'a';
    do {
        uint64_t digit = value % (uint64_t)base;
        *p++ = (digit < 10) ? (char)('0' + digit) : (char)(a + digit - 10);
        value /= (uint64_t)base;
    } while (value);
    *p = '\0';
    size_t len = (size_t)(p - buf);
    reverse(buf, len);
    return len;
}

static size_t itoa64(int64_t value, char* buf, int base, bool* is_negative) {
    *is_negative = false;
    uint64_t uval;
    if (value < 0 && base == 10) {
        *is_negative = true;
        uval = (uint64_t)(-(value + 1)) + 1;
    } else {
        uval = (uint64_t)value;
    }
    return utoa64(uval, buf, base, false);
}

typedef struct {
    bool left_justify;
    bool zero_pad;
    bool show_sign;
    bool space_sign;
    bool alt_form;
    int width;
    int precision;
    int length;
} fmt_flags_t;

static int print_number(const char* prefix, size_t prefix_len,
                        const char* num, size_t num_len,
                        fmt_flags_t* flags, int* written) {
    size_t maxrem = (size_t)(INT_MAX - *written);

    size_t content_len = prefix_len + num_len;

    int precision_pad = 0;
    if (flags->precision >= 0 && (size_t)flags->precision > num_len) {
        precision_pad = flags->precision - (int)num_len;
        content_len += (size_t)precision_pad;
        flags->zero_pad = false;
    }

    int field_pad = 0;
    if (flags->width > (int)content_len) {
        field_pad = flags->width - (int)content_len;
    }

    if (maxrem < content_len + (size_t)field_pad) {
        errno = EOVERFLOW;
        return -1;
    }

    if (flags->left_justify) {
        flags->zero_pad = false;
    }

    if (!flags->left_justify && !flags->zero_pad && field_pad > 0) {
        for (int i = 0; i < field_pad; i++) {
            if (!print(" ", 1)) return -1;
        }
        *written += field_pad;
        field_pad = 0;
    }

    if (prefix_len > 0) {
        if (!print(prefix, prefix_len)) return -1;
        *written += (int)prefix_len;
    }

    if (flags->zero_pad && field_pad > 0) {
        for (int i = 0; i < field_pad; i++) {
            if (!print("0", 1)) return -1;
        }
        *written += field_pad;
        field_pad = 0;
    }

    for (int i = 0; i < precision_pad; i++) {
        if (!print("0", 1)) return -1;
    }
    *written += precision_pad;

    if (!print(num, num_len)) return -1;
    *written += (int)num_len;

    if (flags->left_justify && field_pad > 0) {
        for (int i = 0; i < field_pad; i++) {
            if (!print(" ", 1)) return -1;
        }
        *written += field_pad;
    }

    return 0;
}

int printf(const char* restrict format, ...) {
    va_list parameters;
    va_start(parameters, format);

    int written = 0;
    char numbuf[68];

    while (*format != '\0') {
        size_t maxrem = (size_t)(INT_MAX - written);

        if (format[0] != '%' || format[1] == '%') {
            if (format[0] == '%')
                format++;
            size_t amount = 1;
            while (format[amount] && format[amount] != '%')
                amount++;
            if (maxrem < amount) {
                errno = EOVERFLOW;
                va_end(parameters);
                return -1;
            }
            if (!print(format, amount)) {
                va_end(parameters);
                return -1;
            }
            format += amount;
            written += (int)amount;
            continue;
        }

        format++;

        fmt_flags_t flags = {
            .left_justify = false,
            .zero_pad = false,
            .show_sign = false,
            .space_sign = false,
            .alt_form = false,
            .width = 0,
            .precision = -1,
            .length = 0
        };

        bool parsing_flags = true;
        while (parsing_flags && *format) {
            switch (*format) {
                case '-': flags.left_justify = true; format++; break;
                case '0': flags.zero_pad = true; format++; break;
                case '+': flags.show_sign = true; format++; break;
                case ' ': flags.space_sign = true; format++; break;
                case '#': flags.alt_form = true; format++; break;
                default: parsing_flags = false; break;
            }
        }

        if (flags.show_sign) flags.space_sign = false;

        if (*format == '*') {
            int w = va_arg(parameters, int);
            if (w < 0) {
                flags.left_justify = true;
                flags.width = -w;
            } else {
                flags.width = w;
            }
            format++;
        } else {
            while (*format >= '0' && *format <= '9') {
                flags.width = flags.width * 10 + (*format - '0');
                format++;
            }
        }

        if (*format == '.') {
            format++;
            flags.precision = 0;
            if (*format == '*') {
                int p = va_arg(parameters, int);
                flags.precision = (p < 0) ? -1 : p;
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    flags.precision = flags.precision * 10 + (*format - '0');
                    format++;
                }
            }
        }

        if (*format == 'h') {
            format++;
            if (*format == 'h') {
                flags.length = -2;
                format++;
            } else {
                flags.length = -1;
            }
        } else if (*format == 'l') {
            format++;
            if (*format == 'l') {
                flags.length = 2;
                format++;
            } else {
                flags.length = 1;
            }
        } else if (*format == 'z') {
            flags.length = 3;
            format++;
        }

        char spec = *format;
        if (spec == '\0') {
            break;
        }
        format++;

        switch (spec) {
        case 'c': {
            char c = (char)va_arg(parameters, int);
            int pad = (flags.width > 1) ? (flags.width - 1) : 0;
            if (maxrem < (size_t)(1 + pad)) {
                errno = EOVERFLOW;
                va_end(parameters);
                return -1;
            }
            if (!flags.left_justify) {
                for (int i = 0; i < pad; i++) {
                    if (!print(" ", 1)) { va_end(parameters); return -1; }
                }
                written += pad;
            }
            if (!print(&c, 1)) { va_end(parameters); return -1; }
            written++;
            if (flags.left_justify) {
                for (int i = 0; i < pad; i++) {
                    if (!print(" ", 1)) { va_end(parameters); return -1; }
                }
                written += pad;
            }
            break;
        }

        case 's': {
            const char* str = va_arg(parameters, const char*);
            if (str == NULL) str = "(null)";
            size_t len = strlen(str);

            if (flags.precision >= 0 && (size_t)flags.precision < len) {
                len = (size_t)flags.precision;
            }

            int pad = (flags.width > (int)len) ? (flags.width - (int)len) : 0;
            if (maxrem < len + (size_t)pad) {
                errno = EOVERFLOW;
                va_end(parameters);
                return -1;
            }
            if (!flags.left_justify) {
                for (int i = 0; i < pad; i++) {
                    if (!print(" ", 1)) { va_end(parameters); return -1; }
                }
                written += pad;
            }
            if (!print(str, len)) { va_end(parameters); return -1; }
            written += (int)len;
            if (flags.left_justify) {
                for (int i = 0; i < pad; i++) {
                    if (!print(" ", 1)) { va_end(parameters); return -1; }
                }
                written += pad;
            }
            break;
        }

        case 'd':
        case 'i': {
            int64_t val;
            if (flags.length == 2) {
                val = va_arg(parameters, long long);
            } else if (flags.length == 1) {
                val = va_arg(parameters, long);
            } else if (flags.length == 3) {
                val = (int64_t)va_arg(parameters, size_t);
            } else if (flags.length == -1) {
                val = (short)va_arg(parameters, int);
            } else if (flags.length == -2) {
                val = (signed char)va_arg(parameters, int);
            } else {
                val = va_arg(parameters, int);
            }

            bool is_negative;
            size_t len = itoa64(val, numbuf, 10, &is_negative);

            char prefix[2] = {0, 0};
            size_t prefix_len = 0;
            if (is_negative) {
                prefix[0] = '-';
                prefix_len = 1;
            } else if (flags.show_sign) {
                prefix[0] = '+';
                prefix_len = 1;
            } else if (flags.space_sign) {
                prefix[0] = ' ';
                prefix_len = 1;
            }

            if (flags.precision == 0 && val == 0) {
                len = 0;
                numbuf[0] = '\0';
            }

            if (print_number(prefix, prefix_len, numbuf, len, &flags, &written) < 0) {
                va_end(parameters);
                return -1;
            }
            break;
        }

        case 'u': {
            uint64_t val;
            if (flags.length == 2) {
                val = va_arg(parameters, unsigned long long);
            } else if (flags.length == 1) {
                val = va_arg(parameters, unsigned long);
            } else if (flags.length == 3) {
                val = va_arg(parameters, size_t);
            } else if (flags.length == -1) {
                val = (unsigned short)va_arg(parameters, unsigned int);
            } else if (flags.length == -2) {
                val = (unsigned char)va_arg(parameters, unsigned int);
            } else {
                val = va_arg(parameters, unsigned int);
            }

            size_t len = utoa64(val, numbuf, 10, false);

            if (flags.precision == 0 && val == 0) {
                len = 0;
                numbuf[0] = '\0';
            }

            if (print_number("", 0, numbuf, len, &flags, &written) < 0) {
                va_end(parameters);
                return -1;
            }
            break;
        }

        case 'x':
        case 'X': {
            bool uppercase = (spec == 'X');
            uint64_t val;
            if (flags.length == 2) {
                val = va_arg(parameters, unsigned long long);
            } else if (flags.length == 1) {
                val = va_arg(parameters, unsigned long);
            } else if (flags.length == 3) {
                val = va_arg(parameters, size_t);
            } else if (flags.length == -1) {
                val = (unsigned short)va_arg(parameters, unsigned int);
            } else if (flags.length == -2) {
                val = (unsigned char)va_arg(parameters, unsigned int);
            } else {
                val = va_arg(parameters, unsigned int);
            }

            size_t len = utoa64(val, numbuf, 16, uppercase);

            const char* prefix = "";
            size_t prefix_len = 0;
            if (flags.alt_form && val != 0) {
                prefix = uppercase ? "0X" : "0x";
                prefix_len = 2;
            }

            if (flags.precision == 0 && val == 0) {
                len = 0;
                numbuf[0] = '\0';
            }

            if (print_number(prefix, prefix_len, numbuf, len, &flags, &written) < 0) {
                va_end(parameters);
                return -1;
            }
            break;
        }

        case 'o': {
            uint64_t val;
            if (flags.length == 2) {
                val = va_arg(parameters, unsigned long long);
            } else if (flags.length == 1) {
                val = va_arg(parameters, unsigned long);
            } else if (flags.length == 3) {
                val = va_arg(parameters, size_t);
            } else if (flags.length == -1) {
                val = (unsigned short)va_arg(parameters, unsigned int);
            } else if (flags.length == -2) {
                val = (unsigned char)va_arg(parameters, unsigned int);
            } else {
                val = va_arg(parameters, unsigned int);
            }

            size_t len = utoa64(val, numbuf, 8, false);

            const char* prefix = "";
            size_t prefix_len = 0;
            if (flags.alt_form && (val != 0 || len == 0)) {
                if (numbuf[0] != '0') {
                    prefix = "0";
                    prefix_len = 1;
                }
            }

            if (flags.precision == 0 && val == 0 && !flags.alt_form) {
                len = 0;
                numbuf[0] = '\0';
            }

            if (print_number(prefix, prefix_len, numbuf, len, &flags, &written) < 0) {
                va_end(parameters);
                return -1;
            }
            break;
        }

        case 'b': {
            uint64_t val;
            if (flags.length == 2) {
                val = va_arg(parameters, unsigned long long);
            } else if (flags.length == 1) {
                val = va_arg(parameters, unsigned long);
            } else if (flags.length == 3) {
                val = va_arg(parameters, size_t);
            } else {
                val = va_arg(parameters, unsigned int);
            }

            size_t len = utoa64(val, numbuf, 2, false);

            const char* prefix = "";
            size_t prefix_len = 0;
            if (flags.alt_form && val != 0) {
                prefix = "0b";
                prefix_len = 2;
            }

            if (print_number(prefix, prefix_len, numbuf, len, &flags, &written) < 0) {
                va_end(parameters);
                return -1;
            }
            break;
        }

        case 'p': {
            void* ptr = va_arg(parameters, void*);
            if (ptr == NULL) {
                const char* nil_str = "(nil)";
                size_t nil_len = 5;
                int pad = (flags.width > (int)nil_len) ? (flags.width - (int)nil_len) : 0;
                if (!flags.left_justify) {
                    for (int i = 0; i < pad; i++) {
                        if (!print(" ", 1)) { va_end(parameters); return -1; }
                    }
                    written += pad;
                }
                if (!print(nil_str, nil_len)) { va_end(parameters); return -1; }
                written += (int)nil_len;
                if (flags.left_justify) {
                    for (int i = 0; i < pad; i++) {
                        if (!print(" ", 1)) { va_end(parameters); return -1; }
                    }
                    written += pad;
                }
            } else {
                uintptr_t val = (uintptr_t)ptr;
                size_t len = utoa64((uint64_t)val, numbuf, 16, false);

                if (print_number("0x", 2, numbuf, len, &flags, &written) < 0) {
                    va_end(parameters);
                    return -1;
                }
            }
            break;
        }

        case 'n': {
            if (flags.length == 2) {
                long long* p = va_arg(parameters, long long*);
                if (p) *p = written;
            } else if (flags.length == 1) {
                long* p = va_arg(parameters, long*);
                if (p) *p = written;
            } else if (flags.length == -1) {
                short* p = va_arg(parameters, short*);
                if (p) *p = (short)written;
            } else if (flags.length == -2) {
                signed char* p = va_arg(parameters, signed char*);
                if (p) *p = (signed char)written;
            } else {
                int* p = va_arg(parameters, int*);
                if (p) *p = written;
            }
            break;
        }

        default: {
            if (maxrem < 1) {
                errno = EOVERFLOW;
                va_end(parameters);
                return -1;
            }
            if (!print(&spec, 1)) {
                va_end(parameters);
                return -1;
            }
            written++;
            break;
        }
        }
    }

    va_end(parameters);
    return written;
}

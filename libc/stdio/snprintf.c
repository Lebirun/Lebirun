#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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

int vsnprintf(char* buf, size_t size, const char* format, va_list ap) {
    if (size == 0) return 0;
    
    size_t written = 0;
    char numbuf[68];
    
    while (*format != '\0') {
        if (written >= size - 1) break;
        
        if (format[0] != '%' || format[1] == '%') {
            if (format[0] == '%') format++;
            buf[written++] = *format++;
            continue;
        }
        
        format++; 
        
        bool left_justify = false;
        bool zero_pad = false;
        bool show_sign = false;
        
        while (*format == '-' || *format == '0' || *format == '+' || *format == ' ' || *format == '#') {
            if (*format == '-') left_justify = true;
            if (*format == '0') zero_pad = true;
            if (*format == '+') show_sign = true;
            format++;
        }
        
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        if (*format == '.') {
            format++;
            while (*format >= '0' && *format <= '9') format++;
        }
        
        int length = 0;
        if (*format == 'l') {
            format++;
            length = 1;
            if (*format == 'l') {
                format++;
                length = 2;
            }
        } else if (*format == 'h') {
            format++;
            length = -1;
        } else if (*format == 'z') {
            format++;
            length = 3;
        }
        
        char spec = *format;
        if (spec == '\0') break;
        format++;
        
        switch (spec) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (written < size - 1) buf[written++] = c;
            break;
        }
        
        case 's': {
            const char* str = va_arg(ap, const char*);
            if (!str) str = "(null)";
            while (*str && written < size - 1) {
                buf[written++] = *str++;
            }
            break;
        }
        
        case 'd':
        case 'i': {
            int64_t val;
            if (length == 2) val = va_arg(ap, long long);
            else if (length == 1) val = va_arg(ap, long);
            else val = va_arg(ap, int);
            
            bool neg;
            size_t len = itoa64(val, numbuf, 10, &neg);
            
            if (neg && written < size - 1) buf[written++] = '-';
            else if (show_sign && val >= 0 && written < size - 1) buf[written++] = '+';
            
            for (size_t i = 0; i < len && written < size - 1; i++) {
                buf[written++] = numbuf[i];
            }
            break;
        }
        
        case 'u': {
            uint64_t val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else if (length == 3) val = va_arg(ap, size_t);
            else val = va_arg(ap, unsigned int);
            
            size_t len = utoa64(val, numbuf, 10, false);
            for (size_t i = 0; i < len && written < size - 1; i++) {
                buf[written++] = numbuf[i];
            }
            break;
        }
        
        case 'x':
        case 'X': {
            uint64_t val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else if (length == 3) val = va_arg(ap, size_t);
            else val = va_arg(ap, unsigned int);
            
            size_t len = utoa64(val, numbuf, 16, spec == 'X');
            for (size_t i = 0; i < len && written < size - 1; i++) {
                buf[written++] = numbuf[i];
            }
            break;
        }
        
        case 'p': {
            void* ptr = va_arg(ap, void*);
            if (ptr == NULL) {
                const char* nil = "(nil)";
                while (*nil && written < size - 1) buf[written++] = *nil++;
            } else {
                if (written < size - 1) buf[written++] = '0';
                if (written < size - 1) buf[written++] = 'x';
                size_t len = utoa64((uintptr_t)ptr, numbuf, 16, false);
                for (size_t i = 0; i < len && written < size - 1; i++) {
                    buf[written++] = numbuf[i];
                }
            }
            break;
        }
        
        default:
            if (written < size - 1) buf[written++] = spec;
            break;
        }
        
        (void)width;
        (void)left_justify;
        (void)zero_pad;
    }
    
    buf[written] = '\0';
    return (int)written;
}

int snprintf(char* buf, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vsnprintf(buf, size, format, ap);
    va_end(ap);
    return result;
}

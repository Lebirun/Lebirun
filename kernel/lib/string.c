#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>

#include <kernel/vring.h>
#include <kernel/console.h>

extern void terminal_putchar(char c);

void* memset(void* bufptr, int value, size_t size) {
	if (!bufptr) return bufptr;
	unsigned char* buf = (unsigned char*) bufptr;
	for (size_t i = 0; i < size; i++)
		buf[i] = (unsigned char) value;
	return bufptr;
}

void* memcpy(void* __restrict dstptr, const void* __restrict srcptr, size_t size) {
	if (!dstptr || !srcptr) return dstptr;
	unsigned char* dst = (unsigned char*) dstptr;
	const unsigned char* src = (const unsigned char*) srcptr;
	for (size_t i = 0; i < size; i++)
		dst[i] = src[i];
	return dstptr;
}

void* memmove(void* dstptr, const void* srcptr, size_t size) {
	if (!dstptr || !srcptr) return dstptr;
	unsigned char* dst = (unsigned char*) dstptr;
	const unsigned char* src = (const unsigned char*) srcptr;
	if (dst < src) {
		for (size_t i = 0; i < size; i++)
			dst[i] = src[i];
	} else {
		for (size_t i = size; i != 0; i--)
			dst[i-1] = src[i-1];
	}
	return dstptr;
}

int memcmp(const void* aptr, const void* bptr, size_t size) {
	if (!aptr || !bptr) return 0;
	const unsigned char* a = (const unsigned char*) aptr;
	const unsigned char* b = (const unsigned char*) bptr;
	for (size_t i = 0; i < size; i++) {
		if (a[i] < b[i])
			return -1;
		else if (b[i] < a[i])
			return 1;
	}
	return 0;
}

size_t strlen(const char* str) {
	if (!str) return 0;
	size_t len = 0;
	while (str[len])
		len++;
	return len;
}

int strcmp(const char* s1, const char* s2) {
	if (!s1 || !s2) return 0;
	while (*s1 && (*s1 == *s2)) {
		s1++;
		s2++;
	}
	return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
	if (!s1 || !s2) return 0;
	while (n && *s1 && (*s1 == *s2)) {
		s1++;
		s2++;
		n--;
	}
	if (n == 0) return 0;
	return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strcpy(char* dest, const char* src) {
	if (!dest) return dest;
	if (!src) { dest[0] = '\0'; return dest; }
	char* d = dest;
	while ((*d++ = *src++));
	return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
	if (!dest) return dest;
	size_t i;
	for (i = 0; i < n && src && src[i]; i++)
		dest[i] = src[i];
	for (; i < n; i++)
		dest[i] = '\0';
	return dest;
}

char* strchr(const char* s, int c) {
	if (!s) return NULL;
	while (*s) {
		if (*s == (char)c) return (char*)s;
		s++;
	}
	return (c == '\0') ? (char*)s : NULL;
}

char* strrchr(const char* s, int c) {
	if (!s) return NULL;
	const char* last = NULL;
	while (*s) {
		if (*s == (char)c) last = s;
		s++;
	}
	return (c == '\0') ? (char*)s : (char*)last;
}

static int putchar_kernel(int c) {
	char ch = (char)c;
	if (console_is_initialized() && kprint_is_ready()) {
		kprint_write(0, &ch, 1);
		return c;
	}
	terminal_putchar(ch);
	return c;
}

static bool kprint(const char* data, size_t length) {
	if (!data || length == 0) return true;
	if (console_is_initialized() && kprint_is_ready()) {
		kprint_write(0, data, length);
		return true;
	}
	for (size_t i = 0; i < length; i++) {
		putchar_kernel(data[i]);
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
		i++; j--;
	}
}

static size_t utoa64(uint64_t value, char* buf, int base, bool uppercase) {
	if (base < 2 || base > 36) { buf[0] = '\0'; return 0; }
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
	if (!format) return -1;
	
	size_t written = 0;
	char numbuf[68];
	
	#define PUTC(c) do { \
		if (buf && size > 0 && written < size - 1) buf[written] = (c); \
		written++; \
	} while(0)
	
	while (*format) {
		if (*format != '%') {
			PUTC(*format++);
			continue;
		}
		format++;
		if (*format == '%') { PUTC('%'); format++; continue; }
		
		bool left = false, zero = false, plus = false, space = false, alt = false;
		while (*format) {
			if (*format == '-') { left = true; format++; }
			else if (*format == '0') { zero = true; format++; }
			else if (*format == '+') { plus = true; format++; }
			else if (*format == ' ') { space = true; format++; }
			else if (*format == '#') { alt = true; format++; }
			else break;
		}
		(void)alt;
		
		int width = 0;
		if (*format == '*') {
			width = va_arg(ap, int);
			if (width < 0) { left = true; width = -width; }
			format++;
		} else {
			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}
		}
		
		int prec = -1;
		if (*format == '.') {
			format++;
			prec = 0;
			if (*format == '*') {
				prec = va_arg(ap, int);
				format++;
			} else {
				while (*format >= '0' && *format <= '9') {
					prec = prec * 10 + (*format - '0');
					format++;
				}
			}
		}
		
		int length = 0;
		if (*format == 'l') { format++; length = 1; if (*format == 'l') { format++; length = 2; } }
		else if (*format == 'h') { format++; length = -1; if (*format == 'h') { format++; length = -2; } }
		else if (*format == 'z') { format++; length = 3; }
		
		char spec = *format++;
		if (!spec) break;
		
		switch (spec) {
		case 'c': {
			char c = (char)va_arg(ap, int);
			if (!left) for (int i = 1; i < width; i++) PUTC(' ');
			PUTC(c);
			if (left) for (int i = 1; i < width; i++) PUTC(' ');
			break;
		}
		case 's': {
			const char* s = va_arg(ap, const char*);
			if (!s) s = "(null)";
			size_t slen = strlen(s);
			if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
			if (!left) for (size_t i = slen; i < (size_t)width; i++) PUTC(' ');
			for (size_t i = 0; i < slen; i++) PUTC(s[i]);
			if (left) for (size_t i = slen; i < (size_t)width; i++) PUTC(' ');
			break;
		}
		case 'd': case 'i': {
			int64_t val;
			if (length == 2) val = va_arg(ap, long long);
			else if (length == 1) val = va_arg(ap, long);
			else if (length == 3) val = (int64_t)va_arg(ap, size_t);
			else val = va_arg(ap, int);
			bool neg;
			size_t len = itoa64(val, numbuf, 10, &neg);
			char prefix[2] = {0};
			int plen = 0;
			if (neg) { prefix[0] = '-'; plen = 1; }
			else if (plus) { prefix[0] = '+'; plen = 1; }
			else if (space) { prefix[0] = ' '; plen = 1; }
			int total = plen + (int)len;
			int pad = (width > total) ? (width - total) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) PUTC(' ');
			for (int i = 0; i < plen; i++) PUTC(prefix[i]);
			if (!left && zero) for (int i = 0; i < pad; i++) PUTC('0');
			for (size_t i = 0; i < len; i++) PUTC(numbuf[i]);
			if (left) for (int i = 0; i < pad; i++) PUTC(' ');
			break;
		}
		case 'u': {
			uint64_t val;
			if (length == 2) val = va_arg(ap, unsigned long long);
			else if (length == 1) val = va_arg(ap, unsigned long);
			else if (length == 3) val = va_arg(ap, size_t);
			else val = va_arg(ap, unsigned int);
			size_t len = utoa64(val, numbuf, 10, false);
			int pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) PUTC(' ');
			if (!left && zero) for (int i = 0; i < pad; i++) PUTC('0');
			for (size_t i = 0; i < len; i++) PUTC(numbuf[i]);
			if (left) for (int i = 0; i < pad; i++) PUTC(' ');
			break;
		}
		case 'x': case 'X': {
			bool upper = (spec == 'X');
			uint64_t val;
			if (length == 2) val = va_arg(ap, unsigned long long);
			else if (length == 1) val = va_arg(ap, unsigned long);
			else if (length == 3) val = va_arg(ap, size_t);
			else val = va_arg(ap, unsigned int);
			size_t len = utoa64(val, numbuf, 16, upper);
			int pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) PUTC(' ');
			if (!left && zero) for (int i = 0; i < pad; i++) PUTC('0');
			for (size_t i = 0; i < len; i++) PUTC(numbuf[i]);
			if (left) for (int i = 0; i < pad; i++) PUTC(' ');
			break;
		}
		case 'p': {
			void* ptr = va_arg(ap, void*);
			if (!ptr) {
				const char* nil = "(nil)";
				for (int i = 0; nil[i]; i++) PUTC(nil[i]);
			} else {
				PUTC('0'); PUTC('x');
				size_t len = utoa64((uintptr_t)ptr, numbuf, 16, false);
				for (size_t i = 0; i < len; i++) PUTC(numbuf[i]);
			}
			break;
		}
		default:
			PUTC(spec);
			break;
		}
	}
	
	if (buf && size > 0) {
		buf[(written < size) ? written : size - 1] = '\0';
	}
	return (int)written;
	#undef PUTC
}

int snprintf(char* buf, size_t size, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	int ret = vsnprintf(buf, size, format, ap);
	va_end(ap);
	return ret;
}

int sprintf(char* buf, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	int ret = vsnprintf(buf, (size_t)-1, format, ap);
	va_end(ap);
	return ret;
}

int printf(const char* format, ...) {
	if (!format) return -1;
	va_list ap;
	va_start(ap, format);
	
	int written = 0;
	char numbuf[68];
	
	while (*format) {
		if (*format != '%') {
			kprint(format, 1);
			format++;
			written++;
			continue;
		}
		format++;
		if (*format == '%') { kprint("%", 1); format++; written++; continue; }
		
		bool left = false, zero = false, plus = false, space = false, alt = false;
		while (*format) {
			if (*format == '-') { left = true; format++; }
			else if (*format == '0') { zero = true; format++; }
			else if (*format == '+') { plus = true; format++; }
			else if (*format == ' ') { space = true; format++; }
			else if (*format == '#') { alt = true; format++; }
			else break;
		}
		(void)alt;
		
		int width = 0;
		if (*format == '*') {
			width = va_arg(ap, int);
			if (width < 0) { left = true; width = -width; }
			format++;
		} else {
			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}
		}
		
		int prec = -1;
		if (*format == '.') {
			format++;
			prec = 0;
			if (*format == '*') {
				prec = va_arg(ap, int);
				format++;
			} else {
				while (*format >= '0' && *format <= '9') {
					prec = prec * 10 + (*format - '0');
					format++;
				}
			}
		}
		
		int length = 0;
		if (*format == 'l') { format++; length = 1; if (*format == 'l') { format++; length = 2; } }
		else if (*format == 'h') { format++; length = -1; if (*format == 'h') { format++; length = -2; } }
		else if (*format == 'z') { format++; length = 3; }
		
		char spec = *format++;
		if (!spec) break;
		
		switch (spec) {
		case 'c': {
			char c = (char)va_arg(ap, int);
			if (!left) for (int i = 1; i < width; i++) { kprint(" ", 1); written++; }
			kprint(&c, 1); written++;
			if (left) for (int i = 1; i < width; i++) { kprint(" ", 1); written++; }
			break;
		}
		case 's': {
			const char* s = va_arg(ap, const char*);
			if (!s) s = "(null)";
			size_t slen = strlen(s);
			if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
			if (!left) for (size_t i = slen; i < (size_t)width; i++) { kprint(" ", 1); written++; }
			kprint(s, slen); written += (int)slen;
			if (left) for (size_t i = slen; i < (size_t)width; i++) { kprint(" ", 1); written++; }
			break;
		}
		case 'd': case 'i': {
			int64_t val;
			if (length == 2) val = va_arg(ap, long long);
			else if (length == 1) val = va_arg(ap, long);
			else if (length == 3) val = (int64_t)va_arg(ap, size_t);
			else val = va_arg(ap, int);
			bool neg;
			size_t len = itoa64(val, numbuf, 10, &neg);
			char prefix[2] = {0};
			int plen = 0;
			if (neg) { prefix[0] = '-'; plen = 1; }
			else if (plus) { prefix[0] = '+'; plen = 1; }
			else if (space) { prefix[0] = ' '; plen = 1; }
			int total = plen + (int)len;
			int pad = (width > total) ? (width - total) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			if (plen) { kprint(prefix, (size_t)plen); written += plen; }
			if (!left && zero) for (int i = 0; i < pad; i++) { kprint("0", 1); written++; }
			kprint(numbuf, len); written += (int)len;
			if (left) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			break;
		}
		case 'u': {
			uint64_t val;
			if (length == 2) val = va_arg(ap, unsigned long long);
			else if (length == 1) val = va_arg(ap, unsigned long);
			else if (length == 3) val = va_arg(ap, size_t);
			else val = va_arg(ap, unsigned int);
			size_t len = utoa64(val, numbuf, 10, false);
			int pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			if (!left && zero) for (int i = 0; i < pad; i++) { kprint("0", 1); written++; }
			kprint(numbuf, len); written += (int)len;
			if (left) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			break;
		}
		case 'x': case 'X': {
			bool upper = (spec == 'X');
			uint64_t val;
			if (length == 2) val = va_arg(ap, unsigned long long);
			else if (length == 1) val = va_arg(ap, unsigned long);
			else if (length == 3) val = va_arg(ap, size_t);
			else val = va_arg(ap, unsigned int);
			size_t len = utoa64(val, numbuf, 16, upper);
			int pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			if (!left && zero) for (int i = 0; i < pad; i++) { kprint("0", 1); written++; }
			kprint(numbuf, len); written += (int)len;
			if (left) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			break;
		}
		case 'o': {
			uint64_t val;
			if (length == 2) val = va_arg(ap, unsigned long long);
			else if (length == 1) val = va_arg(ap, unsigned long);
			else if (length == 3) val = va_arg(ap, size_t);
			else val = va_arg(ap, unsigned int);
			size_t len = utoa64(val, numbuf, 8, false);
			int pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			if (!left && zero) for (int i = 0; i < pad; i++) { kprint("0", 1); written++; }
			kprint(numbuf, len); written += (int)len;
			if (left) for (int i = 0; i < pad; i++) { kprint(" ", 1); written++; }
			break;
		}
		case 'p': {
			void* ptr = va_arg(ap, void*);
			if (!ptr) {
				kprint("(nil)", 5); written += 5;
			} else {
				kprint("0x", 2); written += 2;
				size_t len = utoa64((uintptr_t)ptr, numbuf, 16, false);
				kprint(numbuf, len); written += (int)len;
			}
			break;
		}
		default:
			kprint(&spec, 1); written++;
			break;
		}
	}
	
	va_end(ap);
	return written;
}
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
	size_t i;
	if (!data || length == 0) return true;
	if (console_is_initialized() && kprint_is_ready()) {
		kprint_write(0, data, length);
		return true;
	}
	for (i = 0; i < length; i++) {
		putchar_kernel(data[i]);
	}
	return true;
}

static void reverse(char* str, size_t len) {
	size_t i;
	size_t j;
	char tmp;
	if (len == 0) return;
	i = 0;
	j = len - 1;
	while (i < j) {
		tmp = str[i];
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
	size_t written;
	char numbuf[68];
	int left, zero, plus, space, alt;
	int width;
	int prec;
	int length;
	char spec;
	int i;
	size_t si;
	char c;
	const char *s;
	size_t slen;
	int64_t ival;
	uint64_t uval;
	bool neg;
	size_t len;
	char prefix[2];
	int plen;
	int total;
	int pad;
	int upper;
	void *ptr;
	const char *nil;

	if (!format) return -1;
	written = 0;
	
	#define PUTC(ch) do { \
		if (buf && size > 0 && written < size - 1) buf[written] = (ch); \
		written++; \
	} while(0)
	
	while (*format) {
		if (*format != '%') {
			PUTC(*format++);
			continue;
		}
		format++;
		if (*format == '%') { PUTC('%'); format++; continue; }
		
		left = 0; zero = 0; plus = 0; space = 0; alt = 0;
		while (*format) {
			if (*format == '-') { left = 1; format++; }
			else if (*format == '0') { zero = 1; format++; }
			else if (*format == '+') { plus = 1; format++; }
			else if (*format == ' ') { space = 1; format++; }
			else if (*format == '#') { alt = 1; format++; }
			else break;
		}
		(void)alt;
		
		width = 0;
		if (*format == '*') {
			width = va_arg(ap, int);
			if (width < 0) { left = 1; width = -width; }
			format++;
		} else {
			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}
		}
		
		prec = -1;
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
		
		length = 0;
		if (*format == 'l') { format++; length = 1; if (*format == 'l') { format++; length = 2; } }
		else if (*format == 'h') { format++; length = -1; if (*format == 'h') { format++; length = -2; } }
		else if (*format == 'z') { format++; length = 3; }
		
		spec = *format++;
		if (!spec) break;
		
		switch (spec) {
		case 'c':
			c = (char)va_arg(ap, int);
			if (!left) { for (i = 1; i < width; i++) PUTC(' '); }
			PUTC(c);
			if (left) { for (i = 1; i < width; i++) PUTC(' '); }
			break;
		case 's':
			s = va_arg(ap, const char*);
			if (!s) s = "(null)";
			slen = strlen(s);
			if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
			if (!left) { for (si = slen; si < (size_t)width; si++) PUTC(' '); }
			for (si = 0; si < slen; si++) PUTC(s[si]);
			if (left) { for (si = slen; si < (size_t)width; si++) PUTC(' '); }
			break;
		case 'd':
		case 'i':
			if (length == 2) ival = va_arg(ap, long long);
			else if (length == 1) ival = va_arg(ap, long);
			else if (length == 3) ival = (int64_t)va_arg(ap, size_t);
			else ival = va_arg(ap, int);
			len = itoa64(ival, numbuf, 10, &neg);
			prefix[0] = 0; prefix[1] = 0;
			plen = 0;
			if (neg) { prefix[0] = '-'; plen = 1; }
			else if (plus) { prefix[0] = '+'; plen = 1; }
			else if (space) { prefix[0] = ' '; plen = 1; }
			total = plen + (int)len;
			pad = (width > total) ? (width - total) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) PUTC(' '); }
			for (i = 0; i < plen; i++) PUTC(prefix[i]);
			if (!left && zero) { for (i = 0; i < pad; i++) PUTC('0'); }
			for (si = 0; si < len; si++) PUTC(numbuf[si]);
			if (left) { for (i = 0; i < pad; i++) PUTC(' '); }
			break;
		case 'u':
			if (length == 2) uval = va_arg(ap, unsigned long long);
			else if (length == 1) uval = va_arg(ap, unsigned long);
			else if (length == 3) uval = va_arg(ap, size_t);
			else uval = va_arg(ap, unsigned int);
			len = utoa64(uval, numbuf, 10, 0);
			pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) PUTC(' '); }
			if (!left && zero) { for (i = 0; i < pad; i++) PUTC('0'); }
			for (si = 0; si < len; si++) PUTC(numbuf[si]);
			if (left) { for (i = 0; i < pad; i++) PUTC(' '); }
			break;
		case 'x':
		case 'X':
			upper = (spec == 'X');
			if (length == 2) uval = va_arg(ap, unsigned long long);
			else if (length == 1) uval = va_arg(ap, unsigned long);
			else if (length == 3) uval = va_arg(ap, size_t);
			else uval = va_arg(ap, unsigned int);
			len = utoa64(uval, numbuf, 16, upper);
			pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) PUTC(' '); }
			if (!left && zero) { for (i = 0; i < pad; i++) PUTC('0'); }
			for (si = 0; si < len; si++) PUTC(numbuf[si]);
			if (left) { for (i = 0; i < pad; i++) PUTC(' '); }
			break;
		case 'p':
			ptr = va_arg(ap, void*);
			if (!ptr) {
				nil = "(nil)";
				for (i = 0; nil[i]; i++) PUTC(nil[i]);
			} else {
				PUTC('0'); PUTC('x');
				len = utoa64((uintptr_t)ptr, numbuf, 16, 0);
				for (si = 0; si < len; si++) PUTC(numbuf[si]);
			}
			break;
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
	int ret;
	va_start(ap, format);
	ret = vsnprintf(buf, size, format, ap);
	va_end(ap);
	return ret;
}

int sprintf(char* buf, const char* format, ...) {
	va_list ap;
	int ret;
	va_start(ap, format);
	ret = vsnprintf(buf, (size_t)-1, format, ap);
	va_end(ap);
	return ret;
}

int printf(const char* format, ...) {
	va_list ap;
	int written;
	char numbuf[68];
	int left, zero, plus, space, alt;
	int width;
	int prec;
	int length;
	char spec;
	int i;
	size_t si;
	char c;
	const char *s;
	size_t slen;
	int64_t ival;
	uint64_t uval;
	bool neg;
	size_t len;
	char prefix[2];
	int plen;
	int total;
	int pad;
	int upper;
	void *ptr;

	if (!format) return -1;
	va_start(ap, format);
	written = 0;
	
	while (*format) {
		if (*format != '%') {
			kprint(format, 1);
			format++;
			written++;
			continue;
		}
		format++;
		if (*format == '%') { kprint("%", 1); format++; written++; continue; }
		
		left = 0; zero = 0; plus = 0; space = 0; alt = 0;
		while (*format) {
			if (*format == '-') { left = 1; format++; }
			else if (*format == '0') { zero = 1; format++; }
			else if (*format == '+') { plus = 1; format++; }
			else if (*format == ' ') { space = 1; format++; }
			else if (*format == '#') { alt = 1; format++; }
			else break;
		}
		(void)alt;
		
		width = 0;
		if (*format == '*') {
			width = va_arg(ap, int);
			if (width < 0) { left = 1; width = -width; }
			format++;
		} else {
			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}
		}
		
		prec = -1;
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
		
		length = 0;
		if (*format == 'l') { format++; length = 1; if (*format == 'l') { format++; length = 2; } }
		else if (*format == 'h') { format++; length = -1; if (*format == 'h') { format++; length = -2; } }
		else if (*format == 'z') { format++; length = 3; }
		
		spec = *format++;
		if (!spec) break;
		
		switch (spec) {
		case 'c':
			c = (char)va_arg(ap, int);
			if (!left) { for (i = 1; i < width; i++) { kprint(" ", 1); written++; } }
			kprint(&c, 1); written++;
			if (left) { for (i = 1; i < width; i++) { kprint(" ", 1); written++; } }
			break;
		case 's':
			s = va_arg(ap, const char*);
			if (!s) s = "(null)";
			slen = strlen(s);
			if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
			if (!left) { for (si = slen; si < (size_t)width; si++) { kprint(" ", 1); written++; } }
			kprint(s, slen); written += (int)slen;
			if (left) { for (si = slen; si < (size_t)width; si++) { kprint(" ", 1); written++; } }
			break;
		case 'd':
		case 'i':
			if (length == 2) ival = va_arg(ap, long long);
			else if (length == 1) ival = va_arg(ap, long);
			else if (length == 3) ival = (int64_t)va_arg(ap, size_t);
			else ival = va_arg(ap, int);
			len = itoa64(ival, numbuf, 10, &neg);
			prefix[0] = 0; prefix[1] = 0;
			plen = 0;
			if (neg) { prefix[0] = '-'; plen = 1; }
			else if (plus) { prefix[0] = '+'; plen = 1; }
			else if (space) { prefix[0] = ' '; plen = 1; }
			total = plen + (int)len;
			pad = (width > total) ? (width - total) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			if (plen) { kprint(prefix, (size_t)plen); written += plen; }
			if (!left && zero) { for (i = 0; i < pad; i++) { kprint("0", 1); written++; } }
			kprint(numbuf, len); written += (int)len;
			if (left) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			break;
		case 'u':
			if (length == 2) uval = va_arg(ap, unsigned long long);
			else if (length == 1) uval = va_arg(ap, unsigned long);
			else if (length == 3) uval = va_arg(ap, size_t);
			else uval = va_arg(ap, unsigned int);
			len = utoa64(uval, numbuf, 10, 0);
			pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			if (!left && zero) { for (i = 0; i < pad; i++) { kprint("0", 1); written++; } }
			kprint(numbuf, len); written += (int)len;
			if (left) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			break;
		case 'x':
		case 'X':
			upper = (spec == 'X');
			if (length == 2) uval = va_arg(ap, unsigned long long);
			else if (length == 1) uval = va_arg(ap, unsigned long);
			else if (length == 3) uval = va_arg(ap, size_t);
			else uval = va_arg(ap, unsigned int);
			len = utoa64(uval, numbuf, 16, upper);
			pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			if (!left && zero) { for (i = 0; i < pad; i++) { kprint("0", 1); written++; } }
			kprint(numbuf, len); written += (int)len;
			if (left) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			break;
		case 'o':
			if (length == 2) uval = va_arg(ap, unsigned long long);
			else if (length == 1) uval = va_arg(ap, unsigned long);
			else if (length == 3) uval = va_arg(ap, size_t);
			else uval = va_arg(ap, unsigned int);
			len = utoa64(uval, numbuf, 8, 0);
			pad = (width > (int)len) ? (width - (int)len) : 0;
			if (!left && !zero) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			if (!left && zero) { for (i = 0; i < pad; i++) { kprint("0", 1); written++; } }
			kprint(numbuf, len); written += (int)len;
			if (left) { for (i = 0; i < pad; i++) { kprint(" ", 1); written++; } }
			break;
		case 'p':
			ptr = va_arg(ap, void*);
			if (!ptr) {
				kprint("(nil)", 5); written += 5;
			} else {
				kprint("0x", 2); written += 2;
				len = utoa64((uintptr_t)ptr, numbuf, 16, 0);
				kprint(numbuf, len); written += (int)len;
			}
			break;
		default:
			kprint(&spec, 1); written++;
			break;
		}
	}
	
	va_end(ap);
	return written;
}
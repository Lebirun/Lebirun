#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool print(const char* data, size_t length) {
	const unsigned char* bytes = (const unsigned char*) data;
	for (size_t i = 0; i < length; i++)
		if (putchar(bytes[i]) == EOF)
			return false;
	return true;
}

static void reverse(char* str, size_t len) {
	size_t i = 0, j = len - 1;
	while (i < j) {
		char tmp = str[i];
		str[i] = str[j];
		str[j] = tmp;
		i++;
		j--;
	}
}

static size_t itoa_signed(int value, char* buf, int base) {
	char* p = buf;
	unsigned int uval;
	bool negative = false;
	if (value < 0 && base == 10) {
		negative = true;
		uval = (unsigned int)(-(value + 1)) + 1;
	} else {
		uval = (unsigned int)value;
	}
	do {
		unsigned int digit = uval % base;
		*p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
		uval /= base;
	} while (uval);
	if (negative) *p++ = '-';
	*p = '\0';
	size_t len = p - buf;
	reverse(buf, len);
	return len;
}

static size_t utoa(unsigned int value, char* buf, int base) {
	char* p = buf;
	do {
		unsigned int digit = value % base;
		*p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
		value /= base;
	} while (value);
	*p = '\0';
	size_t len = p - buf;
	reverse(buf, len);
	return len;
}

static size_t ultoa(unsigned long value, char* buf, int base) {
	char* p = buf;
	do {
		unsigned long digit = value % base;
		*p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
		value /= base;
	} while (value);
	*p = '\0';
	size_t len = p - buf;
	reverse(buf, len);
	return len;
}

static size_t ltoa_signed(long value, char* buf, int base) {
	char* p = buf;
	unsigned long uval;
	bool negative = false;
	if (value < 0 && base == 10) {
		negative = true;
		uval = (unsigned long)(-(value + 1)) + 1;
	} else {
		uval = (unsigned long)value;
	}
	do {
		unsigned long digit = uval % base;
		*p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
		uval /= base;
	} while (uval);
	if (negative) *p++ = '-';
	*p = '\0';
	size_t len = p - buf;
	reverse(buf, len);
	return len;
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;
	char numbuf[32];

	while (*format != '\0') {
		size_t maxrem = INT_MAX - written;

		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%')
				format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%')
				amount++;
			if (maxrem < amount) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(format, amount))
				return -1;
			format += amount;
			written += amount;
			continue;
		}

		const char* format_begun_at = format++;

		bool is_long = false;
		if (*format == 'l') {
			is_long = true;
			format++;
		}

		if (*format == 'c') {
			format++;
			char c = (char) va_arg(parameters, int);
			if (!maxrem) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(&c, sizeof(c)))
				return -1;
			written++;
		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			size_t len = strlen(str);
			if (maxrem < len) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(str, len))
				return -1;
			written += len;
		} else if (*format == 'd' || *format == 'i') {
			format++;
			size_t len;
			if (is_long) {
				long val = va_arg(parameters, long);
				len = ltoa_signed(val, numbuf, 10);
			} else {
				int val = va_arg(parameters, int);
				len = itoa_signed(val, numbuf, 10);
			}
			if (maxrem < len) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(numbuf, len))
				return -1;
			written += len;
		} else if (*format == 'u') {
			format++;
			size_t len;
			if (is_long) {
				unsigned long val = va_arg(parameters, unsigned long);
				len = ultoa(val, numbuf, 10);
			} else {
				unsigned int val = va_arg(parameters, unsigned int);
				len = utoa(val, numbuf, 10);
			}
			if (maxrem < len) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(numbuf, len))
				return -1;
			written += len;
		} else if (*format == 'x') {
			format++;
			size_t len;
			if (is_long) {
				unsigned long val = va_arg(parameters, unsigned long);
				len = ultoa(val, numbuf, 16);
			} else {
				unsigned int val = va_arg(parameters, unsigned int);
				len = utoa(val, numbuf, 16);
			}
			if (maxrem < len) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(numbuf, len))
				return -1;
			written += len;
		} else if (*format == 'p') {
			format++;
			void* ptr = va_arg(parameters, void*);
			unsigned long val = (unsigned long)ptr;
			size_t len = ultoa(val, numbuf, 16);
			if (maxrem < len + 2) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print("0x", 2))
				return -1;
			if (!print(numbuf, len))
				return -1;
			written += len + 2;
		} else {
			format = format_begun_at;
			size_t len = 1;
			if (maxrem < len) {
				errno = EOVERFLOW;
				return -1;
			}
			if (!print(format, len))
				return -1;
			format++;
			written += len;
		}
	}

	va_end(parameters);
	return written;
}

#include <string.h>

char* strcpy(char* restrict dest, const char* restrict src) {
	if (!dest || !src) return dest;
	char* ret = dest;
	while ((*dest++ = *src++))
		;
	return ret;
}

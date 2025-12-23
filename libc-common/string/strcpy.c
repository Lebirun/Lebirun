#include <string.h>

char* strcpy(char* __restrict dest, const char* __restrict src) {
	if (!dest || !src) return dest;
	char* ret = dest;
	while ((*dest++ = *src++))
		;
	return ret;
}

#include <string.h>

char* strcpy(char* __restrict dest, const char* __restrict src) {
	char* ret = dest;
	while ((*dest++ = *src++))
		;
	return ret;
}

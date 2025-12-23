#include <string.h>

void* mempcpy(void* restrict dstptr, const void* restrict srcptr, size_t size) {
	if (!dstptr || !srcptr) return dstptr;
	unsigned char* dst = (unsigned char*) dstptr;
	const unsigned char* src = (const unsigned char*) srcptr;
	for (size_t i = 0; i < size; i++)
		dst[i] = src[i];
	return dst + size;
}

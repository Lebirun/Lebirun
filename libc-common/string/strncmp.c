#include <string.h>

int strncmp(const char* s1, const char* s2, size_t n) {
	if (!s1 && !s2) return 0;
	if (!s1) return -1;
	if (!s2) return 1;
	while (n && *s1 && *s1 == *s2) {
		s1++;
		s2++;
		n--;
	}
	return n ? ((unsigned char)*s1 - (unsigned char)*s2) : 0;
}

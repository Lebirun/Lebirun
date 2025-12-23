#ifndef _STRINGS_H
#define _STRINGS_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int bcmp(const void *s1, const void *s2, size_t n);
void bcopy(const void *src, void *dest, size_t n);
void bzero(void *s, size_t n);

int ffs(int i);
int ffsl(long i);
int ffsll(long long i);

char *index(const char *s, int c);
char *rindex(const char *s, int c);

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif

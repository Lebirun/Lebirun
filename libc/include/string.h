#ifndef _STRING_H
#define _STRING_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int memcmp(const void*, const void*, size_t);
void* memcpy(void* __restrict, const void* __restrict, size_t);
void* mempcpy(void* __restrict, const void* __restrict, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
void* memchr(const void*, int, size_t);
void* memrchr(const void*, int, size_t);

size_t strlen(const char*);
size_t strnlen(const char*, size_t);

char* strcpy(char* __restrict, const char* __restrict);
char* strncpy(char* __restrict, const char* __restrict, size_t);
char* stpcpy(char* __restrict, const char* __restrict);
char* stpncpy(char* __restrict, const char* __restrict, size_t);

int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
int strcasecmp(const char*, const char*);
int strncasecmp(const char*, const char*, size_t);

char* strcat(char* __restrict, const char* __restrict);
char* strncat(char* __restrict, const char* __restrict, size_t);

char* strchr(const char*, int);
char* strrchr(const char*, int);
char* strchrnul(const char*, int);
char* strstr(const char*, const char*);
char* strcasestr(const char*, const char*);

char* strdup(const char*);
char* strndup(const char*, size_t);

size_t strspn(const char*, const char*);
size_t strcspn(const char*, const char*);
char* strpbrk(const char*, const char*);
char* strtok(char*, const char*);
char* strtok_r(char*, const char*, char**);

char* strsep(char**, const char*);

char* strerror(int);
int strerror_r(int, char*, size_t);

char* strsignal(int);

size_t strxfrm(char* __restrict, const char* __restrict, size_t);
int strcoll(const char*, const char*);

void* memccpy(void* __restrict, const void* __restrict, int, size_t);

int bcmp(const void*, const void*, size_t);
void bcopy(const void*, void*, size_t);
void bzero(void*, size_t);
char* index(const char*, int);
char* rindex(const char*, int);

int ffs(int);
int ffsl(long);
int ffsll(long long);

#ifdef __cplusplus
}
#endif

#endif

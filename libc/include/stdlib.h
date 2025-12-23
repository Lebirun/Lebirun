#ifndef _STDLIB_H
#define _STDLIB_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#ifndef NULL
#define NULL ((void*)0)
#endif

#define RAND_MAX 2147483647

#define MB_CUR_MAX 4

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((__noreturn__))
void abort(void);

__attribute__((__noreturn__))
void exit(int status);

__attribute__((__noreturn__))
void _Exit(int status);

int atexit(void (*function)(void));
int on_exit(void (*function)(int, void *), void *arg);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);

long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
long long strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
double strtod(const char *s, char **endptr);
float strtof(const char *s, char **endptr);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

int abs(int j);
long labs(long j);
long long llabs(long long j);
div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int clearenv(void);

extern char **environ;

int rand(void);
void srand(unsigned int seed);
int rand_r(unsigned int *seedp);

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

int system(const char *command);

int mkstemp(char *template);
int mkstemps(char *template, int suffixlen);
char *mkdtemp(char *template);

int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);
int mblen(const char *s, size_t n);

char *realpath(const char *path, char *resolved_path);
void *aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _WCHAR_H
#define _WCHAR_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN 0
#endif

#ifndef WCHAR_MAX
#define WCHAR_MAX 0x7fffffff
#endif

typedef int32_t wint_t;

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

typedef struct {
    int __count;
    union {
        wint_t __wch;
        char __wchb[4];
    } __value;
} mbstate_t;

struct tm;
struct _FILE;
typedef struct _FILE FILE;

wint_t btowc(int c);
int wctob(wint_t c);

int mbsinit(const mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);

size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
int mblen(const char *s, size_t n);

size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *s1, const wchar_t *s2);
int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n);
int wcscasecmp(const wchar_t *s1, const wchar_t *s2);
int wcsncasecmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcstok(wchar_t *str, const wchar_t *delim, wchar_t **saveptr);
wchar_t *wcsdup(const wchar_t *s);

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);

long wcstol(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base);
long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base);
double wcstod(const wchar_t *nptr, wchar_t **endptr);
float wcstof(const wchar_t *nptr, wchar_t **endptr);

int iswctype(wint_t wc, wctype_t desc);
wctype_t wctype(const char *property);
wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);
wctrans_t wctrans(const char *charclass);
wint_t towctrans(wint_t wc, wctrans_t desc);

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

wint_t fgetwc(FILE *stream);
wchar_t *fgetws(wchar_t *ws, int n, FILE *stream);
wint_t fputwc(wchar_t wc, FILE *stream);
int fputws(const wchar_t *ws, FILE *stream);
wint_t getwc(FILE *stream);
wint_t getwchar(void);
wint_t putwc(wchar_t wc, FILE *stream);
wint_t putwchar(wchar_t wc);
wint_t ungetwc(wint_t wc, FILE *stream);

int fwide(FILE *stream, int mode);

int fwprintf(FILE *stream, const wchar_t *format, ...);
int wprintf(const wchar_t *format, ...);
int swprintf(wchar_t *ws, size_t maxlen, const wchar_t *format, ...);
int vfwprintf(FILE *stream, const wchar_t *format, va_list args);
int vwprintf(const wchar_t *format, va_list args);
int vswprintf(wchar_t *ws, size_t maxlen, const wchar_t *format, va_list args);

int fwscanf(FILE *stream, const wchar_t *format, ...);
int wscanf(const wchar_t *format, ...);
int swscanf(const wchar_t *ws, const wchar_t *format, ...);
int vfwscanf(FILE *stream, const wchar_t *format, va_list args);
int vwscanf(const wchar_t *format, va_list args);
int vswscanf(const wchar_t *ws, const wchar_t *format, va_list args);

size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format, const struct tm *timeptr);

#ifdef __cplusplus
}
#endif

#endif

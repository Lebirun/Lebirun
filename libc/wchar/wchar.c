#include <wchar.h>
#include <wctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

wint_t btowc(int c) {
    if (c == EOF) return WEOF;
    if ((unsigned char)c > 127) return WEOF;
    return (wint_t)(unsigned char)c;
}

int wctob(wint_t c) {
    if (c == WEOF || c > 127) return EOF;
    return (int)(unsigned char)c;
}

int mbsinit(const mbstate_t *ps) {
    return ps == NULL || ps->__count == 0;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    mbstate_t internal;
    if (!ps) ps = &internal;
    return mbrtowc(NULL, s, n, ps);
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (!s) {
        ps->__count = 0;
        return 0;
    }
    if (n == 0) return (size_t)-2;
    unsigned char c = (unsigned char)*s;
    if (c == 0) {
        if (pwc) *pwc = 0;
        return 0;
    }
    if (c < 0x80) {
        if (pwc) *pwc = c;
        return 1;
    }
    if ((c & 0xe0) == 0xc0 && n >= 2) {
        wchar_t wc = (c & 0x1f) << 6;
        wc |= ((unsigned char)s[1] & 0x3f);
        if (pwc) *pwc = wc;
        return 2;
    }
    if ((c & 0xf0) == 0xe0 && n >= 3) {
        wchar_t wc = (c & 0x0f) << 12;
        wc |= ((unsigned char)s[1] & 0x3f) << 6;
        wc |= ((unsigned char)s[2] & 0x3f);
        if (pwc) *pwc = wc;
        return 3;
    }
    if ((c & 0xf8) == 0xf0 && n >= 4) {
        wchar_t wc = (c & 0x07) << 18;
        wc |= ((unsigned char)s[1] & 0x3f) << 12;
        wc |= ((unsigned char)s[2] & 0x3f) << 6;
        wc |= ((unsigned char)s[3] & 0x3f);
        if (pwc) *pwc = wc;
        return 4;
    }
    return (size_t)-1;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (!s) {
        ps->__count = 0;
        return 1;
    }
    if (wc < 0x80) {
        *s = (char)wc;
        return 1;
    }
    if (wc < 0x800) {
        s[0] = 0xc0 | (wc >> 6);
        s[1] = 0x80 | (wc & 0x3f);
        return 2;
    }
    if (wc < 0x10000) {
        s[0] = 0xe0 | (wc >> 12);
        s[1] = 0x80 | ((wc >> 6) & 0x3f);
        s[2] = 0x80 | (wc & 0x3f);
        return 3;
    }
    if (wc < 0x110000) {
        s[0] = 0xf0 | (wc >> 18);
        s[1] = 0x80 | ((wc >> 12) & 0x3f);
        s[2] = 0x80 | ((wc >> 6) & 0x3f);
        s[3] = 0x80 | (wc & 0x3f);
        return 4;
    }
    return (size_t)-1;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    size_t written = 0;
    while (len > 0) {
        wchar_t wc;
        size_t ret = mbrtowc(&wc, *src, 4, ps);
        if (ret == 0) {
            if (dst) dst[written] = 0;
            *src = NULL;
            return written;
        }
        if (ret == (size_t)-1 || ret == (size_t)-2) return (size_t)-1;
        if (dst) dst[written] = wc;
        written++;
        *src += ret;
        len--;
    }
    return written;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    size_t written = 0;
    while (len > 0) {
        char buf[4];
        size_t ret = wcrtomb(buf, **src, ps);
        if (ret == (size_t)-1) return (size_t)-1;
        if (ret > len) return written;
        if (dst) memcpy(dst + written, buf, ret);
        written += ret;
        len -= ret;
        if (**src == 0) {
            *src = NULL;
            return written - 1;
        }
        (*src)++;
    }
    return written;
}

size_t mbstowcs(wchar_t *dest, const char *src, size_t n) {
    const char *s = src;
    return mbsrtowcs(dest, &s, n, NULL);
}

size_t wcstombs(char *dest, const wchar_t *src, size_t n) {
    const wchar_t *s = src;
    return wcsrtombs(dest, &s, n, NULL);
}

int mbtowc(wchar_t *pwc, const char *s, size_t n) {
    if (!s) return 0;
    size_t ret = mbrtowc(pwc, s, n, NULL);
    if (ret == (size_t)-2) return -1;
    if (ret == (size_t)-1) return -1;
    return (int)ret;
}

int wctomb(char *s, wchar_t wc) {
    if (!s) return 0;
    size_t ret = wcrtomb(s, wc, NULL);
    if (ret == (size_t)-1) return -1;
    return (int)ret;
}

int mblen(const char *s, size_t n) {
    if (!s) return 0;
    return mbtowc(NULL, s, n);
}

size_t wcslen(const wchar_t *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src) {
    wchar_t *d = dest;
    while ((*d++ = *src++));
    return dest;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = 0;
    return dest;
}

wchar_t *wcscat(wchar_t *dest, const wchar_t *src) {
    wchar_t *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n) {
    wchar_t *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = 0;
    return dest;
}

int wcscmp(const wchar_t *s1, const wchar_t *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *s1 - *s2;
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { n--; s1++; s2++; }
    return n ? *s1 - *s2 : 0;
}

int wcscasecmp(const wchar_t *s1, const wchar_t *s2) {
    while (*s1 && towlower(*s1) == towlower(*s2)) { s1++; s2++; }
    return towlower(*s1) - towlower(*s2);
}

int wcsncasecmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    while (n && *s1 && towlower(*s1) == towlower(*s2)) { n--; s1++; s2++; }
    return n ? towlower(*s1) - towlower(*s2) : 0;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    while (*s) {
        if (*s == c) return (wchar_t *)s;
        s++;
    }
    return c ? NULL : (wchar_t *)s;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *last = NULL;
    while (*s) {
        if (*s == c) last = s;
        s++;
    }
    return c ? (wchar_t *)last : (wchar_t *)s;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
    while (*s) {
        const wchar_t *a = accept;
        while (*a) {
            if (*s == *a) return (wchar_t *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
    size_t count = 0;
    while (*s) {
        const wchar_t *a = accept;
        while (*a && *a != *s) a++;
        if (!*a) break;
        count++;
        s++;
    }
    return count;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
    size_t count = 0;
    while (*s) {
        const wchar_t *r = reject;
        while (*r && *r != *s) r++;
        if (*r) break;
        count++;
        s++;
    }
    return count;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
    if (!*needle) return (wchar_t *)haystack;
    size_t needlelen = wcslen(needle);
    for (; *haystack; haystack++) {
        if (wcsncmp(haystack, needle, needlelen) == 0) {
            return (wchar_t *)haystack;
        }
    }
    return NULL;
}

wchar_t *wcstok(wchar_t *str, const wchar_t *delim, wchar_t **saveptr) {
    if (!str) str = *saveptr;
    if (!str) return NULL;
    str += wcsspn(str, delim);
    if (!*str) {
        *saveptr = NULL;
        return NULL;
    }
    wchar_t *end = str + wcscspn(str, delim);
    if (*end) {
        *end = 0;
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return str;
}

wchar_t *wcsdup(const wchar_t *s) {
    size_t len = wcslen(s) + 1;
    wchar_t *dup = malloc(len * sizeof(wchar_t));
    if (dup) wmemcpy(dup, s, len);
    return dup;
}

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dest[i] = src[i];
    return dest;
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n) {
    if (dest < src) {
        for (size_t i = 0; i < n; i++) dest[i] = src[i];
    } else {
        for (size_t i = n; i > 0; i--) dest[i-1] = src[i-1];
    }
    return dest;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) s[i] = c;
    return s;
}

int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return s1[i] - s2[i];
    }
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) return (wchar_t *)(s + i);
    }
    return NULL;
}

int wcwidth(wchar_t c) {
    if (c == 0) return 0;
    if (c < 32 || (c >= 0x7f && c < 0xa0)) return -1;
    if (c >= 0x1100 && 
        ((c <= 0x115f) ||
         (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
         (c >= 0xac00 && c <= 0xd7a3) ||
         (c >= 0xf900 && c <= 0xfaff) ||
         (c >= 0xfe10 && c <= 0xfe1f) ||
         (c >= 0xfe30 && c <= 0xfe6f) ||
         (c >= 0xff00 && c <= 0xff60) ||
         (c >= 0xffe0 && c <= 0xffe6) ||
         (c >= 0x20000 && c <= 0x2fffd) ||
         (c >= 0x30000 && c <= 0x3fffd)))
        return 2;
    return 1;
}

int wcswidth(const wchar_t *s, size_t n) {
    int width = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        int w = wcwidth(s[i]);
        if (w < 0) return -1;
        width += w;
    }
    return width;
}

int iswalnum(wint_t wc) {
    return iswalpha(wc) || iswdigit(wc);
}

int iswalpha(wint_t wc) {
    if (wc < 128) return isalpha((int)wc);
    return (wc >= 0x00C0 && wc <= 0x024F);
}

int iswblank(wint_t wc) {
    return wc == L' ' || wc == L'\t';
}

int iswcntrl(wint_t wc) {
    return (wc < 32) || (wc >= 0x7f && wc < 0xa0);
}

int iswdigit(wint_t wc) {
    return wc >= L'0' && wc <= L'9';
}

int iswgraph(wint_t wc) {
    return iswprint(wc) && wc != L' ';
}

int iswlower(wint_t wc) {
    if (wc < 128) return islower((int)wc);
    return 0;
}

int iswprint(wint_t wc) {
    if (wc < 128) return isprint((int)wc);
    return wc >= 0xa0;
}

int iswpunct(wint_t wc) {
    return iswgraph(wc) && !iswalnum(wc);
}

int iswspace(wint_t wc) {
    return wc == L' ' || wc == L'\t' || wc == L'\n' || 
           wc == L'\r' || wc == L'\f' || wc == L'\v';
}

int iswupper(wint_t wc) {
    if (wc < 128) return isupper((int)wc);
    return 0;
}

int iswxdigit(wint_t wc) {
    return iswdigit(wc) || (wc >= L'a' && wc <= L'f') || (wc >= L'A' && wc <= L'F');
}

wint_t towlower(wint_t wc) {
    if (wc >= L'A' && wc <= L'Z') return wc + 32;
    return wc;
}

wint_t towupper(wint_t wc) {
    if (wc >= L'a' && wc <= L'z') return wc - 32;
    return wc;
}

int iswctype(wint_t wc, wctype_t desc) {
    switch (desc) {
        case 1: return iswalnum(wc);
        case 2: return iswalpha(wc);
        case 3: return iswblank(wc);
        case 4: return iswcntrl(wc);
        case 5: return iswdigit(wc);
        case 6: return iswgraph(wc);
        case 7: return iswlower(wc);
        case 8: return iswprint(wc);
        case 9: return iswpunct(wc);
        case 10: return iswspace(wc);
        case 11: return iswupper(wc);
        case 12: return iswxdigit(wc);
        default: return 0;
    }
}

wctype_t wctype(const char *property) {
    if (strcmp(property, "alnum") == 0) return 1;
    if (strcmp(property, "alpha") == 0) return 2;
    if (strcmp(property, "blank") == 0) return 3;
    if (strcmp(property, "cntrl") == 0) return 4;
    if (strcmp(property, "digit") == 0) return 5;
    if (strcmp(property, "graph") == 0) return 6;
    if (strcmp(property, "lower") == 0) return 7;
    if (strcmp(property, "print") == 0) return 8;
    if (strcmp(property, "punct") == 0) return 9;
    if (strcmp(property, "space") == 0) return 10;
    if (strcmp(property, "upper") == 0) return 11;
    if (strcmp(property, "xdigit") == 0) return 12;
    return 0;
}

wctrans_t wctrans(const char *charclass) {
    if (strcmp(charclass, "tolower") == 0) return (wctrans_t)1;
    if (strcmp(charclass, "toupper") == 0) return (wctrans_t)2;
    return (wctrans_t)0;
}

wint_t towctrans(wint_t wc, wctrans_t desc) {
    if (desc == (wctrans_t)1) return towlower(wc);
    if (desc == (wctrans_t)2) return towupper(wc);
    return wc;
}

int fwide(FILE *stream, int mode) {
    (void)stream;
    (void)mode;
    return 0;
}

wint_t fgetwc(FILE *stream) {
    int c = fgetc(stream);
    if (c == EOF) return WEOF;
    return (wint_t)(unsigned char)c;
}

wint_t getwc(FILE *stream) {
    return fgetwc(stream);
}

wint_t getwchar(void) {
    return fgetwc(stdin);
}

wint_t fputwc(wchar_t wc, FILE *stream) {
    char buf[4];
    size_t len = wcrtomb(buf, wc, NULL);
    if (len == (size_t)-1) return WEOF;
    for (size_t i = 0; i < len; i++) {
        if (fputc(buf[i], stream) == EOF) return WEOF;
    }
    return (wint_t)wc;
}

wint_t putwc(wchar_t wc, FILE *stream) {
    return fputwc(wc, stream);
}

wint_t putwchar(wchar_t wc) {
    return fputwc(wc, stdout);
}

wint_t ungetwc(wint_t wc, FILE *stream) {
    (void)stream; (void)wc;
    return WEOF;
}

wchar_t *fgetws(wchar_t *ws, int n, FILE *stream) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        wint_t c = fgetwc(stream);
        if (c == WEOF) {
            if (i == 0) return NULL;
            break;
        }
        ws[i++] = (wchar_t)c;
        if (c == L'\n') break;
    }
    ws[i] = 0;
    return ws;
}

int fputws(const wchar_t *ws, FILE *stream) {
    while (*ws) {
        if (fputwc(*ws++, stream) == WEOF) return -1;
    }
    return 0;
}

int fwprintf(FILE *stream, const wchar_t *format, ...) {
    (void)stream; (void)format;
    return 0;
}

int wprintf(const wchar_t *format, ...) {
    (void)format;
    return 0;
}

int swprintf(wchar_t *ws, size_t maxlen, const wchar_t *format, ...) {
    (void)ws; (void)maxlen; (void)format;
    if (maxlen > 0) ws[0] = 0;
    return 0;
}

int vfwprintf(FILE *stream, const wchar_t *format, va_list args) {
    (void)stream; (void)format; (void)args;
    return 0;
}

int vwprintf(const wchar_t *format, va_list args) {
    (void)format; (void)args;
    return 0;
}

int vswprintf(wchar_t *ws, size_t maxlen, const wchar_t *format, va_list args) {
    (void)ws; (void)maxlen; (void)format; (void)args;
    if (maxlen > 0) ws[0] = 0;
    return 0;
}

int fwscanf(FILE *stream, const wchar_t *format, ...) {
    (void)stream; (void)format;
    return 0;
}

int wscanf(const wchar_t *format, ...) {
    (void)format;
    return 0;
}

int swscanf(const wchar_t *ws, const wchar_t *format, ...) {
    (void)ws; (void)format;
    return 0;
}

int vfwscanf(FILE *stream, const wchar_t *format, va_list args) {
    (void)stream; (void)format; (void)args;
    return 0;
}

int vwscanf(const wchar_t *format, va_list args) {
    (void)format; (void)args;
    return 0;
}

int vswscanf(const wchar_t *ws, const wchar_t *format, va_list args) {
    (void)ws; (void)format; (void)args;
    return 0;
}

size_t wcsnlen(const wchar_t *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format, const struct tm *timeptr) {
    (void)format; (void)timeptr;
    if (maxsize > 0) s[0] = 0;
    return 0;
}

long wcstol(const wchar_t *nptr, wchar_t **endptr, int base) {
    long result = 0;
    int sign = 1;
    while (iswspace(*nptr)) nptr++;
    if (*nptr == L'-') { sign = -1; nptr++; }
    else if (*nptr == L'+') nptr++;
    if (base == 0) {
        if (*nptr == L'0') {
            nptr++;
            if (*nptr == L'x' || *nptr == L'X') { base = 16; nptr++; }
            else base = 8;
        } else base = 10;
    }
    while (*nptr) {
        int digit;
        if (*nptr >= L'0' && *nptr <= L'9') digit = *nptr - L'0';
        else if (*nptr >= L'a' && *nptr <= L'z') digit = *nptr - L'a' + 10;
        else if (*nptr >= L'A' && *nptr <= L'Z') digit = *nptr - L'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        nptr++;
    }
    if (endptr) *endptr = (wchar_t *)nptr;
    return result * sign;
}

unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base) {
    unsigned long result = 0;
    while (iswspace(*nptr)) nptr++;
    if (*nptr == L'+') nptr++;
    if (base == 0) {
        if (*nptr == L'0') {
            nptr++;
            if (*nptr == L'x' || *nptr == L'X') { base = 16; nptr++; }
            else base = 8;
        } else base = 10;
    }
    while (*nptr) {
        int digit;
        if (*nptr >= L'0' && *nptr <= L'9') digit = *nptr - L'0';
        else if (*nptr >= L'a' && *nptr <= L'z') digit = *nptr - L'a' + 10;
        else if (*nptr >= L'A' && *nptr <= L'Z') digit = *nptr - L'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        nptr++;
    }
    if (endptr) *endptr = (wchar_t *)nptr;
    return result;
}

long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base) {
    return (long long)wcstol(nptr, endptr, base);
}

unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base) {
    return (unsigned long long)wcstoul(nptr, endptr, base);
}

double wcstod(const wchar_t *nptr, wchar_t **endptr) {
    double result = 0.0;
    int sign = 1;
    while (iswspace(*nptr)) nptr++;
    if (*nptr == L'-') { sign = -1; nptr++; }
    else if (*nptr == L'+') nptr++;
    while (*nptr >= L'0' && *nptr <= L'9') {
        result = result * 10.0 + (*nptr - L'0');
        nptr++;
    }
    if (*nptr == L'.') {
        nptr++;
        double frac = 0.1;
        while (*nptr >= L'0' && *nptr <= L'9') {
            result += (*nptr - L'0') * frac;
            frac *= 0.1;
            nptr++;
        }
    }
    if (endptr) *endptr = (wchar_t *)nptr;
    return result * sign;
}

float wcstof(const wchar_t *nptr, wchar_t **endptr) {
    return (float)wcstod(nptr, endptr);
}

int wcscoll(const wchar_t *s1, const wchar_t *s2) {
    return wcscmp(s1, s2);
}

size_t wcsxfrm(wchar_t *dest, const wchar_t *src, size_t n) {
    size_t len = wcslen(src);
    if (n > 0) wcsncpy(dest, src, n);
    return len;
}

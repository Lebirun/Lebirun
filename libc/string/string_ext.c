#include <string.h>
#include <ctype.h>
#include <stdlib.h>

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

char *stpcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
    return dest;
}

char *stpncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    char *ret = dest + i;
    while (i < n) dest[i++] = '\0';
    return ret;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

char *strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

char *strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t needlelen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, needlelen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!str) str = *saveptr;
    if (!str) return NULL;
    str += strspn(str, delim);
    if (!*str) {
        *saveptr = NULL;
        return NULL;
    }
    char *end = str + strcspn(str, delim);
    if (*end) {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return str;
}

char *strsep(char **stringp, const char *delim) {
    if (!*stringp) return NULL;
    char *start = *stringp;
    char *end = start + strcspn(start, delim);
    if (*end) {
        *end = '\0';
        *stringp = end + 1;
    } else {
        *stringp = NULL;
    }
    return start;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) {
        if (*--p == (unsigned char)c) return (void *)p;
    }
    return NULL;
}

char *strsignal(int sig) {
    static char buf[32];
    static const char *signals[] = {
        "Unknown signal",
        "Hangup",
        "Interrupt",
        "Quit",
        "Illegal instruction",
        "Trace/breakpoint trap",
        "Aborted",
        "Bus error",
        "Floating point exception",
        "Killed",
        "User defined signal 1",
        "Segmentation fault",
        "User defined signal 2",
        "Broken pipe",
        "Alarm clock",
        "Terminated"
    };
    if (sig > 0 && sig < 16) {
        return (char *)signals[sig];
    }
    char *p = buf;
    const char *prefix = "Signal ";
    while (*prefix) *p++ = *prefix++;
    if (sig < 0) {
        *p++ = '-';
        sig = -sig;
    }
    char tmp[12];
    int i = 0;
    do {
        tmp[i++] = '0' + (sig % 10);
        sig /= 10;
    } while (sig);
    while (i > 0) *p++ = tmp[--i];
    *p = '\0';
    return buf;
}

size_t strxfrm(char *dest, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n > 0) {
        size_t copy = (len < n - 1) ? len : n - 1;
        memcpy(dest, src, copy);
        dest[copy] = '\0';
    }
    return len;
}

int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

void *memccpy(void *dest, const void *src, int c, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d = *s++;
        if (*d++ == (unsigned char)c) return d;
    }
    return NULL;
}

int bcmp(const void *s1, const void *s2, size_t n) {
    return memcmp(s1, s2, n);
}

void bcopy(const void *src, void *dest, size_t n) {
    memmove(dest, src, n);
}

void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

char *index(const char *s, int c) {
    return strchr(s, c);
}

char *rindex(const char *s, int c) {
    return strrchr(s, c);
}

int ffs(int i) {
    if (i == 0) return 0;
    int pos = 1;
    while (!(i & 1)) {
        i >>= 1;
        pos++;
    }
    return pos;
}

int ffsl(long i) {
    if (i == 0) return 0;
    int pos = 1;
    while (!(i & 1)) {
        i >>= 1;
        pos++;
    }
    return pos;
}

int ffsll(long long i) {
    if (i == 0) return 0;
    int pos = 1;
    while (!(i & 1)) {
        i >>= 1;
        pos++;
    }
    return pos;
}

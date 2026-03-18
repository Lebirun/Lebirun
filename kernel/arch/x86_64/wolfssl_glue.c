#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/types.h>
#include <wolfssl/wolfcrypt/wc_port.h>
#include <kernel/mem_map.h>
#include <kernel/pit.h>
#include <kernel/rtc.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#undef XMALLOC
#undef XFREE
#undef XREALLOC
#undef XTIME
#undef XGMTIME

void *XMALLOC(size_t n, void *heap, int type)
{
    (void)heap;
    (void)type;
    return kmalloc(n);
}

void XFREE(void *p, void *heap, int type)
{
    (void)heap;
    (void)type;
    if (p) kfree(p);
}

void *XREALLOC(void *p, size_t n, void *heap, int type)
{
    (void)heap;
    (void)type;
    return krealloc(p, n);
}

time_t XTIME(time_t *timer)
{
    time_t now = (time_t)rtc_get_time();
    if (timer) *timer = now;
    return now;
}

struct tm *XGMTIME(const time_t *timer, struct tm *tmp)
{
    time_t t;
    int days, rem, y;
    int mon_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m;

    if (!timer || !tmp) return tmp;
    t = *timer;
    memset(tmp, 0, sizeof(*tmp));

    days = (int)(t / 86400);
    rem = (int)(t % 86400);
    if (rem < 0) { days--; rem += 86400; }

    tmp->tm_hour = rem / 3600;
    rem %= 3600;
    tmp->tm_min = rem / 60;
    tmp->tm_sec = rem % 60;

    tmp->tm_wday = (days + 4) % 7;
    if (tmp->tm_wday < 0) tmp->tm_wday += 7;

    y = 1970;
    while (days >= 365) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
        int ydays = 365 + leap;
        if (days < ydays) break;
        days -= ydays;
        y++;
    }
    tmp->tm_year = y - 1900;
    tmp->tm_yday = days;

    if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))
        mon_days[1] = 29;

    for (m = 0; m < 12; m++) {
        if (days < mon_days[m]) break;
        days -= mon_days[m];
    }
    tmp->tm_mon = m;
    tmp->tm_mday = days + 1;

    return tmp;
}

int kernel_wolfssl_seed(unsigned char *output, unsigned int sz)
{
    uint64_t ticks;
    uint64_t rtc;
    unsigned int i;

    ticks = pit_get_ticks64();
    rtc = rtc_get_time();

    for (i = 0; i < sz; i++) {
        ticks = ticks * 6364136223846793005ULL + 1442695040888963407ULL;
        rtc ^= (uint64_t)(ticks >> 33);
        output[i] = (unsigned char)(ticks ^ rtc ^ (i * 0x9E3779B9U));
    }

    return 0;
}

int kernel_wolfssl_random_block(unsigned char *output, unsigned int sz)
{
    return kernel_wolfssl_seed(output, sz);
}

extern void terminal_writestring(const char *s);

static void put_decimal(unsigned long val, char *buf, int *pos)
{
    char tmp[12];
    int n = 0;

    if (val == 0) {
        buf[(*pos)++] = '0';
        return;
    }
    while (val > 0) {
        tmp[n++] = '0' + (val % 10);
        val /= 10;
    }
    while (n > 0) {
        buf[(*pos)++] = tmp[--n];
    }
}

int kernel_wolfssl_printf(const char *fmt, ...)
{
    char buf[256];
    int pos = 0;
    va_list args;
    const char *p;

    va_start(args, fmt);
    for (p = fmt; *p && pos < 254; p++) {
        if (*p == '%' && *(p + 1)) {
            p++;
            if (*p == 's') {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 254) buf[pos++] = *s++;
            } else if (*p == 'd') {
                int v = va_arg(args, int);
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                put_decimal((unsigned long)v, buf, &pos);
            } else if (*p == 'u') {
                unsigned int v = va_arg(args, unsigned int);
                put_decimal(v, buf, &pos);
            } else if (*p == 'x') {
                unsigned int v = va_arg(args, unsigned int);
                int i;
                int started = 0;
                for (i = 28; i >= 0; i -= 4) {
                    int digit = (v >> i) & 0xF;
                    if (digit || started || i == 0) {
                        buf[pos++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
                        started = 1;
                    }
                }
            } else if (*p == 'p') {
                unsigned long v = (unsigned long)va_arg(args, void *);
                int i;
                buf[pos++] = '0';
                buf[pos++] = 'x';
                for (i = 28; i >= 0; i -= 4) {
                    int digit = (v >> i) & 0xF;
                    buf[pos++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
                }
            } else if (*p == 'l') {
                p++;
                if (*p == 'd') {
                    long v = va_arg(args, long);
                    if (v < 0) { buf[pos++] = '-'; v = -v; }
                    put_decimal((unsigned long)v, buf, &pos);
                } else if (*p == 'u') {
                    unsigned long v = va_arg(args, unsigned long);
                    put_decimal(v, buf, &pos);
                }
            } else {
                buf[pos++] = '%';
                buf[pos++] = *p;
            }
        } else {
            buf[pos++] = *p;
        }
    }
    va_end(args);
    buf[pos] = '\0';
    terminal_writestring(buf);
    return pos;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen;

    if (!*needle) return (char *)haystack;
    nlen = strlen(needle);
    while (*haystack) {
        if (memcmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    size_t i;

    while (*d) d++;
    for (i = 0; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dest;
}

int strcasecmp(const char *s1, const char *s2)
{
    unsigned char c1, c2;

    while (1) {
        c1 = (unsigned char)*s1++;
        c2 = (unsigned char)*s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        if (c1 == 0) return 0;
    }
}

static int tolower_table[257];
static int toupper_table[257];
static int ctype_inited = 0;

static void init_ctype_tables(void)
{
    int i;
    for (i = 0; i < 257; i++) {
        tolower_table[i] = i;
        toupper_table[i] = i;
    }
    for (i = 'A'; i <= 'Z'; i++)
        tolower_table[i] = i + 32;
    for (i = 'a'; i <= 'z'; i++)
        toupper_table[i] = i - 32;
    ctype_inited = 1;
}

const int **__ctype_tolower_loc(void)
{
    static const int *ptr;
    if (!ctype_inited) init_ctype_tables();
    ptr = tolower_table;
    return &ptr;
}

const int **__ctype_toupper_loc(void)
{
    static const int *ptr;
    if (!ctype_inited) init_ctype_tables();
    ptr = toupper_table;
    return &ptr;
}

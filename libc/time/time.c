#include <time.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>

#if !defined(__is_libk)
#include <syscall.h>
#include <unistd.h>
#endif

static struct tm static_tm;
static char static_asctime[26];

static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static const char *day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", 
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_year(int year) {
    return is_leap_year(year) ? 366 : 365;
}

time_t time(time_t *tloc) {
#if !defined(__is_libk)
    time_t t = (time_t)syscall0(SYS_TIME);
    if (tloc) {
        *tloc = t;
    }
    return t;
#else
    (void)tloc;
    return 0;
#endif
}

clock_t clock(void) {
#if !defined(__is_libk)
    return (clock_t)syscall0(SYS_GETTICKS) * (CLOCKS_PER_SEC / 1000);
#else
    return 0;
#endif
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!timep || !result) {
        return NULL;
    }
    
    time_t t = *timep;
    int days = t / 86400;
    int remaining = t % 86400;
    
    if (remaining < 0) {
        remaining += 86400;
        days--;
    }
    
    result->tm_hour = remaining / 3600;
    remaining %= 3600;
    result->tm_min = remaining / 60;
    result->tm_sec = remaining % 60;
    
    result->tm_wday = (days + 4) % 7;
    if (result->tm_wday < 0) {
        result->tm_wday += 7;
    }
    
    int year = 1970;
    while (days >= days_in_year(year)) {
        days -= days_in_year(year);
        year++;
    }
    while (days < 0) {
        year--;
        days += days_in_year(year);
    }
    
    result->tm_year = year - 1900;
    result->tm_yday = days;
    
    int leap = is_leap_year(year);
    int month = 0;
    while (month < 11) {
        int dim = days_in_month[month];
        if (month == 1 && leap) {
            dim++;
        }
        if (days < dim) {
            break;
        }
        days -= dim;
        month++;
    }
    
    result->tm_mon = month;
    result->tm_mday = days + 1;
    result->tm_isdst = 0;
    result->tm_gmtoff = 0;
    result->tm_zone = "UTC";
    
    return result;
}

struct tm *gmtime(const time_t *timep) {
    return gmtime_r(timep, &static_tm);
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    return gmtime_r(timep, result);
}

struct tm *localtime(const time_t *timep) {
    return gmtime_r(timep, &static_tm);
}

time_t mktime(struct tm *tm) {
    if (!tm) {
        return (time_t)-1;
    }
    
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon;
    int day = tm->tm_mday;
    
    while (mon < 0) {
        mon += 12;
        year--;
    }
    while (mon >= 12) {
        mon -= 12;
        year++;
    }
    
    time_t result = 0;
    
    for (int y = 1970; y < year; y++) {
        result += days_in_year(y) * 86400;
    }
    for (int y = year; y < 1970; y++) {
        result -= days_in_year(y) * 86400;
    }
    
    for (int m = 0; m < mon; m++) {
        int dim = days_in_month[m];
        if (m == 1 && is_leap_year(year)) {
            dim++;
        }
        result += dim * 86400;
    }
    
    result += (day - 1) * 86400;
    result += tm->tm_hour * 3600;
    result += tm->tm_min * 60;
    result += tm->tm_sec;
    
    gmtime_r(&result, tm);
    
    return result;
}

static void int_to_str(char *buf, int val, int width) {
    for (int i = width - 1; i >= 0; i--) {
        buf[i] = '0' + (val % 10);
        val /= 10;
    }
}

char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) {
        return NULL;
    }
    
    memcpy(buf, day_names[tm->tm_wday], 3);
    buf[3] = ' ';
    memcpy(buf + 4, month_names[tm->tm_mon], 3);
    buf[7] = ' ';
    
    int day = tm->tm_mday;
    buf[8] = (day >= 10) ? '0' + (day / 10) : ' ';
    buf[9] = '0' + (day % 10);
    buf[10] = ' ';
    
    int_to_str(buf + 11, tm->tm_hour, 2);
    buf[13] = ':';
    int_to_str(buf + 14, tm->tm_min, 2);
    buf[16] = ':';
    int_to_str(buf + 17, tm->tm_sec, 2);
    buf[19] = ' ';
    
    int year = tm->tm_year + 1900;
    int_to_str(buf + 20, year, 4);
    buf[24] = '\n';
    buf[25] = '\0';
    
    return buf;
}

char *asctime(const struct tm *tm) {
    return asctime_r(tm, static_asctime);
}

char *ctime_r(const time_t *timep, char *buf) {
    struct tm tm;
    return asctime_r(localtime_r(timep, &tm), buf);
}

char *ctime(const time_t *timep) {
    return asctime(localtime(timep));
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || !format || !tm || max == 0) {
        return 0;
    }
    
    size_t pos = 0;
    
    while (*format && pos < max - 1) {
        if (*format != '%') {
            s[pos++] = *format++;
            continue;
        }
        format++;
        if (!*format) break;
        
        char tmp[32];
        const char *src = tmp;
        size_t len = 0;
        
        switch (*format) {
            case 'a':
                src = day_names[tm->tm_wday];
                len = 3;
                break;
            case 'A': {
                static const char *full_days[] = {"Sunday", "Monday", "Tuesday", 
                    "Wednesday", "Thursday", "Friday", "Saturday"};
                src = full_days[tm->tm_wday];
                len = strlen(src);
                break;
            }
            case 'b':
            case 'h':
                src = month_names[tm->tm_mon];
                len = 3;
                break;
            case 'B': {
                static const char *full_months[] = {"January", "February", "March",
                    "April", "May", "June", "July", "August", "September",
                    "October", "November", "December"};
                src = full_months[tm->tm_mon];
                len = strlen(src);
                break;
            }
            case 'd':
                int_to_str(tmp, tm->tm_mday, 2);
                len = 2;
                break;
            case 'e':
                if (tm->tm_mday < 10) {
                    tmp[0] = ' ';
                    tmp[1] = '0' + tm->tm_mday;
                } else {
                    int_to_str(tmp, tm->tm_mday, 2);
                }
                len = 2;
                break;
            case 'H':
                int_to_str(tmp, tm->tm_hour, 2);
                len = 2;
                break;
            case 'I': {
                int h = tm->tm_hour % 12;
                if (h == 0) h = 12;
                int_to_str(tmp, h, 2);
                len = 2;
                break;
            }
            case 'j':
                int_to_str(tmp, tm->tm_yday + 1, 3);
                len = 3;
                break;
            case 'm':
                int_to_str(tmp, tm->tm_mon + 1, 2);
                len = 2;
                break;
            case 'M':
                int_to_str(tmp, tm->tm_min, 2);
                len = 2;
                break;
            case 'n':
                tmp[0] = '\n';
                len = 1;
                break;
            case 'p':
                src = tm->tm_hour < 12 ? "AM" : "PM";
                len = 2;
                break;
            case 'S':
                int_to_str(tmp, tm->tm_sec, 2);
                len = 2;
                break;
            case 't':
                tmp[0] = '\t';
                len = 1;
                break;
            case 'u':
                tmp[0] = '0' + (tm->tm_wday == 0 ? 7 : tm->tm_wday);
                len = 1;
                break;
            case 'w':
                tmp[0] = '0' + tm->tm_wday;
                len = 1;
                break;
            case 'y':
                int_to_str(tmp, tm->tm_year % 100, 2);
                len = 2;
                break;
            case 'Y':
                int_to_str(tmp, tm->tm_year + 1900, 4);
                len = 4;
                break;
            case 'Z':
                src = tm->tm_zone ? tm->tm_zone : "UTC";
                len = strlen(src);
                break;
            case '%':
                tmp[0] = '%';
                len = 1;
                break;
            default:
                tmp[0] = '%';
                tmp[1] = *format;
                len = 2;
                break;
        }
        
        format++;
        
        while (len > 0 && pos < max - 1) {
            s[pos++] = *src++;
            len--;
        }
    }
    
    s[pos] = '\0';
    return pos;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
#if !defined(__is_libk)
    int ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    syscall1(SYS_SLEEP, ms);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
#else
    (void)rem;
    return -1;
#endif
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
#if !defined(__is_libk)
    switch (clk_id) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE: {
            time_t t = time(NULL);
            tp->tv_sec = t;
            tp->tv_nsec = 0;
            return 0;
        }
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME: {
            unsigned int ticks = getticks();
            tp->tv_sec = ticks / 1000;
            tp->tv_nsec = (ticks % 1000) * 1000000;
            return 0;
        }
        default:
            errno = EINVAL;
            return -1;
    }
#else
    (void)clk_id;
    tp->tv_sec = 0;
    tp->tv_nsec = 0;
    return 0;
#endif
}

int clock_settime(clockid_t clk_id, const struct timespec *tp) {
    (void)clk_id;
    (void)tp;
    errno = EPERM;
    return -1;
}

int clock_getres(clockid_t clk_id, struct timespec *res) {
    if (!res) {
        errno = EINVAL;
        return -1;
    }
    (void)clk_id;
    res->tv_sec = 0;
    res->tv_nsec = 1000000;
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
#if !defined(__is_libk)
    time_t t = time(NULL);
    tv->tv_sec = t;
    tv->tv_usec = 0;
    return 0;
#else
    tv->tv_sec = 0;
    tv->tv_usec = 0;
    return 0;
#endif
}

int settimeofday(const struct timeval *tv, const void *tz) {
    (void)tv;
    (void)tz;
    errno = EPERM;
    return -1;
}

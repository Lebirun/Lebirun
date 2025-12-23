#ifndef _UTIME_H
#define _UTIME_H 1

#include <sys/types.h>
#include <time.h>
#include <sys/time.h>

#ifndef UTIME_NOW
#define UTIME_NOW 1073741823L
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT 1073741822L
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char *filename, const struct utimbuf *times);
int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
int futimesat(int dirfd, const char *pathname, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

#endif

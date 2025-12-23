#ifndef _SYS_TIME_H
#define _SYS_TIME_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

int getitimer(int which, struct itimerval *curr_value);
int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value);
int utimes(const char *filename, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FD_SETSIZE 1024

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define _FD_BITS (8 * sizeof(unsigned long))
#define FD_ZERO(set) do { \
    unsigned int __i; \
    for (__i = 0; __i < sizeof(fd_set)/sizeof(unsigned long); __i++) \
        ((fd_set*)(set))->fds_bits[__i] = 0; \
} while (0)
#define FD_SET(fd, set) ((set)->fds_bits[(fd) / _FD_BITS] |= (1UL << ((fd) % _FD_BITS)))
#define FD_CLR(fd, set) ((set)->fds_bits[(fd) / _FD_BITS] &= ~(1UL << ((fd) % _FD_BITS)))
#define FD_ISSET(fd, set) ((set)->fds_bits[(fd) / _FD_BITS] & (1UL << ((fd) % _FD_BITS)))

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif

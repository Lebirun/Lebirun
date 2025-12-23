#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    (void)nfds;
    (void)readfds;
    (void)writefds;
    (void)exceptfds;
    if (timeout) {
        usleep(timeout->tv_sec * 1000000 + timeout->tv_usec);
    }
    return 0;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask) {
    (void)sigmask;
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000;
        tvp = &tv;
    }
    return select(nfds, readfds, writefds, exceptfds, tvp);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)fds;
    (void)nfds;
    if (timeout > 0) {
        usleep(timeout * 1000);
    }
    return 0;
}

int getitimer(int which, struct itimerval *curr_value) {
    (void)which;
    if (curr_value) {
        curr_value->it_interval.tv_sec = 0;
        curr_value->it_interval.tv_usec = 0;
        curr_value->it_value.tv_sec = 0;
        curr_value->it_value.tv_usec = 0;
    }
    return 0;
}

int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    (void)which;
    (void)new_value;
    if (old_value) {
        old_value->it_interval.tv_sec = 0;
        old_value->it_interval.tv_usec = 0;
        old_value->it_value.tv_sec = 0;
        old_value->it_value.tv_usec = 0;
    }
    return 0;
}

int utimes(const char *filename, const struct timeval times[2]) {
    (void)filename;
    (void)times;
    return 0;
}

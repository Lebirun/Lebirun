#include <utime.h>
#include <errno.h>
#include <fcntl.h>

int utime(const char *filename, const struct utimbuf *times) {
    (void)filename;
    (void)times;
    errno = ENOSYS;
    return -1;
}

int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags) {
    (void)dirfd;
    (void)pathname;
    (void)times;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int futimens(int fd, const struct timespec times[2]) {
    (void)fd;
    (void)times;
    errno = ENOSYS;
    return -1;
}

int futimesat(int dirfd, const char *pathname, const struct timeval times[2]) {
    (void)dirfd;
    (void)pathname;
    (void)times;
    errno = ENOSYS;
    return -1;
}

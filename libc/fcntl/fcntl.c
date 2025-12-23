#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

#if !defined(__is_libk)

#include <syscall.h>
#include <unistd.h>

static int fd_flags[256] = {0};
static int fd_status_flags[256] = {0};

int creat(const char *pathname, mode_t mode) {
    return open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    
    int result = -1;
    
    switch (cmd) {
        case F_DUPFD: {
            int arg = va_arg(ap, int);
            (void)arg;
            result = -1;
            errno = ENOSYS;
            break;
        }
        case F_DUPFD_CLOEXEC: {
            int arg = va_arg(ap, int);
            (void)arg;
            result = -1;
            errno = ENOSYS;
            break;
        }
        case F_GETFD:
            if (fd >= 0 && fd < 256) {
                result = fd_flags[fd];
            } else {
                errno = EBADF;
                result = -1;
            }
            break;
        case F_SETFD: {
            int arg = va_arg(ap, int);
            if (fd >= 0 && fd < 256) {
                fd_flags[fd] = arg;
                result = 0;
            } else {
                errno = EBADF;
                result = -1;
            }
            break;
        }
        case F_GETFL:
            if (fd >= 0 && fd < 256) {
                result = fd_status_flags[fd];
            } else {
                errno = EBADF;
                result = -1;
            }
            break;
        case F_SETFL: {
            int arg = va_arg(ap, int);
            if (fd >= 0 && fd < 256) {
                fd_status_flags[fd] = arg & (O_APPEND | O_NONBLOCK | O_SYNC | O_DSYNC | O_RSYNC);
                result = 0;
            } else {
                errno = EBADF;
                result = -1;
            }
            break;
        }
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW: {
            struct flock *lock = va_arg(ap, struct flock *);
            (void)lock;
            result = 0;
            break;
        }
        case F_GETOWN:
            result = 0;
            break;
        case F_SETOWN: {
            int arg = va_arg(ap, int);
            (void)arg;
            result = 0;
            break;
        }
        default:
            errno = EINVAL;
            result = -1;
            break;
    }
    
    va_end(ap);
    return result;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    (void)dirfd;
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(ap, mode_t);
    }
    va_end(ap);
    (void)mode;
    return open(pathname, flags);
}

#else

int creat(const char *pathname, mode_t mode) {
    (void)pathname;
    (void)mode;
    return -1;
}

int fcntl(int fd, int cmd, ...) {
    (void)fd;
    (void)cmd;
    return -1;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    (void)dirfd;
    (void)pathname;
    (void)flags;
    return -1;
}

#endif

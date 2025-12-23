#if !defined(__is_libk)

#include <termios.h>
#include <syscall.h>

int tcsendbreak(int fd, int duration) {
    (void)fd;
    (void)duration;
    return 0;
}

int tcdrain(int fd) {
    return syscall1(SYS_TCDRAIN, fd);
}

int tcflush(int fd, int queue_selector) {
    return syscall2(SYS_TCFLUSH, fd, queue_selector);
}

int tcflow(int fd, int action) {
    return syscall2(SYS_TCFLOW, fd, action);
}

#endif

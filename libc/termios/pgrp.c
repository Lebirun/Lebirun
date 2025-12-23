#if !defined(__is_libk)

#include <termios.h>
#include <syscall.h>

int tcgetpgrp(int fd) {
    return syscall1(SYS_TCGETPGRP, fd);
}

int tcsetpgrp(int fd, int pgrp) {
    return syscall2(SYS_TCSETPGRP, fd, pgrp);
}

#endif

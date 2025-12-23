#if !defined(__is_libk)

#include <termios.h>
#include <syscall.h>

int tcgetattr(int fd, struct termios *termios_p) {
    return syscall2(SYS_TCGETATTR, fd, (int)termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    return syscall3(SYS_TCSETATTR, fd, optional_actions, (int)termios_p);
}

#endif

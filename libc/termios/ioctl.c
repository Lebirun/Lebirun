#if !defined(__is_libk)

#include <sys/ioctl.h>
#include <syscall.h>
#include <stdarg.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    int arg = va_arg(ap, int);
    va_end(ap);
    return syscall3(SYS_IOCTL, fd, (int)request, arg);
}

#endif

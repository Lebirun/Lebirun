#if !defined(__is_libk)

#include <syscall.h>
#include <unistd.h>

void _exit(int status) {
	syscall1(SYS_EXIT, status);
	__builtin_unreachable();
}

int read(int fd, void* buf, size_t count) {
	return syscall3(SYS_READ, fd, (int)buf, (int)count);
}

int write(int fd, const void* buf, size_t count) {
	return syscall3(SYS_WRITE, fd, (int)buf, (int)count);
}

#endif

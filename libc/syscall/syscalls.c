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

int getpid(void) {
	return syscall0(SYS_GETPID);
}

int yield(void) {
	return syscall0(SYS_YIELD);
}

int sleep_ms(int ms) {
	return syscall1(SYS_SLEEP, ms);
}

int waitpid(int pid, int *status, int options) {
	(void)options;
	return syscall2(SYS_WAITPID, pid, (int)status);
}

void *sbrk(int inc) {
	return (void*)syscall1(SYS_SBRK, inc);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
	(void)addr; (void)flags; (void)fd; (void)off;
	return (void*)syscall2(SYS_MMAP, (int)len, prot);
}

int kill(int pid, int sig) {
	(void)sig;
	return syscall2(SYS_KILL, pid, sig);
}

unsigned int getticks(void) {
	return (unsigned int)syscall0(SYS_GETTICKS);
}

unsigned int time(void *unused) {
	(void)unused;
	return (unsigned int)syscall0(SYS_TIME);
}

int isatty(int fd) {
	return syscall1(SYS_ISATTY, fd);
}

int fork(void) {
	return syscall0(SYS_FORK);
}

int exec(const void *bin, unsigned int size) {
	return syscall2(SYS_EXEC, (int)bin, (int)size);
}

#endif

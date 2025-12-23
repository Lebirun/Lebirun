#include <stdlib.h>

#if defined(__is_libk)

__attribute__((__noreturn__))
void exit(int status) {
	(void)status;
	__asm__ volatile ("cli");
	while (1) {
		__asm__ volatile ("hlt");
	}
	__builtin_unreachable();
}

#else

#include <unistd.h>

extern void __run_atexit(void);
extern void __set_exit_status(int status);

__attribute__((__noreturn__))
void exit(int status) {
	__set_exit_status(status);
	__run_atexit();
	_exit(status);
	__builtin_unreachable();
}

#endif

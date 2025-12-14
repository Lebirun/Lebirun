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

__attribute__((__noreturn__))
void exit(int status) {
	_exit(status);
	__builtin_unreachable();
}

#endif

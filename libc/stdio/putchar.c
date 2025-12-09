#include <stdio.h>

#if defined(__is_libk)
#include <kernel/tty.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
	terminal_write(&c, sizeof(c));
#else
	/* Userspace stdio: write system call not yet implemented.
	 * For now, this is a no-op in userspace builds.
	 */
	(void)ic;
#endif
	return ic;
}

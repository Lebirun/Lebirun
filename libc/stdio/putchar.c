#include <stdio.h>

#if defined(__is_libk)
#include <kernel/tty.h>

int putchar(int ic) {
	char c = (char) ic;
	terminal_write(&c, sizeof(c));
	return ic;
}

#else
#include <unistd.h>

int putchar(int ic) {
	char c = (char) ic;
	write(STDOUT_FILENO, &c, 1);
	return ic;
}

#endif

#include <stdio.h>

#if defined(__is_libk)

int getchar(void) {
	return EOF;
}

#else

#include <unistd.h>

int getchar(void) {
	char c;
	int n = read(STDIN_FILENO, &c, 1);
	return (n > 0) ? (unsigned char)c : EOF;
}

#endif

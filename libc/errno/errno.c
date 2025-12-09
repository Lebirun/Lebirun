#include <errno.h>

#ifdef errno
#undef errno
#endif

int errno = 0;

int* __errno_location(void) { return &errno; }

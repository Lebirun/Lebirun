#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

void _exit(int status) __attribute__((noreturn));
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);

#endif

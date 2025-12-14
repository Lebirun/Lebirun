#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((__noreturn__))
void _exit(int status);

int read(int fd, void* buf, size_t count);
int write(int fd, const void* buf, size_t count);

#ifdef __cplusplus
}
#endif

#endif

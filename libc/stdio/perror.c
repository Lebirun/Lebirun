#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

void perror(const char *s) {
    char *errmsg = strerror(errno);
    if (s && *s) {
        write(STDERR_FILENO, s, strlen(s));
        write(STDERR_FILENO, ": ", 2);
    }
    write(STDERR_FILENO, errmsg, strlen(errmsg));
    write(STDERR_FILENO, "\n", 1);
}

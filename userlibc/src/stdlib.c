#include <stdlib.h>
#include <unistd.h>

void exit(int status) {
    _exit(status);
}

void abort(void) {
    _exit(127);
}

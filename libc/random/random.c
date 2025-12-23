#include <sys/random.h>
#include <errno.h>
#include <stdint.h>

static uint32_t random_state = 1;

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < buflen; i++) {
        random_state = random_state * 1103515245 + 12345;
        p[i] = (unsigned char)(random_state >> 16);
    }
    return (ssize_t)buflen;
}

int getentropy(void *buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    getrandom(buffer, length, 0);
    return 0;
}

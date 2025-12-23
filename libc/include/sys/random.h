#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H 1

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int getentropy(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif

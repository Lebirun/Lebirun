#ifndef _ALLOCA_H
#define _ALLOCA_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define alloca(size) __builtin_alloca(size)

void *alloca(size_t size);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _ASSERT_H
#define _ASSERT_H 1

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG

#define assert(expr) ((void)0)

#else

#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((__noreturn__))
void __assert_fail(const char *expr, const char *file, int line, const char *func);

#ifdef __cplusplus
}
#endif

#endif

#endif

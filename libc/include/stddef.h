#ifndef _STDDEF_H
#define _STDDEF_H 1

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __SIZE_TYPE__
#define __SIZE_TYPE__ unsigned long
#endif

#ifndef __PTRDIFF_TYPE__
#define __PTRDIFF_TYPE__ long
#endif

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef __cplusplus
typedef int wchar_t;
#endif

typedef long double max_align_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#ifdef __cplusplus
}
#endif

#endif

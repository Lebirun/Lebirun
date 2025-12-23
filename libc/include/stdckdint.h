#ifndef _STDCKDINT_H
#define _STDCKDINT_H 1

#if defined(__GNUC__) && __GNUC__ >= 5

#define ckd_add(result, a, b) __builtin_add_overflow(a, b, result)
#define ckd_sub(result, a, b) __builtin_sub_overflow(a, b, result)
#define ckd_mul(result, a, b) __builtin_mul_overflow(a, b, result)

#else

#define ckd_add(result, a, b) (*(result) = (a) + (b), 0)
#define ckd_sub(result, a, b) (*(result) = (a) - (b), 0)
#define ckd_mul(result, a, b) (*(result) = (a) * (b), 0)

#endif

#endif

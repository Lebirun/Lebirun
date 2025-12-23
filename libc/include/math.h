#ifndef _MATH_H
#define _MATH_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUGE_VAL 1e1000
#define HUGE_VALF 1e1000f
#define HUGE_VALL 1e1000L

#define INFINITY (1.0f / 0.0f)
#define NAN (0.0f / 0.0f)

#define FP_INFINITE 1
#define FP_NAN 2
#define FP_NORMAL 3
#define FP_SUBNORMAL 4
#define FP_ZERO 5

#define FP_ILOGB0 (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2

#define math_errhandling MATH_ERRNO

typedef float float_t;
typedef double double_t;

double acos(double x);
float acosf(float x);
double acosh(double x);
float acoshf(float x);
double asin(double x);
float asinf(float x);
double asinh(double x);
float asinhf(float x);
double atan(double x);
float atanf(float x);
double atan2(double y, double x);
float atan2f(float y, float x);
double atanh(double x);
float atanhf(float x);
double cbrt(double x);
float cbrtf(float x);
double ceil(double x);
float ceilf(float x);
double copysign(double x, double y);
float copysignf(float x, float y);
double cos(double x);
float cosf(float x);
double cosh(double x);
float coshf(float x);
double erf(double x);
float erff(float x);
double erfc(double x);
float erfcf(float x);
double exp(double x);
float expf(float x);
double exp2(double x);
float exp2f(float x);
double expm1(double x);
float expm1f(float x);
double fabs(double x);
float fabsf(float x);
double fdim(double x, double y);
float fdimf(float x, float y);
double floor(double x);
float floorf(float x);
double fma(double x, double y, double z);
float fmaf(float x, float y, float z);
double fmax(double x, double y);
float fmaxf(float x, float y);
double fmin(double x, double y);
float fminf(float x, float y);
double fmod(double x, double y);
float fmodf(float x, float y);
double frexp(double x, int *exp);
float frexpf(float x, int *exp);
double hypot(double x, double y);
float hypotf(float x, float y);
int ilogb(double x);
int ilogbf(float x);
double ldexp(double x, int exp);
float ldexpf(float x, int exp);
double lgamma(double x);
float lgammaf(float x);
long long llrint(double x);
long long llrintf(float x);
long long llround(double x);
long long llroundf(float x);
double log(double x);
float logf(float x);
double log10(double x);
float log10f(float x);
double log1p(double x);
float log1pf(float x);
double log2(double x);
float log2f(float x);
double logb(double x);
float logbf(float x);
long lrint(double x);
long lrintf(float x);
long lround(double x);
long lroundf(float x);
double modf(double x, double *iptr);
float modff(float x, float *iptr);
double nan(const char *tagp);
float nanf(const char *tagp);
double nearbyint(double x);
float nearbyintf(float x);
double nextafter(double x, double y);
float nextafterf(float x, float y);
double nexttoward(double x, long double y);
float nexttowardf(float x, long double y);
double pow(double x, double y);
float powf(float x, float y);
double remainder(double x, double y);
float remainderf(float x, float y);
double remquo(double x, double y, int *quo);
float remquof(float x, float y, int *quo);
double rint(double x);
float rintf(float x);
double round(double x);
float roundf(float x);
double scalbln(double x, long n);
float scalblnf(float x, long n);
double scalbn(double x, int n);
float scalbnf(float x, int n);
double sin(double x);
float sinf(float x);
double sinh(double x);
float sinhf(float x);
double sqrt(double x);
float sqrtf(float x);
double tan(double x);
float tanf(float x);
double tanh(double x);
float tanhf(float x);
double tgamma(double x);
float tgammaf(float x);
double trunc(double x);
float truncf(float x);

long double acosl(long double x);
long double acoshl(long double x);
long double asinl(long double x);
long double asinhl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double atanhl(long double x);
long double cbrtl(long double x);
long double ceill(long double x);
long double copysignl(long double x, long double y);
long double cosl(long double x);
long double coshl(long double x);
long double erfl(long double x);
long double erfcl(long double x);
long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double fabsl(long double x);
long double fdiml(long double x, long double y);
long double floorl(long double x);
long double fmal(long double x, long double y, long double z);
long double fmaxl(long double x, long double y);
long double fminl(long double x, long double y);
long double fmodl(long double x, long double y);
long double frexpl(long double x, int *exp);
long double hypotl(long double x, long double y);
int ilogbl(long double x);
long double ldexpl(long double x, int exp);
long double lgammal(long double x);
long long llrintl(long double x);
long long llroundl(long double x);
long double logl(long double x);
long double log10l(long double x);
long double log1pl(long double x);
long double log2l(long double x);
long double logbl(long double x);
long lrintl(long double x);
long lroundl(long double x);
long double modfl(long double x, long double *iptr);
long double nanl(const char *tagp);
long double nearbyintl(long double x);
long double nextafterl(long double x, long double y);
long double nexttowardl(long double x, long double y);
long double powl(long double x, long double y);
long double remainderl(long double x, long double y);
long double remquol(long double x, long double y, int *quo);
long double rintl(long double x);
long double roundl(long double x);
long double scalblnl(long double x, long n);
long double scalbnl(long double x, int n);
long double sinl(long double x);
long double sinhl(long double x);
long double sqrtl(long double x);
long double tanl(long double x);
long double tanhl(long double x);
long double tgammal(long double x);
long double truncl(long double x);

#ifdef __cplusplus
}
#endif

#endif

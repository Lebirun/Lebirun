#include <math.h>
#include <stdint.h>

static const double PI = 3.14159265358979323846;
static const double E = 2.71828182845904523536;
static const double LN2 = 0.693147180559945309417;
static const double LN10 = 2.30258509299404568402;
static const double LOG2E = 1.44269504088896340736;
static const double LOG10E = 0.434294481903251827651;

typedef union {
    double d;
    uint64_t u;
} double_bits;

typedef union {
    float f;
    uint32_t u;
} float_bits;

static int __isinf(double x) {
    double_bits db;
    db.d = x;
    return (db.u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}

static int __isnan(double x) {
    double_bits db;
    db.d = x;
    return ((db.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
           ((db.u & 0x000FFFFFFFFFFFFFULL) != 0);
}

static int __isinff(float x) {
    float_bits fb;
    fb.f = x;
    return (fb.u & 0x7FFFFFFF) == 0x7F800000;
}

static int __isnanf(float x) {
    float_bits fb;
    fb.f = x;
    return ((fb.u & 0x7F800000) == 0x7F800000) && ((fb.u & 0x007FFFFF) != 0);
}

double fabs(double x) {
    return x < 0 ? -x : x;
}

float fabsf(float x) {
    return x < 0 ? -x : x;
}

double floor(double x) {
    if (__isnan(x) || __isinf(x)) return x;
    long long i = (long long)x;
    return (double)i - (i > x);
}

float floorf(float x) {
    if (__isnanf(x) || __isinff(x)) return x;
    long long i = (long long)x;
    return (float)i - (i > x);
}

double ceil(double x) {
    if (__isnan(x) || __isinf(x)) return x;
    long long i = (long long)x;
    return (double)i + (i < x);
}

float ceilf(float x) {
    if (__isnanf(x) || __isinff(x)) return x;
    long long i = (long long)x;
    return (float)i + (i < x);
}

double trunc(double x) {
    if (__isnan(x) || __isinf(x)) return x;
    return (double)(long long)x;
}

float truncf(float x) {
    if (__isnanf(x) || __isinff(x)) return x;
    return (float)(long long)x;
}

double round(double x) {
    if (__isnan(x) || __isinf(x)) return x;
    return floor(x + 0.5);
}

float roundf(float x) {
    if (__isnanf(x) || __isinff(x)) return x;
    return floorf(x + 0.5f);
}

double fmod(double x, double y) {
    if (y == 0.0 || __isnan(x) || __isnan(y) || __isinf(x)) return NAN;
    if (__isinf(y)) return x;
    return x - trunc(x / y) * y;
}

float fmodf(float x, float y) {
    if (y == 0.0f || __isnanf(x) || __isnanf(y) || __isinff(x)) return NAN;
    if (__isinff(y)) return x;
    return x - truncf(x / y) * y;
}

double sqrt(double x) {
    if (x < 0) return NAN;
    if (x == 0 || __isnan(x) || __isinf(x)) return x;
    double guess = x / 2.0;
    for (int i = 0; i < 50; i++) {
        double newguess = (guess + x / guess) / 2.0;
        if (fabs(newguess - guess) < 1e-15 * fabs(guess)) break;
        guess = newguess;
    }
    return guess;
}

float sqrtf(float x) {
    return (float)sqrt((double)x);
}

double sin(double x) {
    if (__isnan(x) || __isinf(x)) return NAN;
    x = fmod(x, 2.0 * PI);
    if (x < 0) x += 2.0 * PI;
    if (x > PI) x -= 2.0 * PI;
    double result = 0.0;
    double term = x;
    double x2 = x * x;
    for (int n = 1; n <= 15; n++) {
        result += term;
        term *= -x2 / ((2 * n) * (2 * n + 1));
    }
    return result;
}

float sinf(float x) {
    return (float)sin((double)x);
}

double cos(double x) {
    if (__isnan(x) || __isinf(x)) return NAN;
    x = fmod(x, 2.0 * PI);
    if (x < 0) x += 2.0 * PI;
    if (x > PI) x -= 2.0 * PI;
    double result = 0.0;
    double term = 1.0;
    double x2 = x * x;
    for (int n = 1; n <= 15; n++) {
        result += term;
        term *= -x2 / ((2 * n - 1) * (2 * n));
    }
    return result;
}

float cosf(float x) {
    return (float)cos((double)x);
}

double tan(double x) {
    double c = cos(x);
    if (fabs(c) < 1e-15) return HUGE_VAL;
    return sin(x) / c;
}

float tanf(float x) {
    return (float)tan((double)x);
}

double atan(double x) {
    if (__isnan(x)) return NAN;
    if (__isinf(x)) return x > 0 ? PI / 2.0 : -PI / 2.0;
    int negate = 0, invert = 0;
    if (x < 0) { x = -x; negate = 1; }
    if (x > 1.0) { x = 1.0 / x; invert = 1; }
    double result = 0.0;
    double term = x;
    double x2 = x * x;
    for (int n = 0; n < 25; n++) {
        result += term / (2 * n + 1);
        term *= -x2;
    }
    if (invert) result = PI / 2.0 - result;
    return negate ? -result : result;
}

float atanf(float x) {
    return (float)atan((double)x);
}

double atan2(double y, double x) {
    if (__isnan(x) || __isnan(y)) return NAN;
    if (x > 0) return atan(y / x);
    if (x < 0 && y >= 0) return atan(y / x) + PI;
    if (x < 0 && y < 0) return atan(y / x) - PI;
    if (x == 0 && y > 0) return PI / 2.0;
    if (x == 0 && y < 0) return -PI / 2.0;
    return 0.0;
}

float atan2f(float y, float x) {
    return (float)atan2((double)y, (double)x);
}

double asin(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return atan2(x, sqrt(1.0 - x * x));
}

float asinf(float x) {
    return (float)asin((double)x);
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return atan2(sqrt(1.0 - x * x), x);
}

float acosf(float x) {
    return (float)acos((double)x);
}

double exp(double x) {
    if (__isnan(x)) return NAN;
    if (x > 709.0) return HUGE_VAL;
    if (x < -745.0) return 0.0;
    int k = (int)(x / LN2 + (x >= 0 ? 0.5 : -0.5));
    double r = x - k * LN2;
    double result = 1.0;
    double term = 1.0;
    for (int n = 1; n <= 20; n++) {
        term *= r / n;
        result += term;
    }
    double_bits db;
    db.d = result;
    uint64_t exp_bits = (db.u >> 52) & 0x7FF;
    exp_bits += k;
    db.u = (db.u & 0x800FFFFFFFFFFFFFULL) | (exp_bits << 52);
    return db.d;
}

float expf(float x) {
    return (float)exp((double)x);
}

double log(double x) {
    if (x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (__isinf(x)) return HUGE_VAL;
    if (__isnan(x)) return NAN;
    double_bits db;
    db.d = x;
    int exp = ((db.u >> 52) & 0x7FF) - 1023;
    db.u = (db.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = db.d;
    double y = (m - 1.0) / (m + 1.0);
    double y2 = y * y;
    double result = 0.0;
    double term = y;
    for (int n = 0; n < 25; n++) {
        result += term / (2 * n + 1);
        term *= y2;
    }
    result *= 2.0;
    return result + exp * LN2;
}

float logf(float x) {
    return (float)log((double)x);
}

double log10(double x) {
    return log(x) / LN10;
}

float log10f(float x) {
    return (float)log10((double)x);
}

double log2(double x) {
    return log(x) / LN2;
}

float log2f(float x) {
    return (float)log2((double)x);
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return y > 0 ? 0.0 : HUGE_VAL;
    if (x == 1.0) return 1.0;
    if (__isnan(x) || __isnan(y)) return NAN;
    if (x < 0) {
        if (y != floor(y)) return NAN;
        double result = exp(y * log(-x));
        return ((long long)y & 1) ? -result : result;
    }
    return exp(y * log(x));
}

float powf(float x, float y) {
    return (float)pow((double)x, (double)y);
}

double sinh(double x) {
    if (__isnan(x)) return NAN;
    if (__isinf(x)) return x;
    double ex = exp(x);
    return (ex - 1.0 / ex) / 2.0;
}

float sinhf(float x) {
    return (float)sinh((double)x);
}

double cosh(double x) {
    if (__isnan(x)) return NAN;
    if (__isinf(x)) return HUGE_VAL;
    double ex = exp(x);
    return (ex + 1.0 / ex) / 2.0;
}

float coshf(float x) {
    return (float)cosh((double)x);
}

double tanh(double x) {
    if (__isnan(x)) return NAN;
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double ex = exp(2.0 * x);
    return (ex - 1.0) / (ex + 1.0);
}

float tanhf(float x) {
    return (float)tanh((double)x);
}

double asinh(double x) {
    if (__isnan(x) || __isinf(x)) return x;
    return log(x + sqrt(x * x + 1.0));
}

float asinhf(float x) {
    return (float)asinh((double)x);
}

double acosh(double x) {
    if (x < 1.0) return NAN;
    return log(x + sqrt(x * x - 1.0));
}

float acoshf(float x) {
    return (float)acosh((double)x);
}

double atanh(double x) {
    if (x <= -1.0 || x >= 1.0) return NAN;
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

float atanhf(float x) {
    return (float)atanh((double)x);
}

double frexp(double x, int *exp) {
    if (x == 0.0 || __isnan(x) || __isinf(x)) {
        *exp = 0;
        return x;
    }
    double_bits db;
    db.d = x;
    int e = ((db.u >> 52) & 0x7FF) - 1022;
    *exp = e;
    db.u = (db.u & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    return db.d;
}

float frexpf(float x, int *exp) {
    double result = frexp((double)x, exp);
    return (float)result;
}

double ldexp(double x, int exp) {
    if (x == 0.0 || __isnan(x) || __isinf(x)) return x;
    double_bits db;
    db.d = x;
    int e = ((db.u >> 52) & 0x7FF) + exp;
    if (e >= 0x7FF) return x > 0 ? HUGE_VAL : -HUGE_VAL;
    if (e <= 0) return 0.0;
    db.u = (db.u & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)e << 52);
    return db.d;
}

float ldexpf(float x, int exp) {
    return (float)ldexp((double)x, exp);
}

double modf(double x, double *iptr) {
    if (__isnan(x)) {
        *iptr = NAN;
        return NAN;
    }
    if (__isinf(x)) {
        *iptr = x;
        return 0.0;
    }
    *iptr = trunc(x);
    return x - *iptr;
}

float modff(float x, float *iptr) {
    double i;
    float result = (float)modf((double)x, &i);
    *iptr = (float)i;
    return result;
}

double copysign(double x, double y) {
    double_bits bx, by;
    bx.d = x;
    by.d = y;
    bx.u = (bx.u & 0x7FFFFFFFFFFFFFFFULL) | (by.u & 0x8000000000000000ULL);
    return bx.d;
}

float copysignf(float x, float y) {
    float_bits bx, by;
    bx.f = x;
    by.f = y;
    bx.u = (bx.u & 0x7FFFFFFF) | (by.u & 0x80000000);
    return bx.f;
}

double fmax(double x, double y) {
    if (__isnan(x)) return y;
    if (__isnan(y)) return x;
    return x > y ? x : y;
}

float fmaxf(float x, float y) {
    if (__isnanf(x)) return y;
    if (__isnanf(y)) return x;
    return x > y ? x : y;
}

double fmin(double x, double y) {
    if (__isnan(x)) return y;
    if (__isnan(y)) return x;
    return x < y ? x : y;
}

float fminf(float x, float y) {
    if (__isnanf(x)) return y;
    if (__isnanf(y)) return x;
    return x < y ? x : y;
}

double fdim(double x, double y) {
    if (__isnan(x) || __isnan(y)) return NAN;
    return x > y ? x - y : 0.0;
}

float fdimf(float x, float y) {
    if (__isnanf(x) || __isnanf(y)) return NAN;
    return x > y ? x - y : 0.0f;
}

double hypot(double x, double y) {
    return sqrt(x * x + y * y);
}

float hypotf(float x, float y) {
    return sqrtf(x * x + y * y);
}

double cbrt(double x) {
    if (x == 0.0 || __isnan(x) || __isinf(x)) return x;
    int neg = x < 0;
    if (neg) x = -x;
    double result = pow(x, 1.0 / 3.0);
    return neg ? -result : result;
}

float cbrtf(float x) {
    return (float)cbrt((double)x);
}

int ilogb(double x) {
    if (x == 0.0) return FP_ILOGB0;
    if (__isnan(x)) return FP_ILOGBNAN;
    if (__isinf(x)) return 0x7FFFFFFF;
    double_bits db;
    db.d = x;
    return ((db.u >> 52) & 0x7FF) - 1023;
}

int ilogbf(float x) {
    return ilogb((double)x);
}

double logb(double x) {
    return (double)ilogb(x);
}

float logbf(float x) {
    return (float)ilogb(x);
}

double scalbn(double x, int n) {
    return ldexp(x, n);
}

float scalbnf(float x, int n) {
    return ldexpf(x, n);
}

double scalbln(double x, long n) {
    return ldexp(x, (int)n);
}

float scalblnf(float x, long n) {
    return ldexpf(x, (int)n);
}

double log1p(double x) {
    if (x <= -1.0) return -HUGE_VAL;
    if (fabs(x) < 1e-4) {
        return x - x * x / 2.0 + x * x * x / 3.0;
    }
    return log(1.0 + x);
}

float log1pf(float x) {
    return (float)log1p((double)x);
}

double expm1(double x) {
    if (fabs(x) < 1e-4) {
        return x + x * x / 2.0 + x * x * x / 6.0;
    }
    return exp(x) - 1.0;
}

float expm1f(float x) {
    return (float)expm1((double)x);
}

double exp2(double x) {
    return pow(2.0, x);
}

float exp2f(float x) {
    return powf(2.0f, x);
}

double rint(double x) {
    return round(x);
}

float rintf(float x) {
    return roundf(x);
}

double nearbyint(double x) {
    return round(x);
}

float nearbyintf(float x) {
    return roundf(x);
}

long lrint(double x) {
    return (long)round(x);
}

long lrintf(float x) {
    return (long)roundf(x);
}

long long llrint(double x) {
    return (long long)round(x);
}

long long llrintf(float x) {
    return (long long)roundf(x);
}

long lround(double x) {
    return (long)round(x);
}

long lroundf(float x) {
    return (long)roundf(x);
}

long long llround(double x) {
    return (long long)round(x);
}

long long llroundf(float x) {
    return (long long)roundf(x);
}

double remainder(double x, double y) {
    if (y == 0.0) return NAN;
    double q = x / y;
    double n = round(q);
    return x - n * y;
}

float remainderf(float x, float y) {
    return (float)remainder((double)x, (double)y);
}

double remquo(double x, double y, int *quo) {
    if (y == 0.0) {
        *quo = 0;
        return NAN;
    }
    double q = x / y;
    double n = round(q);
    *quo = (int)n & 0x7;
    if ((x < 0) != (y < 0)) *quo = -*quo;
    return x - n * y;
}

float remquof(float x, float y, int *quo) {
    return (float)remquo((double)x, (double)y, quo);
}

double nan(const char *tagp) {
    (void)tagp;
    return NAN;
}

float nanf(const char *tagp) {
    (void)tagp;
    return NAN;
}

double nextafter(double x, double y) {
    if (__isnan(x) || __isnan(y)) return NAN;
    if (x == y) return y;
    double_bits db;
    db.d = x;
    if (x == 0.0) {
        db.u = 1;
        return y > 0 ? db.d : -db.d;
    }
    if ((x > 0 && y > x) || (x < 0 && y < x)) {
        db.u++;
    } else {
        db.u--;
    }
    return db.d;
}

float nextafterf(float x, float y) {
    if (__isnanf(x) || __isnanf(y)) return NAN;
    if (x == y) return y;
    float_bits fb;
    fb.f = x;
    if (x == 0.0f) {
        fb.u = 1;
        return y > 0 ? fb.f : -fb.f;
    }
    if ((x > 0 && y > x) || (x < 0 && y < x)) {
        fb.u++;
    } else {
        fb.u--;
    }
    return fb.f;
}

double nexttoward(double x, long double y) {
    return nextafter(x, (double)y);
}

float nexttowardf(float x, long double y) {
    return nextafterf(x, (float)y);
}

double fma(double x, double y, double z) {
    return x * y + z;
}

float fmaf(float x, float y, float z) {
    return x * y + z;
}

double erf(double x) {
    double a1 = 0.254829592;
    double a2 = -0.284496736;
    double a3 = 1.421413741;
    double a4 = -1.453152027;
    double a5 = 1.061405429;
    double p = 0.3275911;
    int sign = x < 0 ? -1 : 1;
    x = fabs(x);
    double t = 1.0 / (1.0 + p * x);
    double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-x * x);
    return sign * y;
}

float erff(float x) {
    return (float)erf((double)x);
}

double erfc(double x) {
    return 1.0 - erf(x);
}

float erfcf(float x) {
    return (float)erfc((double)x);
}

double lgamma(double x) {
    static const double c[7] = {
        1.000000000190015,
        76.18009172947146,
        -86.50532032941677,
        24.01409824083091,
        -1.231739572450155,
        0.1208650973866179e-2,
        -0.5395239384953e-5
    };
    if (x <= 0 && x == floor(x)) return HUGE_VAL;
    double y = x;
    double tmp = x + 5.5;
    tmp -= (x + 0.5) * log(tmp);
    double ser = c[0];
    for (int j = 1; j < 7; j++) {
        ser += c[j] / ++y;
    }
    return -tmp + log(2.5066282746310005 * ser / x);
}

float lgammaf(float x) {
    return (float)lgamma((double)x);
}

double tgamma(double x) {
    if (x <= 0 && x == floor(x)) return NAN;
    return exp(lgamma(x));
}

float tgammaf(float x) {
    return (float)tgamma((double)x);
}

long double acosl(long double x) { return (long double)acos((double)x); }
long double acoshl(long double x) { return (long double)acosh((double)x); }
long double asinl(long double x) { return (long double)asin((double)x); }
long double asinhl(long double x) { return (long double)asinh((double)x); }
long double atanl(long double x) { return (long double)atan((double)x); }
long double atan2l(long double y, long double x) { return (long double)atan2((double)y, (double)x); }
long double atanhl(long double x) { return (long double)atanh((double)x); }
long double cbrtl(long double x) { return (long double)cbrt((double)x); }
long double ceill(long double x) { return (long double)ceil((double)x); }
long double copysignl(long double x, long double y) { return (long double)copysign((double)x, (double)y); }
long double cosl(long double x) { return (long double)cos((double)x); }
long double coshl(long double x) { return (long double)cosh((double)x); }
long double erfl(long double x) { return (long double)erf((double)x); }
long double erfcl(long double x) { return (long double)erfc((double)x); }
long double expl(long double x) { return (long double)exp((double)x); }
long double exp2l(long double x) { return (long double)exp2((double)x); }
long double expm1l(long double x) { return (long double)expm1((double)x); }
long double fabsl(long double x) { return x < 0 ? -x : x; }
long double fdiml(long double x, long double y) { return (long double)fdim((double)x, (double)y); }
long double floorl(long double x) { return (long double)floor((double)x); }
long double fmal(long double x, long double y, long double z) { return x * y + z; }
long double fmaxl(long double x, long double y) { return (long double)fmax((double)x, (double)y); }
long double fminl(long double x, long double y) { return (long double)fmin((double)x, (double)y); }
long double fmodl(long double x, long double y) { return (long double)fmod((double)x, (double)y); }
long double frexpl(long double x, int *exp) { return (long double)frexp((double)x, exp); }
long double hypotl(long double x, long double y) { return (long double)hypot((double)x, (double)y); }
int ilogbl(long double x) { return ilogb((double)x); }
long double ldexpl(long double x, int exp) { return (long double)ldexp((double)x, exp); }
long double lgammal(long double x) { return (long double)lgamma((double)x); }
long long llrintl(long double x) { return llrint((double)x); }
long long llroundl(long double x) { return llround((double)x); }
long double logl(long double x) { return (long double)log((double)x); }
long double log10l(long double x) { return (long double)log10((double)x); }
long double log1pl(long double x) { return (long double)log1p((double)x); }
long double log2l(long double x) { return (long double)log2((double)x); }
long double logbl(long double x) { return (long double)logb((double)x); }
long lrintl(long double x) { return lrint((double)x); }
long lroundl(long double x) { return lround((double)x); }
long double modfl(long double x, long double *iptr) { double i; long double r = (long double)modf((double)x, &i); *iptr = (long double)i; return r; }
long double nanl(const char *tagp) { (void)tagp; return NAN; }
long double nearbyintl(long double x) { return (long double)nearbyint((double)x); }
long double nextafterl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
long double nexttowardl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
long double powl(long double x, long double y) { return (long double)pow((double)x, (double)y); }
long double remainderl(long double x, long double y) { return (long double)remainder((double)x, (double)y); }
long double remquol(long double x, long double y, int *quo) { return (long double)remquo((double)x, (double)y, quo); }
long double rintl(long double x) { return (long double)rint((double)x); }
long double roundl(long double x) { return (long double)round((double)x); }
long double scalblnl(long double x, long n) { return (long double)scalbln((double)x, n); }
long double scalbnl(long double x, int n) { return (long double)scalbn((double)x, n); }
long double sinl(long double x) { return (long double)sin((double)x); }
long double sinhl(long double x) { return (long double)sinh((double)x); }
long double sqrtl(long double x) { return (long double)sqrt((double)x); }
long double tanl(long double x) { return (long double)tan((double)x); }
long double tanhl(long double x) { return (long double)tanh((double)x); }
long double tgammal(long double x) { return (long double)tgamma((double)x); }
long double truncl(long double x) { return (long double)trunc((double)x); }

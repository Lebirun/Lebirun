#include <stdlib.h>

int abs(int j) {
    return j < 0 ? -j : j;
}

long labs(long j) {
    return j < 0 ? -j : j;
}

long long llabs(long long j) {
    return j < 0 ? -j : j;
}

div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

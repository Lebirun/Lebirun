#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "tls_int.h"

#define FE_MASK51 0x7FFFFFFFFFFFFLL

typedef int64_t fe[5];

static void fe_carry(fe h) {
    int i;
    int64_t c;

    for (i = 0; i < 4; i++) {
        c = h[i] >> 51;
        h[i] -= c << 51;
        h[i + 1] += c;
    }
    c = h[4] >> 51;
    h[4] -= c << 51;
    h[0] += c * 19;
    c = h[0] >> 51;
    h[0] -= c << 51;
    h[1] += c;
}

static void fe_add(fe h, const fe f, const fe g) {
    int i;

    for (i = 0; i < 5; i++)
        h[i] = f[i] + g[i];
    fe_carry(h);
}

static void fe_sub(fe h, const fe f, const fe g) {
    static const int64_t bias[5] = {
        4503599627370458LL, 4503599627370494LL, 4503599627370494LL,
        4503599627370494LL, 4503599627370494LL
    };
    int i;

    for (i = 0; i < 5; i++)
        h[i] = f[i] - g[i] + bias[i];
    fe_carry(h);
}

static void fe_mul(fe h, const fe f, const fe g) {
    unsigned __int128 t[5];
    unsigned __int128 c;
    int i;
    int j;

    for (i = 0; i < 5; i++)
        t[i] = 0;
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 5; j++) {
            if (i + j < 5)
                t[i + j] += (unsigned __int128)(uint64_t)f[i] * (uint64_t)g[j];
            else
                t[i + j - 5] += 19 * (unsigned __int128)(uint64_t)f[i] * (uint64_t)g[j];
        }
    }
    for (i = 0; i < 4; i++) {
        c = t[i] >> 51;
        t[i] -= c << 51;
        t[i + 1] += c;
    }
    c = t[4] >> 51;
    t[4] -= c << 51;
    t[0] += c * 19;
    c = t[0] >> 51;
    t[0] -= c << 51;
    t[1] += c;
    for (i = 0; i < 5; i++)
        h[i] = (int64_t)t[i];
}

static void fe_sq(fe h, const fe f) {
    fe_mul(h, f, f);
}

static void fe_pow(fe out, const fe a, const uint8_t exp[32]) {
    fe base;
    fe acc;
    int i;
    int bit;

    memcpy(base, a, sizeof(base));
    memset(acc, 0, sizeof(acc));
    acc[0] = 1;
    for (i = 0; i < 256; i++) {
        bit = (exp[i / 8] >> (7 - (i % 8))) & 1;
        fe_sq(acc, acc);
        if (bit)
            fe_mul(acc, acc, base);
    }
    memcpy(out, acc, sizeof(acc));
}

static void fe_cswap(fe f, fe g, int swap) {
    int i;
    int64_t mask;
    int64_t t;

    mask = (int64_t)(0 - swap);
    for (i = 0; i < 5; i++) {
        t = mask & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

static void fe_unpack(fe h, const uint8_t in[32]) {
    unsigned __int128 buf;
    int bits;
    int i;
    int o;

    for (i = 0; i < 5; i++)
        h[i] = 0;
    buf = 0;
    bits = 0;
    o = 0;
    for (i = 0; i < 32; i++) {
        buf |= (unsigned __int128)in[i] << bits;
        bits += 8;
        while (bits >= 51 && o < 5) {
            h[o++] = (int64_t)(buf & (unsigned __int128)FE_MASK51);
            buf >>= 51;
            bits -= 51;
        }
    }
    if (o < 5)
        h[o] = (int64_t)buf;
}

static void fe_serialize(uint8_t out[32], const fe h) {
    static const int64_t p[5] = {
        2251799813685229LL, 2251799813685247LL, 2251799813685247LL,
        2251799813685247LL, 2251799813685247LL
    };
    fe t;
    int64_t d[5];
    int64_t b;
    int i;
    int k;
    unsigned __int128 buf;
    int bits;
    int o;

    memcpy(t, h, sizeof(t));
    fe_carry(t);
    fe_carry(t);
    for (k = 0; k < 16; k++) {
        b = 0;
        for (i = 0; i < 5; i++) {
            d[i] = t[i] - p[i] - b;
            b = d[i] < 0 ? 1 : 0;
        }
        if (b != 0)
            break;
        for (i = 0; i < 5; i++)
            t[i] = d[i];
    }
    buf = 0;
    bits = 0;
    o = 0;
    for (i = 0; i < 5 && o < 32; i++) {
        buf |= (unsigned __int128)(uint64_t)t[i] << bits;
        bits += 51;
        while (bits >= 8 && o < 32) {
            out[o++] = (uint8_t)(buf & 0xFF);
            buf >>= 8;
            bits -= 8;
        }
    }
    if (bits > 0 && o < 32) {
        out[o++] = (uint8_t)(buf & 0xFF);
        buf >>= 8;
        bits = 0;
    }
    while (o < 32)
        out[o++] = 0;
}

static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                              const uint8_t point[32]) {
    static const uint8_t inv_exp[32] = {
        0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEB
    };
    fe x1;
    fe x2;
    fe z2;
    fe x3;
    fe z3;
    fe tmp0;
    fe tmp1;
    fe tmp2;
    fe tmp3;
    fe tmp4;
    fe a24;
    int i;
    int k;
    int swap;

    fe_unpack(x1, point);
    memset(x2, 0, sizeof(x2));
    memset(z2, 0, sizeof(z2));
    memset(z3, 0, sizeof(z3));
    x2[0] = 1;
    memcpy(x3, x1, sizeof(x3));
    z3[0] = 1;
    memset(a24, 0, sizeof(a24));
    a24[0] = 121665;
    swap = 0;
    for (i = 255; i >= 0; i--) {
        k = (scalar[i / 8] >> (i % 8)) & 1;
        swap ^= k;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = k;

        fe_add(tmp0, x2, z2);
        fe_sub(tmp1, x2, z2);
        fe_add(tmp2, x3, z3);
        fe_sub(tmp3, x3, z3);
        fe_mul(tmp4, tmp3, tmp0);
        fe_mul(tmp3, tmp2, tmp1);
        fe_add(tmp2, tmp4, tmp3);
        fe_sq(x3, tmp2);
        fe_sub(tmp2, tmp4, tmp3);
        fe_sq(tmp2, tmp2);
        fe_mul(z3, x1, tmp2);
        fe_sq(tmp0, tmp0);
        fe_sq(tmp1, tmp1);
        fe_mul(x2, tmp0, tmp1);
        fe_sub(tmp2, tmp0, tmp1);
        fe_mul(tmp4, a24, tmp2);
        fe_add(tmp4, tmp4, tmp0);
        fe_mul(z2, tmp2, tmp4);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_pow(tmp0, z2, inv_exp);
    fe_mul(tmp0, tmp0, x2);
    fe_serialize(out, tmp0);
}

static const uint8_t x25519_base[32] = {
    9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void x25519_clamp(uint8_t s[32]) {
    s[0] &= 248;
    s[31] &= 127;
    s[31] |= 64;
}

int lebtls_x25519_pub(const uint8_t priv[32], uint8_t pub[32]) {
    uint8_t s[32];

    if (!priv || !pub)
        return -1;
    memcpy(s, priv, 32);
    x25519_clamp(s);
    x25519_scalarmult(pub, s, x25519_base);
    memset(s, 0, sizeof(s));
    return 0;
}

int lebtls_x25519_shared(const uint8_t priv[32], const uint8_t peer[32],
                         uint8_t out[32]) {
    uint8_t s[32];

    if (!priv || !peer || !out)
        return -1;
    memcpy(s, priv, 32);
    x25519_clamp(s);
    x25519_scalarmult(out, s, peer);
    memset(s, 0, sizeof(s));
    return 0;
}

int lebtls_x25519_selftest(void) {
    static const uint8_t s0[32] = {
        0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
        0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
        0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
        0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4
    };
    static const uint8_t u0[32] = {
        0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
        0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
        0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
        0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c
    };
    static const uint8_t e0[32] = {
        0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
        0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
        0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
        0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52
    };
    static const uint8_t s1[32] = {
        0x4b, 0x66, 0xe9, 0xd4, 0xd1, 0xb4, 0x67, 0x3c,
        0x5a, 0xd2, 0x26, 0x91, 0x95, 0x7d, 0x6a, 0xf5,
        0xc1, 0x1b, 0x64, 0x21, 0xe0, 0xea, 0x01, 0xd4,
        0x2c, 0xa4, 0x16, 0x9e, 0x79, 0x18, 0xba, 0x0d
    };
    static const uint8_t u1[32] = {
        0xe5, 0x21, 0x0f, 0x12, 0x78, 0x68, 0x11, 0xd3,
        0xf4, 0xb7, 0x95, 0x9d, 0x05, 0x38, 0xae, 0x2c,
        0x31, 0xdb, 0xe7, 0x10, 0x6f, 0xc0, 0x3c, 0x3e,
        0xfc, 0x4c, 0xd5, 0x49, 0xc7, 0x15, 0xa4, 0x93
    };
    static const uint8_t e1[32] = {
        0x95, 0xcb, 0xde, 0x94, 0x76, 0xe8, 0x90, 0x7d,
        0x7a, 0xad, 0xe4, 0x5c, 0xb4, 0xb8, 0x73, 0xf8,
        0x8b, 0x59, 0x5a, 0x68, 0x79, 0x9f, 0xa1, 0x52,
        0xe6, 0xf8, 0xf7, 0x64, 0x7a, 0xac, 0x79, 0x57
    };
    static const uint8_t iter_u[32] = {
        0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t iter_e[32] = {
        0x42, 0x2c, 0x8e, 0x7a, 0x62, 0x27, 0xd7, 0xbc,
        0xa1, 0x35, 0x0b, 0x3e, 0x2b, 0xb7, 0x27, 0x9f,
        0x78, 0x97, 0xb8, 0x7b, 0xb6, 0x85, 0x4b, 0x78,
        0x3c, 0x60, 0xe8, 0x03, 0x11, 0xae, 0x30, 0x79
    };
    uint8_t sc[32];
    uint8_t out[32];
    int i;

    for (i = 0; i < 2; i++) {
        const uint8_t *s = i == 0 ? s0 : s1;
        const uint8_t *u = i == 0 ? u0 : u1;
        const uint8_t *e = i == 0 ? e0 : e1;

        memcpy(sc, s, 32);
        x25519_clamp(sc);
        x25519_scalarmult(out, sc, u);
        memset(sc, 0, sizeof(sc));
        if (memcmp(out, e, 32) != 0) {
            memset(out, 0, sizeof(out));
            return -1;
        }
    }
    memcpy(sc, iter_u, 32);
    x25519_clamp(sc);
    x25519_scalarmult(out, sc, iter_u);
    memset(sc, 0, sizeof(sc));
    if (memcmp(out, iter_e, 32) != 0) {
        memset(out, 0, sizeof(out));
        return -1;
    }
    memset(out, 0, sizeof(out));
    return 0;
}

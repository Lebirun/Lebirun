#include <lebirun/mem_map.h>
#include <lebirun/crypto.h>
#include <string.h>
#include "tls_int.h"

#define P256_LIMBS 8

static const uint32_t p256_p[P256_LIMBS] = {
    0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0x00000000UL,
    0x00000000UL, 0x00000000UL, 0x00000001UL, 0xFFFFFFFFUL
};

static const uint32_t p256_n[P256_LIMBS] = {
    0xFC632551UL, 0xF3B9CAC2UL, 0xA7179E84UL, 0xBCE6FAADUL,
    0xFFFFFFFFUL, 0xFFFFFFFFUL, 0x00000000UL, 0xFFFFFFFFUL
};

static const uint32_t p256_gx[P256_LIMBS] = {
    0xD898C296UL, 0xF4A13945UL, 0x2DEB33A0UL, 0x77037D81UL,
    0x63A440F2UL, 0xF8BCE6E5UL, 0xE12C4247UL, 0x6B17D1F2UL
};

static const uint32_t p256_gy[P256_LIMBS] = {
    0x37BF51F5UL, 0xCBB64068UL, 0x6B315ECEUL, 0x2BCE3357UL,
    0x7C0F9E16UL, 0x8EE7EB4AUL, 0xFE1A7F9BUL, 0x4FE342E2UL
};

static const uint32_t p256_b[P256_LIMBS] = {
    0x27D2604BUL, 0x3BCE3C3EUL, 0xCC53B0F6UL, 0x651D06B0UL,
    0x769886BCUL, 0xB3EBBD55UL, 0xAA3A93E7UL, 0x5AC635D8UL
};

static int p256_cmp(const uint32_t *a, const uint32_t *b) {
    int i;

    for (i = 7; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static void p256_sub_raw(uint32_t r[P256_LIMBS], const uint32_t *a,
                         const uint32_t *b) {
    uint64_t borrow;
    int i;

    borrow = 0;
    for (i = 0; i < 8; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
}

static void p256_add(uint32_t r[P256_LIMBS], const uint32_t *a,
                     const uint32_t *b) {
    uint32_t t[9];
    uint64_t c;
    uint64_t borrow;
    int i;

    c = 0;
    for (i = 0; i < 8; i++) {
        c += (uint64_t)a[i] + b[i];
        t[i] = (uint32_t)c;
        c >>= 32;
    }
    t[8] = (uint32_t)c;
    if (t[8] != 0 || p256_cmp(t, p256_p) >= 0) {
        borrow = 0;
        for (i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)t[i] - p256_p[i] - borrow;
            t[i] = (uint32_t)v;
            borrow = (v >> 63) & 1;
        }
        t[8] -= (uint32_t)borrow;
    }
    memcpy(r, t, 8 * sizeof(uint32_t));
}

static void p256_sub(uint32_t r[P256_LIMBS], const uint32_t *a,
                     const uint32_t *b) {
    if (p256_cmp(a, b) < 0) {
        uint32_t t[P256_LIMBS];

        p256_sub_raw(t, b, a);
        p256_sub_raw(r, p256_p, t);
    } else {
        p256_sub_raw(r, a, b);
    }
}

static void p256_carry_fold(__int128 acc[16]) {
    int i;

    for (i = 0; i < 15; i++) {
        __int128 c = acc[i] >> 32;
        acc[i] -= c << 32;
        acc[i + 1] += c;
    }
}

static void p256_mul(uint32_t r[P256_LIMBS], const uint32_t *a,
                     const uint32_t *b) {
    __int128 acc[16];
    int i;
    int j;
    int iter;

    for (i = 0; i < 16; i++)
        acc[i] = 0;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++)
            acc[i + j] += (__int128)(uint64_t)a[i] * (uint64_t)b[j];
    }
    for (iter = 0; iter < 16; iter++) {
        __int128 hi[8];
        int empty;

        p256_carry_fold(acc);
        empty = 1;
        for (i = 0; i < 8; i++) {
            hi[i] = acc[i + 8];
            acc[i + 8] = 0;
            if (hi[i] != 0)
                empty = 0;
        }
        if (empty)
            break;
        for (i = 0; i < 8; i++) {
            acc[i + 7] += hi[i];
            acc[i + 6] -= hi[i];
            acc[i + 3] -= hi[i];
            acc[i] += hi[i];
        }
    }
    for (i = 0; i < 8; i++)
        r[i] = (uint32_t)acc[i];
    for (i = 0; i < 8; i++) {
        if (p256_cmp(r, p256_p) < 0)
            break;
        p256_sub_raw(r, r, p256_p);
    }
}

static void p256_sq(uint32_t r[P256_LIMBS], const uint32_t a[P256_LIMBS]) {
    p256_mul(r, a, a);
}

static void p256_pow(uint32_t out[P256_LIMBS], const uint32_t *a,
                     const uint8_t *exp, size_t exp_len) {
    uint32_t base[P256_LIMBS];
    uint32_t acc[P256_LIMBS];
    size_t i;
    int b;
    int bit;

    memcpy(base, a, sizeof(base));
    memset(acc, 0, sizeof(acc));
    acc[0] = 1;
    for (i = 0; i < exp_len; i++) {
        for (b = 7; b >= 0; b--) {
            bit = (exp[i] >> b) & 1;
            p256_sq(acc, acc);
            if (bit) {
                uint32_t t[P256_LIMBS];
                p256_mul(t, acc, base);
                memcpy(acc, t, sizeof(acc));
            }
        }
    }
    memcpy(out, acc, sizeof(acc));
}

static void p256_inv(uint32_t out[P256_LIMBS], const uint32_t *a) {
    static const uint8_t exp[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD
    };

    p256_pow(out, a, exp, sizeof(exp));
}

typedef struct {
    uint32_t x[P256_LIMBS];
    uint32_t y[P256_LIMBS];
    uint32_t z[P256_LIMBS];
} p256_jac_t;

static int p256_is_zero(const uint32_t *a) {
    int i;

    for (i = 0; i < 8; i++) {
        if (a[i] != 0)
            return 0;
    }
    return 1;
}

static void p256_point_double(p256_jac_t *r, const p256_jac_t *p) {
    uint32_t xx[8];
    uint32_t yy[8];
    uint32_t yyyy[8];
    uint32_t zz[8];
    uint32_t s[8];
    uint32_t m[8];
    uint32_t t[8];
    uint32_t zzzz[8];
    uint32_t three[8];

    if (p256_is_zero(p->z)) {
        memset(r, 0, sizeof(*r));
        return;
    }
    memset(three, 0, sizeof(three));
    three[0] = 3;
    p256_sq(xx, p->x);
    p256_sq(yy, p->y);
    p256_sq(yyyy, yy);
    p256_sq(zz, p->z);
    p256_sq(zzzz, zz);
    p256_add(s, p->x, yy);
    p256_sq(s, s);
    p256_sub(s, s, xx);
    p256_sub(s, s, yyyy);
    p256_add(s, s, s);
    p256_mul(m, three, xx);
    p256_mul(t, three, zzzz);
    p256_sub(m, m, t);
    p256_sq(t, m);
    p256_sub(t, t, s);
    p256_sub(t, t, s);
    memcpy(r->x, t, sizeof(r->x));
    p256_add(t, p->y, p->z);
    p256_sq(t, t);
    p256_sub(t, t, yy);
    p256_sub(t, t, zz);
    memcpy(r->z, t, sizeof(r->z));
    p256_sub(t, s, r->x);
    p256_mul(t, m, t);
    p256_add(yyyy, yyyy, yyyy);
    p256_add(yyyy, yyyy, yyyy);
    p256_add(yyyy, yyyy, yyyy);
    p256_sub(r->y, t, yyyy);
}

static void p256_point_add(p256_jac_t *r, const p256_jac_t *p,
                           const p256_jac_t *q) {
    uint32_t z1z1[8];
    uint32_t z2z2[8];
    uint32_t u1[8];
    uint32_t u2[8];
    uint32_t s1[8];
    uint32_t s2[8];
    uint32_t h[8];
    uint32_t i[8];
    uint32_t j[8];
    uint32_t rr[8];
    uint32_t v[8];
    uint32_t t[8];

    if (p256_is_zero(p->z)) {
        memcpy(r, q, sizeof(*r));
        return;
    }
    if (p256_is_zero(q->z)) {
        memcpy(r, p, sizeof(*r));
        return;
    }
    p256_sq(z1z1, p->z);
    p256_sq(z2z2, q->z);
    p256_mul(u1, p->x, z2z2);
    p256_mul(u2, q->x, z1z1);
    p256_mul(t, q->z, z2z2);
    p256_mul(s1, p->y, t);
    p256_mul(t, p->z, z1z1);
    p256_mul(s2, q->y, t);
    if (p256_cmp(u1, u2) == 0) {
        if (p256_cmp(s1, s2) != 0) {
            memset(r, 0, sizeof(*r));
            return;
        }
        p256_point_double(r, p);
        return;
    }
    p256_sub(h, u2, u1);
    p256_add(i, h, h);
    p256_sq(i, i);
    p256_mul(j, h, i);
    p256_sub(rr, s2, s1);
    p256_add(rr, rr, rr);
    p256_mul(v, u1, i);
    p256_sq(r->x, rr);
    p256_sub(r->x, r->x, j);
    p256_sub(r->x, r->x, v);
    p256_sub(r->x, r->x, v);
    p256_add(t, p->z, q->z);
    p256_sq(t, t);
    p256_sub(t, t, z1z1);
    p256_sub(t, t, z2z2);
    p256_mul(r->z, t, h);
    p256_sub(t, v, r->x);
    p256_mul(t, rr, t);
    p256_mul(s1, s1, j);
    p256_add(s1, s1, s1);
    p256_sub(r->y, t, s1);
}

static int p256_to_affine(const p256_jac_t *p, uint32_t *x, uint32_t *y) {
    uint32_t zi[8];
    uint32_t zi2[8];
    uint32_t zi3[8];

    if (p256_is_zero(p->z))
        return -1;
    p256_inv(zi, p->z);
    p256_sq(zi2, zi);
    p256_mul(zi3, zi2, zi);
    p256_mul(x, p->x, zi2);
    p256_mul(y, p->y, zi3);
    return 0;
}

static int p256_on_curve(const uint32_t *x, const uint32_t *y) {
    uint32_t lhs[8];
    uint32_t rhs[8];
    uint32_t t[8];
    uint32_t ax[8];
    static const uint32_t three[8] = { 3, 0, 0, 0, 0, 0, 0, 0 };

    if (p256_cmp(x, p256_p) >= 0 || p256_cmp(y, p256_p) >= 0)
        return 0;
    p256_sq(lhs, y);
    p256_sq(t, x);
    p256_mul(rhs, t, x);
    p256_mul(ax, three, x);
    p256_sub(rhs, rhs, ax);
    p256_add(rhs, rhs, p256_b);
    return p256_cmp(lhs, rhs) == 0;
}

static void p256_from_bin(uint32_t r[8], const uint8_t bin[32]) {
    int i;

    for (i = 0; i < 8; i++) {
        r[i] = ((uint32_t)bin[28 - i * 4] << 24) |
               ((uint32_t)bin[29 - i * 4] << 16) |
               ((uint32_t)bin[30 - i * 4] << 8) |
               bin[31 - i * 4];
    }
}

static void p256_to_bin(uint8_t out[32], const uint32_t r[8]) {
    int i;

    for (i = 0; i < 8; i++) {
        out[28 - i * 4] = (uint8_t)(r[i] >> 24);
        out[29 - i * 4] = (uint8_t)(r[i] >> 16);
        out[30 - i * 4] = (uint8_t)(r[i] >> 8);
        out[31 - i * 4] = (uint8_t)r[i];
    }
}

static int p256_n_mul(uint32_t r[8], const uint32_t *a, const uint32_t *b,
                      uint32_t *wa, uint32_t *wm) {
    uint32_t t[16];
    int i;

    for (i = 0; i < 16; i++)
        t[i] = 0;
    {
        int j;

        for (i = 0; i < 8; i++) {
            uint64_t acc = 0;

            for (j = 0; j < 8; j++) {
                acc += (uint64_t)t[i + j] + (uint64_t)a[i] * (uint64_t)b[j];
                t[i + j] = (uint32_t)acc;
                acc >>= 32;
            }
            t[i + 8] = (uint32_t)acc;
        }
    }
    return bn_mod(r, t, p256_n, 8, wa, wm);
}

static int p256_n_inv(uint32_t r[8], const uint32_t *a, uint32_t *wa,
                      uint32_t *wm) {
    static const uint8_t exp[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
        0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x4F
    };
    uint32_t base[8];
    uint32_t acc[8];
    uint32_t t[8];
    size_t i;
    int b;
    int bit;

    memcpy(base, a, sizeof(base));
    memset(acc, 0, sizeof(acc));
    acc[0] = 1;
    for (i = 0; i < 32; i++) {
        for (b = 7; b >= 0; b--) {
            bit = (exp[i] >> b) & 1;
            if (p256_n_mul(t, acc, acc, wa, wm) < 0)
                return -1;
            memcpy(acc, t, sizeof(acc));
            if (bit) {
                if (p256_n_mul(t, acc, base, wa, wm) < 0)
                    return -1;
                memcpy(acc, t, sizeof(acc));
            }
        }
    }
    memcpy(r, acc, sizeof(acc));
    return 0;
}

static int p256_n_cmp(const uint32_t *a, const uint32_t *b) {
    return bn_cmp(a, b, 8);
}

int lebtls_ecdsa_verify(const uint8_t qx[32], const uint8_t qy[32],
                        const uint8_t *hash, size_t hash_len,
                        const uint8_t r_bin[32], const uint8_t s_bin[32]) {
    uint32_t qx_f[8];
    uint32_t qy_f[8];
    uint32_t r[8];
    uint32_t s[8];
    uint32_t e[8];
    uint32_t w[8];
    uint32_t u1[8];
    uint32_t u2[8];
    uint32_t *wa;
    uint32_t *wm;
    p256_jac_t p1;
    p256_jac_t p2;
    p256_jac_t acc;
    uint32_t rx[8];
    uint32_t ry[8];
    uint8_t ebytes[32];
    int rc;

    if (!qx || !qy || !hash || !r_bin || !s_bin)
        return -1;
    if (hash_len == 0)
        return -1;
    wa = kmalloc(16 * sizeof(uint32_t));
    wm = kmalloc(16 * sizeof(uint32_t));
    if (!wa || !wm) {
        if (wa)
            kfree(wa);
        if (wm)
            kfree(wm);
        return -1;
    }
    rc = -1;
    p256_from_bin(qx_f, qx);
    p256_from_bin(qy_f, qy);
    p256_from_bin(r, r_bin);
    p256_from_bin(s, s_bin);
    if (p256_is_zero(r) || p256_is_zero(s))
        goto out;
    if (p256_n_cmp(r, p256_n) >= 0 || p256_n_cmp(s, p256_n) >= 0)
        goto out;
    if (!p256_on_curve(qx_f, qy_f))
        goto out;
    if (hash_len > 32)
        hash_len = 32;
    memset(ebytes, 0, sizeof(ebytes));
    memcpy(ebytes + (32 - hash_len), hash, hash_len);
    p256_from_bin(e, ebytes);
    if (p256_n_inv(w, s, wa, wm) < 0)
        goto out;
    if (p256_n_mul(u1, e, w, wa, wm) < 0)
        goto out;
    if (p256_n_mul(u2, r, w, wa, wm) < 0)
        goto out;
    {
        uint8_t u1b[32];
        uint8_t u2b[32];
        p256_jac_t g;

        p256_to_bin(u1b, u1);
        p256_to_bin(u2b, u2);
        memcpy(g.x, p256_gx, sizeof(g.x));
        memcpy(g.y, p256_gy, sizeof(g.y));
        memset(g.z, 0, sizeof(g.z));
        g.z[0] = 1;
        {
            p256_jac_t t1;
            p256_jac_t t2;
            p256_jac_t q;
            int i;
            int b1;
            int b2;

            memcpy(q.x, qx_f, sizeof(q.x));
            memcpy(q.y, qy_f, sizeof(q.y));
            memset(q.z, 0, sizeof(q.z));
            q.z[0] = 1;
            memset(&acc, 0, sizeof(acc));
            for (i = 0; i < 256; i++) {
                p256_point_double(&acc, &acc);
                b1 = (u1b[i / 8] >> (7 - (i % 8))) & 1;
                b2 = (u2b[i / 8] >> (7 - (i % 8))) & 1;
                if (b1) {
                    p256_point_add(&t1, &acc, &g);
                    memcpy(&acc, &t1, sizeof(acc));
                }
                if (b2) {
                    p256_point_add(&t2, &acc, &q);
                    memcpy(&acc, &t2, sizeof(acc));
                }
            }
            memset(&p1, 0, sizeof(p1));
            memset(&p2, 0, sizeof(p2));
            memset(u1b, 0, sizeof(u1b));
            memset(u2b, 0, sizeof(u2b));
        }
    }
    if (p256_is_zero(acc.z))
        goto out;
    if (p256_to_affine(&acc, rx, ry) < 0)
        goto out;
    {
        uint32_t rv[8];
        uint32_t tmp[8];

        memcpy(rv, rx, sizeof(rv));
        if (p256_n_cmp(rv, p256_n) >= 0) {
            memcpy(tmp, rv, sizeof(tmp));
            p256_sub_raw(tmp, tmp, p256_n);
            memcpy(rv, tmp, sizeof(rv));
            memset(tmp, 0, sizeof(tmp));
        }
        if (p256_n_cmp(rv, r) != 0)
            goto out;
    }
    rc = 0;

out:
    memset(qx_f, 0, sizeof(qx_f));
    memset(qy_f, 0, sizeof(qy_f));
    memset(r, 0, sizeof(r));
    memset(s, 0, sizeof(s));
    memset(e, 0, sizeof(e));
    memset(w, 0, sizeof(w));
    memset(u1, 0, sizeof(u1));
    memset(u2, 0, sizeof(u2));
    memset(&acc, 0, sizeof(acc));
    memset(rx, 0, sizeof(rx));
    memset(ry, 0, sizeof(ry));
    kfree(wa);
    kfree(wm);
    return rc;
}
int lebtls_p256_selftest(void) {
    static const uint8_t qx[32] = {
        0xc0, 0x7f, 0x7b, 0x24, 0xe3, 0x39, 0x50, 0x1a,
        0x68, 0x9d, 0x85, 0x64, 0xb6, 0x90, 0xaa, 0x02,
        0xdd, 0x5c, 0xae, 0x20, 0x5b, 0x09, 0x0c, 0xe4,
        0x70, 0x5c, 0x05, 0x27, 0x9e, 0x2b, 0xe0, 0x72
    };
    static const uint8_t qy[32] = {
        0xa0, 0xd5, 0x30, 0x49, 0x3e, 0x0d, 0xf5, 0x42,
        0x18, 0x9b, 0x27, 0x31, 0x0f, 0x3d, 0x27, 0xfa,
        0xdc, 0x8c, 0xfa, 0xb9, 0xb7, 0xea, 0x26, 0x1d,
        0x07, 0x31, 0x9f, 0xe3, 0xc4, 0x42, 0x2d, 0xaa
    };
    static const uint8_t rr[32] = {
        0x23, 0x44, 0x2c, 0x23, 0xe8, 0xa0, 0x94, 0xd7,
        0xd4, 0x43, 0xac, 0x42, 0x4b, 0xec, 0xc7, 0xb9,
        0xaf, 0x43, 0xcc, 0x38, 0xfb, 0x6f, 0x0f, 0x77,
        0x30, 0x01, 0x0c, 0x7a, 0xa8, 0x20, 0x79, 0x91
    };
    static const uint8_t ss[32] = {
        0x2d, 0x1f, 0xdc, 0xb1, 0x2c, 0x25, 0xc3, 0xbb,
        0x48, 0x7d, 0xf8, 0xb2, 0xd7, 0x1c, 0xdf, 0x52,
        0x06, 0xdb, 0xa8, 0x73, 0x19, 0x9c, 0x78, 0x9e,
        0xa3, 0x85, 0x7a, 0xf8, 0x1f, 0x69, 0x32, 0x2b
    };
    static const uint8_t mm[27] = {
        0x65, 0x63, 0x64, 0x73, 0x61, 0x20, 0x74, 0x65,
        0x73, 0x74, 0x20, 0x6d, 0x65, 0x73, 0x73, 0x61,
        0x67, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x70,
        0x32, 0x35, 0x36
    };
    uint8_t h[32];
    uint8_t bad[32];

    sha256_hash(mm, sizeof(mm), h);
    if (lebtls_ecdsa_verify(qx, qy, h, sizeof(h), rr, ss) < 0)
        return -1;
    memcpy(bad, ss, 32);
    bad[0] ^= 1;
    if (lebtls_ecdsa_verify(qx, qy, h, sizeof(h), rr, bad) == 0)
        return -1;
    memset(bad, 0, sizeof(bad));
    memset(h, 0, sizeof(h));
    return 0;
}

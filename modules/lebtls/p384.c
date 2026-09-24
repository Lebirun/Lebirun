#include <lebirun/mem_map.h>
#include <lebirun/crypto.h>
#include <string.h>

#include "tls_int.h"

#define P384_LIMBS 12

static const uint32_t p384_p[P384_LIMBS] = {
    0xFFFFFFFFUL, 0x00000000UL, 0x00000000UL, 0xFFFFFFFFUL,
    0xFFFFFFFEUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL,
    0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL
};
static const uint32_t p384_b[P384_LIMBS] = {
    0xD3EC2AEFUL, 0x2A85C8EDUL, 0x8A2ED19DUL, 0xC656398DUL,
    0x5013875AUL, 0x0314088FUL, 0xFE814112UL, 0x181D9C6EUL,
    0xE3F82D19UL, 0x988E056BUL, 0xE23EE7E4UL, 0xB3312FA7UL
};
static const uint32_t p384_n[P384_LIMBS] = {
    0xCCC52973UL, 0xECEC196AUL, 0x48B0A77AUL, 0x581A0DB2UL,
    0xF4372DDFUL, 0xC7634D81UL, 0xFFFFFFFFUL, 0xFFFFFFFFUL,
    0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL
};
static const uint32_t p384_gx[P384_LIMBS] = {
    0x72760AB7UL, 0x3A545E38UL, 0xBF55296CUL, 0x5502F25DUL,
    0x82542A38UL, 0x59F741E0UL, 0x8BA79B98UL, 0x6E1D3B62UL,
    0xF320AD74UL, 0x8EB1C71EUL, 0xBE8B0537UL, 0xAA87CA22UL
};
static const uint32_t p384_gy[P384_LIMBS] = {
    0x90EA0E5FUL, 0x7A431D7CUL, 0x1D7E819DUL, 0x0A60B1CEUL,
    0xB5F0B8C0UL, 0xE9DA3113UL, 0x289A147CUL, 0xF8F41DBDUL,
    0x9292DC29UL, 0x5D9E98BFUL, 0x96262C6FUL, 0x3617DE4AUL
};

typedef struct {
    uint32_t x[P384_LIMBS];
    uint32_t y[P384_LIMBS];
    uint32_t z[P384_LIMBS];
} p384_jac_t;

static int p384_cmp(const uint32_t *a, const uint32_t *b) {
    int i;

    for (i = 11; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static int p384_is_zero(const uint32_t *a) {
    int i;

    for (i = 0; i < 12; i++) {
        if (a[i] != 0)
            return 0;
    }
    return 1;
}

static void p384_sub_raw(uint32_t r[P384_LIMBS], const uint32_t *a,
                         const uint32_t *b) {
    uint64_t borrow;
    int i;

    borrow = 0;
    for (i = 0; i < 12; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
}

static void p384_add(uint32_t r[P384_LIMBS], const uint32_t *a,
                     const uint32_t *b) {
    uint32_t t[13];
    uint64_t c;
    uint64_t borrow;
    int i;

    c = 0;
    for (i = 0; i < 12; i++) {
        c += (uint64_t)a[i] + b[i];
        t[i] = (uint32_t)c;
        c >>= 32;
    }
    t[12] = (uint32_t)c;
    if (t[12] != 0 || p384_cmp(t, p384_p) >= 0) {
        borrow = 0;
        for (i = 0; i < 12; i++) {
            uint64_t v = (uint64_t)t[i] - p384_p[i] - borrow;
            t[i] = (uint32_t)v;
            borrow = (v >> 63) & 1;
        }
        t[12] -= (uint32_t)borrow;
    }
    memcpy(r, t, 12 * sizeof(uint32_t));
}

static void p384_sub(uint32_t r[P384_LIMBS], const uint32_t *a,
                     const uint32_t *b) {
    if (p384_cmp(a, b) < 0) {
        uint32_t t[P384_LIMBS];

        p384_sub_raw(t, b, a);
        p384_sub_raw(r, p384_p, t);
    } else {
        p384_sub_raw(r, a, b);
    }
}

static void p384_carry_fold(__int128 acc[24]) {
    int i;

    for (i = 0; i < 23; i++) {
        __int128 c = acc[i] >> 32;
        acc[i] -= c << 32;
        acc[i + 1] += c;
    }
}

static int p384_mul(uint32_t r[P384_LIMBS], const uint32_t *a,
                    const uint32_t *b, uint32_t *wa, uint32_t *wm) {
    __int128 acc[24];
    int i;
    int j;
    int iter;

    (void)wa;
    (void)wm;
    for (i = 0; i < 24; i++)
        acc[i] = 0;
    for (i = 0; i < 12; i++) {
        for (j = 0; j < 12; j++)
            acc[i + j] += (__int128)(uint64_t)a[i] * (uint64_t)b[j];
    }
    for (iter = 0; iter < 32; iter++) {
        __int128 hi[12];
        int empty;

        p384_carry_fold(acc);
        empty = 1;
        for (i = 0; i < 12; i++) {
            hi[i] = acc[i + 12];
            acc[i + 12] = 0;
            if (hi[i] != 0)
                empty = 0;
        }
        if (empty)
            break;
        for (i = 0; i < 12; i++) {
            acc[i] += hi[i];
            acc[i + 1] -= hi[i];
            acc[i + 3] += hi[i];
            acc[i + 4] += hi[i];
        }
    }
    for (i = 0; i < 12; i++)
        r[i] = (uint32_t)acc[i];
    for (i = 0; i < 12; i++) {
        if (p384_cmp(r, p384_p) < 0)
            break;
        p384_sub_raw(r, r, p384_p);
    }
    return 0;
}

static int p384_sq(uint32_t r[P384_LIMBS], const uint32_t *a, uint32_t *wa,
                   uint32_t *wm) {
    return p384_mul(r, a, a, wa, wm);
}

static void p384_from_bin(uint32_t r[12], const uint8_t bin[48]) {
    int i;

    for (i = 0; i < 12; i++) {
        r[i] = ((uint32_t)bin[44 - i * 4] << 24) |
               ((uint32_t)bin[45 - i * 4] << 16) |
               ((uint32_t)bin[46 - i * 4] << 8) |
               bin[47 - i * 4];
    }
}

static void p384_to_bin(uint8_t out[48], const uint32_t r[12]) {
    int i;

    for (i = 0; i < 12; i++) {
        out[44 - i * 4] = (uint8_t)(r[i] >> 24);
        out[45 - i * 4] = (uint8_t)(r[i] >> 16);
        out[46 - i * 4] = (uint8_t)(r[i] >> 8);
        out[47 - i * 4] = (uint8_t)r[i];
    }
}

static int p384_pow(uint32_t out[P384_LIMBS], const uint32_t *a,
                    const uint8_t *exp, size_t exp_len, uint32_t *wa,
                    uint32_t *wm) {
    uint32_t base[P384_LIMBS];
    uint32_t acc[P384_LIMBS];
    size_t i;
    int b;
    int bit;

    memcpy(base, a, sizeof(base));
    memset(acc, 0, sizeof(acc));
    acc[0] = 1;
    for (i = 0; i < exp_len; i++) {
        for (b = 7; b >= 0; b--) {
            uint32_t t[P384_LIMBS];

            bit = (exp[i] >> b) & 1;
            if (p384_sq(acc, acc, wa, wm) < 0)
                return -1;
            if (bit) {
                if (p384_mul(t, acc, base, wa, wm) < 0)
                    return -1;
                memcpy(acc, t, sizeof(acc));
            }
        }
    }
    memcpy(out, acc, sizeof(acc));
    return 0;
}

static int p384_inv(uint32_t out[P384_LIMBS], const uint32_t *a,
                    uint32_t *wa, uint32_t *wm) {
    static const uint8_t exp[48] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFD
    };

    return p384_pow(out, a, exp, sizeof(exp), wa, wm);
}

static int p384_point_double(p384_jac_t *r, const p384_jac_t *p,
                             uint32_t *wa, uint32_t *wm) {
    uint32_t xx[12];
    uint32_t yy[12];
    uint32_t yyyy[12];
    uint32_t zz[12];
    uint32_t s[12];
    uint32_t m[12];
    uint32_t t[12];
    uint32_t zzzz[12];
    uint32_t three[12];

    if (p384_is_zero(p->z)) {
        memset(r, 0, sizeof(*r));
        return 0;
    }
    memset(three, 0, sizeof(three));
    three[0] = 3;
    if (p384_sq(xx, p->x, wa, wm) < 0)
        return -1;
    if (p384_sq(yy, p->y, wa, wm) < 0)
        return -1;
    if (p384_sq(yyyy, yy, wa, wm) < 0)
        return -1;
    if (p384_sq(zz, p->z, wa, wm) < 0)
        return -1;
    if (p384_sq(zzzz, zz, wa, wm) < 0)
        return -1;
    p384_add(s, p->x, yy);
    if (p384_sq(s, s, wa, wm) < 0)
        return -1;
    p384_sub(s, s, xx);
    p384_sub(s, s, yyyy);
    p384_add(s, s, s);
    if (p384_mul(m, three, xx, wa, wm) < 0)
        return -1;
    if (p384_mul(t, three, zzzz, wa, wm) < 0)
        return -1;
    p384_sub(m, m, t);
    if (p384_sq(t, m, wa, wm) < 0)
        return -1;
    p384_sub(t, t, s);
    p384_sub(t, t, s);
    memcpy(r->x, t, sizeof(r->x));
    p384_add(t, p->y, p->z);
    if (p384_sq(t, t, wa, wm) < 0)
        return -1;
    p384_sub(t, t, yy);
    p384_sub(t, t, zz);
    memcpy(r->z, t, sizeof(r->z));
    p384_sub(t, s, r->x);
    if (p384_mul(t, m, t, wa, wm) < 0)
        return -1;
    p384_add(yyyy, yyyy, yyyy);
    p384_add(yyyy, yyyy, yyyy);
    p384_add(yyyy, yyyy, yyyy);
    p384_sub(r->y, t, yyyy);
    return 0;
}

static int p384_point_add(p384_jac_t *r, const p384_jac_t *p,
                          const p384_jac_t *q, uint32_t *wa, uint32_t *wm) {
    uint32_t z1z1[12];
    uint32_t z2z2[12];
    uint32_t u1[12];
    uint32_t u2[12];
    uint32_t s1[12];
    uint32_t s2[12];
    uint32_t h[12];
    uint32_t i[12];
    uint32_t j[12];
    uint32_t rr[12];
    uint32_t v[12];
    uint32_t t[12];

    if (p384_is_zero(p->z)) {
        memcpy(r, q, sizeof(*r));
        return 0;
    }
    if (p384_is_zero(q->z)) {
        memcpy(r, p, sizeof(*r));
        return 0;
    }
    if (p384_sq(z1z1, p->z, wa, wm) < 0)
        return -1;
    if (p384_sq(z2z2, q->z, wa, wm) < 0)
        return -1;
    if (p384_mul(u1, p->x, z2z2, wa, wm) < 0)
        return -1;
    if (p384_mul(u2, q->x, z1z1, wa, wm) < 0)
        return -1;
    if (p384_mul(t, q->z, z2z2, wa, wm) < 0)
        return -1;
    if (p384_mul(s1, p->y, t, wa, wm) < 0)
        return -1;
    if (p384_mul(t, p->z, z1z1, wa, wm) < 0)
        return -1;
    if (p384_mul(s2, q->y, t, wa, wm) < 0)
        return -1;
    if (p384_cmp(u1, u2) == 0) {
        if (p384_cmp(s1, s2) != 0) {
            memset(r, 0, sizeof(*r));
            return 0;
        }
        return p384_point_double(r, p, wa, wm);
    }
    p384_sub(h, u2, u1);
    p384_add(i, h, h);
    if (p384_sq(i, i, wa, wm) < 0)
        return -1;
    if (p384_mul(j, h, i, wa, wm) < 0)
        return -1;
    p384_sub(rr, s2, s1);
    p384_add(rr, rr, rr);
    if (p384_mul(v, u1, i, wa, wm) < 0)
        return -1;
    if (p384_sq(r->x, rr, wa, wm) < 0)
        return -1;
    p384_sub(r->x, r->x, j);
    p384_sub(r->x, r->x, v);
    p384_sub(r->x, r->x, v);
    p384_add(t, p->z, q->z);
    if (p384_sq(t, t, wa, wm) < 0)
        return -1;
    p384_sub(t, t, z1z1);
    p384_sub(t, t, z2z2);
    if (p384_mul(r->z, t, h, wa, wm) < 0)
        return -1;
    p384_sub(t, v, r->x);
    if (p384_mul(t, rr, t, wa, wm) < 0)
        return -1;
    if (p384_mul(s1, s1, j, wa, wm) < 0)
        return -1;
    p384_add(s1, s1, s1);
    p384_sub(r->y, t, s1);
    return 0;
}

static int p384_to_affine(const p384_jac_t *p, uint32_t *x, uint32_t *y,
                          uint32_t *wa, uint32_t *wm) {
    uint32_t zi[12];
    uint32_t zi2[12];
    uint32_t zi3[12];

    if (p384_is_zero(p->z))
        return -1;
    if (p384_inv(zi, p->z, wa, wm) < 0)
        return -1;
    if (p384_mul(zi2, zi, zi, wa, wm) < 0)
        return -1;
    if (p384_mul(zi3, zi2, zi, wa, wm) < 0)
        return -1;
    if (p384_mul(x, p->x, zi2, wa, wm) < 0)
        return -1;
    if (p384_mul(y, p->y, zi3, wa, wm) < 0)
        return -1;
    return 0;
}

static int p384_on_curve(const uint32_t *x, const uint32_t *y, uint32_t *wa,
                         uint32_t *wm) {
    uint32_t lhs[12];
    uint32_t rhs[12];
    uint32_t t[12];
    uint32_t ax[12];
    static const uint32_t three[12] = { 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (p384_cmp(x, p384_p) >= 0 || p384_cmp(y, p384_p) >= 0)
        return 0;
    if (p384_sq(lhs, y, wa, wm) < 0)
        return 0;
    if (p384_sq(t, x, wa, wm) < 0)
        return 0;
    if (p384_mul(rhs, t, x, wa, wm) < 0)
        return 0;
    if (p384_mul(ax, three, x, wa, wm) < 0)
        return 0;
    p384_sub(rhs, rhs, ax);
    p384_add(rhs, rhs, p384_b);
    return p384_cmp(lhs, rhs) == 0;
}

static int p384_n_mul(uint32_t r[12], const uint32_t *a, const uint32_t *b,
                      uint32_t *wa, uint32_t *wm) {
    uint32_t t[24];
    int i;
    int j;

    for (i = 0; i < 24; i++)
        t[i] = 0;
    for (i = 0; i < 12; i++) {
        uint64_t acc = 0;

        for (j = 0; j < 12; j++) {
            acc += (uint64_t)t[i + j] + (uint64_t)a[i] * (uint64_t)b[j];
            t[i + j] = (uint32_t)acc;
            acc >>= 32;
        }
        t[i + 12] = (uint32_t)((uint64_t)t[i + 12] + acc);
    }
    return bn_mod(r, t, p384_n, 12, wa, wm);
}

static int p384_n_inv(uint32_t r[12], const uint32_t *a, uint32_t *wa,
                      uint32_t *wm) {
    static const uint8_t exp[48] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xC7, 0x63, 0x4D, 0x81, 0xF4, 0x37, 0x2D, 0xDF,
        0x58, 0x1A, 0x0D, 0xB2, 0x48, 0xB0, 0xA7, 0x7A,
        0xEC, 0xEC, 0x19, 0x6A, 0xCC, 0xC5, 0x29, 0x71
    };
    uint32_t base[12];
    uint32_t acc[12];
    uint32_t t[12];
    size_t i;
    int b;
    int bit;

    memcpy(base, a, sizeof(base));
    memset(acc, 0, sizeof(acc));
    acc[0] = 1;
    for (i = 0; i < 48; i++) {
        for (b = 7; b >= 0; b--) {
            bit = (exp[i] >> b) & 1;
            if (p384_n_mul(t, acc, acc, wa, wm) < 0)
                return -1;
            memcpy(acc, t, sizeof(acc));
            if (bit) {
                if (p384_n_mul(t, acc, base, wa, wm) < 0)
                    return -1;
                memcpy(acc, t, sizeof(acc));
            }
        }
    }
    memcpy(r, acc, sizeof(acc));
    return 0;
}

static int p384_n_cmp(const uint32_t *a, const uint32_t *b) {
    return bn_cmp(a, b, 12);
}

int lebtls_ecdsa384_verify(const uint8_t qx[48], const uint8_t qy[48],
                           const uint8_t *hash, size_t hash_len,
                           const uint8_t r_bin[48], const uint8_t s_bin[48]) {
    uint32_t qx_f[12];
    uint32_t qy_f[12];
    uint32_t r[12];
    uint32_t s[12];
    uint32_t e[12];
    uint32_t w[12];
    uint32_t u1[12];
    uint32_t u2[12];
    uint32_t *wa;
    uint32_t *wm;
    p384_jac_t p1;
    p384_jac_t p2;
    p384_jac_t acc;
    uint32_t rx[12];
    uint32_t ry[12];
    uint8_t ebytes[48];
    int rc;

    if (!qx || !qy || !hash || !r_bin || !s_bin)
        return -1;
    if (hash_len == 0)
        return -1;
    wa = kmalloc(24 * sizeof(uint32_t));
    wm = kmalloc(24 * sizeof(uint32_t));
    if (!wa || !wm) {
        if (wa)
            kfree(wa);
        if (wm)
            kfree(wm);
        return -1;
    }
    rc = -1;
    p384_from_bin(qx_f, qx);
    p384_from_bin(qy_f, qy);
    p384_from_bin(r, r_bin);
    p384_from_bin(s, s_bin);
    if (p384_is_zero(r) || p384_is_zero(s))
        goto out;
    if (p384_n_cmp(r, p384_n) >= 0 || p384_n_cmp(s, p384_n) >= 0)
        goto out;
    if (!p384_on_curve(qx_f, qy_f, wa, wm))
        goto out;
    if (hash_len > 48)
        hash_len = 48;
    memset(ebytes, 0, sizeof(ebytes));
    memcpy(ebytes + (48 - hash_len), hash, hash_len);
    p384_from_bin(e, ebytes);
    if (p384_n_inv(w, s, wa, wm) < 0)
        goto out;
    if (p384_n_mul(u1, e, w, wa, wm) < 0)
        goto out;
    if (p384_n_mul(u2, r, w, wa, wm) < 0)
        goto out;
    {
        uint8_t u1b[48];
        uint8_t u2b[48];
        p384_jac_t g;

        p384_to_bin(u1b, u1);
        p384_to_bin(u2b, u2);
        memcpy(g.x, p384_gx, sizeof(g.x));
        memcpy(g.y, p384_gy, sizeof(g.y));
        memset(g.z, 0, sizeof(g.z));
        g.z[0] = 1;
        {
            p384_jac_t t1;
            p384_jac_t t2;
            p384_jac_t q;
            int i;
            int b1;
            int b2;

            memcpy(q.x, qx_f, sizeof(q.x));
            memcpy(q.y, qy_f, sizeof(q.y));
            memset(q.z, 0, sizeof(q.z));
            q.z[0] = 1;
            memset(&acc, 0, sizeof(acc));
            for (i = 0; i < 384; i++) {
                if (p384_point_double(&acc, &acc, wa, wm) < 0)
                    goto out;
                b1 = (u1b[i / 8] >> (7 - (i % 8))) & 1;
                b2 = (u2b[i / 8] >> (7 - (i % 8))) & 1;
                if (b1) {
                    if (p384_point_add(&t1, &acc, &g, wa, wm) < 0)
                        goto out;
                    memcpy(&acc, &t1, sizeof(acc));
                }
                if (b2) {
                    if (p384_point_add(&t2, &acc, &q, wa, wm) < 0)
                        goto out;
                    memcpy(&acc, &t2, sizeof(acc));
                }
            }
            memset(&p1, 0, sizeof(p1));
            memset(&p2, 0, sizeof(p2));
            memset(u1b, 0, sizeof(u1b));
            memset(u2b, 0, sizeof(u2b));
        }
    }
    if (p384_is_zero(acc.z))
        goto out;
    if (p384_to_affine(&acc, rx, ry, wa, wm) < 0)
        goto out;
    {
        uint32_t rv[12];
        uint32_t tmp[12];

        memcpy(rv, rx, sizeof(rv));
        if (p384_n_cmp(rv, p384_n) >= 0) {
            memcpy(tmp, rv, sizeof(tmp));
            p384_sub_raw(tmp, tmp, p384_n);
            memcpy(rv, tmp, sizeof(rv));
            memset(tmp, 0, sizeof(tmp));
        }
        if (p384_n_cmp(rv, r) != 0)
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
    memset(ebytes, 0, sizeof(ebytes));
    kfree(wa);
    kfree(wm);
    return rc;
}
int lebtls_p384_selftest(void) {
    static const uint8_t qx[48] = {
        0x44, 0x37, 0xfa, 0x78, 0xde, 0x7f, 0xd1, 0x0d,
        0x09, 0x16, 0xbc, 0x2f, 0xef, 0xa9, 0xc2, 0xb7,
        0x37, 0xa4, 0x05, 0xa7, 0x11, 0xf3, 0x4f, 0x91,
        0x25, 0xa9, 0x28, 0x6f, 0x5a, 0xd4, 0xa4, 0xcd,
        0xd5, 0x21, 0x89, 0x4f, 0x82, 0xaa, 0x1a, 0x65,
        0x48, 0x05, 0x04, 0x81, 0x3a, 0x47, 0x9b, 0xcb
    };
    static const uint8_t qy[48] = {
        0xc3, 0x0e, 0x35, 0x0f, 0xc7, 0x34, 0x67, 0xff,
        0x9a, 0x07, 0xf6, 0x83, 0x7d, 0xee, 0xb1, 0xfd,
        0x8a, 0xb9, 0x7a, 0x27, 0x88, 0x08, 0x13, 0xfe,
        0x0f, 0xd6, 0x4c, 0x62, 0x8b, 0x9d, 0xfa, 0x91,
        0x3d, 0xeb, 0x36, 0x6d, 0x24, 0x4b, 0xbd, 0x1d,
        0xb7, 0xdc, 0xcb, 0x79, 0x4c, 0xb1, 0x6d, 0xb9
    };
    static const uint8_t rr[48] = {
        0xb5, 0x58, 0x8b, 0x42, 0x80, 0x3b, 0x53, 0x37,
        0xb8, 0xe4, 0xdb, 0x12, 0x0c, 0xab, 0x30, 0xc1,
        0x06, 0xd0, 0x52, 0x66, 0x16, 0xad, 0x2d, 0x73,
        0xd4, 0xb9, 0xe9, 0x07, 0x7a, 0x2f, 0x9b, 0x22,
        0x43, 0x9b, 0xbe, 0xe7, 0x35, 0x58, 0x92, 0x9a,
        0x98, 0x29, 0x1c, 0x14, 0x4e, 0x97, 0x45, 0x02
    };
    static const uint8_t ss[48] = {
        0xd6, 0x06, 0x13, 0x52, 0x10, 0xbf, 0x26, 0x1f,
        0x66, 0xcf, 0x77, 0x45, 0xe9, 0xaf, 0xef, 0x54,
        0x48, 0xd7, 0x11, 0x8a, 0x21, 0xe0, 0x2f, 0x8d,
        0x9f, 0x45, 0x61, 0xa3, 0xab, 0x61, 0xe1, 0xe0,
        0x2e, 0x0d, 0xb3, 0x4e, 0x79, 0x21, 0x30, 0x87,
        0x49, 0x7a, 0x47, 0x1b, 0xcd, 0xca, 0x08, 0xe7
    };
    static const uint8_t mm[27] = {
        0x65, 0x63, 0x64, 0x73, 0x61, 0x20, 0x74, 0x65,
        0x73, 0x74, 0x20, 0x6d, 0x65, 0x73, 0x73, 0x61,
        0x67, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x70,
        0x33, 0x38, 0x34
    };
    uint8_t h[48];
    uint8_t bad[48];

    sha384_hash(mm, sizeof(mm), h);
    if (lebtls_ecdsa384_verify(qx, qy, h, sizeof(h), rr, ss) < 0)
        return -1;
    memcpy(bad, ss, 48);
    bad[0] ^= 1;
    if (lebtls_ecdsa384_verify(qx, qy, h, sizeof(h), rr, bad) == 0)
        return -1;
    memset(bad, 0, sizeof(bad));
    memset(h, 0, sizeof(h));
    return 0;
}


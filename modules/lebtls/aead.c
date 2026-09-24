#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <lebirun/crypto.h>
#include "tls_int.h"

static uint8_t gcm_xtime(uint8_t v) {
    return (uint8_t)((v << 1) ^ (v & 0x80 ? 0x1B : 0x00));
}

static uint8_t gcm_gfmul(uint8_t a, uint8_t b) {
    uint8_t r;
    int i;

    r = 0;
    for (i = 0; i < 8; i++) {
        if (b & 1)
            r ^= a;
        a = gcm_xtime(a);
        b >>= 1;
    }
    return r;
}

static uint8_t gcm_sbox(uint8_t v) {
    uint8_t inv;
    uint8_t x;
    uint8_t r;
    int i;

    if (v == 0)
        inv = 0;
    else {
        inv = 1;
        x = v;
        for (i = 1; i < 255; i++)
            inv = gcm_gfmul(inv, x);
    }
    r = inv ^ 0x63;
    x = inv;
    for (i = 0; i < 4; i++) {
        x = (uint8_t)((x << 1) | (x >> 7));
        r ^= x;
    }
    return r;
}

static uint8_t gcm_sbox_tab[256];
static int gcm_sbox_ready;

static void gcm_sbox_init(void) {
    int i;

    if (gcm_sbox_ready)
        return;
    for (i = 0; i < 256; i++)
        gcm_sbox_tab[i] = gcm_sbox((uint8_t)i);
    gcm_sbox_ready = 1;
}

static void gcm_key_expand(const uint8_t key[16], uint8_t rk[11][16]) {
    uint8_t rcon;
    int i;
    int k;
    uint8_t t[4];

    gcm_sbox_init();
    memcpy(rk[0], key, 16);
    rcon = 1;
    for (i = 1; i < 11; i++) {
        t[0] = gcm_sbox_tab[rk[i - 1][13]];
        t[1] = gcm_sbox_tab[rk[i - 1][14]];
        t[2] = gcm_sbox_tab[rk[i - 1][15]];
        t[3] = gcm_sbox_tab[rk[i - 1][12]];
        t[0] ^= rcon;
        rcon = gcm_xtime(rcon);
        for (k = 0; k < 4; k++)
            rk[i][k] = rk[i - 1][k] ^ t[k];
        for (k = 4; k < 16; k++)
            rk[i][k] = rk[i - 1][k] ^ rk[i][k - 4];
    }
}

static void gcm_add_round_key(uint8_t s[16], const uint8_t rk[16]) {
    int i;

    for (i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

static void gcm_sub_bytes(uint8_t s[16]) {
    int i;

    gcm_sbox_init();
    for (i = 0; i < 16; i++)
        s[i] = gcm_sbox_tab[s[i]];
}

static void gcm_shift_rows(uint8_t s[16]) {
    uint8_t t[16];
    int r;
    int c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++)
            t[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
    }
    memcpy(s, t, 16);
}

static void gcm_mix_columns(uint8_t s[16]) {
    int c;
    uint8_t a0;
    uint8_t a1;
    uint8_t a2;
    uint8_t a3;

    for (c = 0; c < 4; c++) {
        a0 = s[4 * c];
        a1 = s[4 * c + 1];
        a2 = s[4 * c + 2];
        a3 = s[4 * c + 3];
        s[4 * c] = gcm_xtime(a0) ^ gcm_xtime(a1) ^ a1 ^ a2 ^ a3;
        s[4 * c + 1] = a0 ^ gcm_xtime(a1) ^ gcm_xtime(a2) ^ a2 ^ a3;
        s[4 * c + 2] = a0 ^ a1 ^ gcm_xtime(a2) ^ gcm_xtime(a3) ^ a3;
        s[4 * c + 3] = gcm_xtime(a0) ^ a0 ^ a1 ^ a2 ^ gcm_xtime(a3);
    }
}

static void gcm_block_encrypt(const uint8_t rk[11][16], const uint8_t in[16],
                              uint8_t out[16]) {
    uint8_t s[16];
    int r;

    memcpy(s, in, 16);
    gcm_add_round_key(s, rk[0]);
    for (r = 1; r < 10; r++) {
        gcm_sub_bytes(s);
        gcm_shift_rows(s);
        gcm_mix_columns(s);
        gcm_add_round_key(s, rk[r]);
    }
    gcm_sub_bytes(s);
    gcm_shift_rows(s);
    gcm_add_round_key(s, rk[10]);
    memcpy(out, s, 16);
}

static void gcm_xor_block(uint8_t out[16], const uint8_t a[16], const uint8_t b[16]) {
    int i;

    for (i = 0; i < 16; i++)
        out[i] = a[i] ^ b[i];
}

static void gcm_gf_shift(uint8_t v[16]) {
    int i;
    int carry;
    int next;

    carry = 0;
    for (i = 0; i < 16; i++) {
        next = (v[i] & 1) ? 1 : 0;
        v[i] = (uint8_t)((v[i] >> 1) | (carry << 7));
        carry = next;
    }
}

static void gcm_gf_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16];
    uint8_t v[16];
    int i;
    int j;

    memset(z, 0, 16);
    memcpy(v, y, 16);
    for (i = 0; i < 16; i++) {
        for (j = 7; j >= 0; j--) {
            if ((x[i] >> j) & 1)
                gcm_xor_block(z, z, v);
            if (v[15] & 1) {
                gcm_gf_shift(v);
                v[0] ^= 0xE1;
            } else {
                gcm_gf_shift(v);
            }
        }
    }
    memcpy(out, z, 16);
}

static void gcm_ghash(const uint8_t h[16], const uint8_t *aad, size_t aad_len,
                      const uint8_t *data, size_t data_len, uint8_t out[16]) {
    uint8_t y[16];
    uint8_t blk[16];
    size_t off;
    size_t chunk;
    uint64_t a_bits;
    uint64_t d_bits;

    memset(y, 0, 16);
    off = 0;
    while (off < aad_len) {
        chunk = aad_len - off;
        if (chunk > 16)
            chunk = 16;
        memset(blk, 0, 16);
        memcpy(blk, aad + off, chunk);
        gcm_xor_block(y, y, blk);
        gcm_gf_mul(y, h, y);
        off += chunk;
    }
    off = 0;
    while (off < data_len) {
        chunk = data_len - off;
        if (chunk > 16)
            chunk = 16;
        memset(blk, 0, 16);
        memcpy(blk, data + off, chunk);
        gcm_xor_block(y, y, blk);
        gcm_gf_mul(y, h, y);
        off += chunk;
    }
    a_bits = (uint64_t)aad_len * 8;
    d_bits = (uint64_t)data_len * 8;
    memset(blk, 0, 16);
    blk[0] = (uint8_t)(a_bits >> 56);
    blk[1] = (uint8_t)(a_bits >> 48);
    blk[2] = (uint8_t)(a_bits >> 40);
    blk[3] = (uint8_t)(a_bits >> 32);
    blk[4] = (uint8_t)(a_bits >> 24);
    blk[5] = (uint8_t)(a_bits >> 16);
    blk[6] = (uint8_t)(a_bits >> 8);
    blk[7] = (uint8_t)a_bits;
    blk[8] = (uint8_t)(d_bits >> 56);
    blk[9] = (uint8_t)(d_bits >> 48);
    blk[10] = (uint8_t)(d_bits >> 40);
    blk[11] = (uint8_t)(d_bits >> 32);
    blk[12] = (uint8_t)(d_bits >> 24);
    blk[13] = (uint8_t)(d_bits >> 16);
    blk[14] = (uint8_t)(d_bits >> 8);
    blk[15] = (uint8_t)d_bits;
    gcm_xor_block(y, y, blk);
    gcm_gf_mul(y, h, y);
    memcpy(out, y, 16);
}

static void gcm_ctr(const uint8_t rk[11][16], const uint8_t nonce[12],
                    const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t ctr[16];
    uint8_t stream[16];
    uint32_t c;
    size_t off;
    size_t chunk;
    size_t i;

    memcpy(ctr, nonce, 12);
    memset(ctr + 12, 0, 4);
    c = 2;
    off = 0;
    while (off < len) {
        ctr[12] = (uint8_t)(c >> 24);
        ctr[13] = (uint8_t)(c >> 16);
        ctr[14] = (uint8_t)(c >> 8);
        ctr[15] = (uint8_t)c;
        c++;
        gcm_block_encrypt(rk, ctr, stream);
        chunk = len - off;
        if (chunk > 16)
            chunk = 16;
        for (i = 0; i < chunk; i++)
            out[off + i] = in[off + i] ^ stream[i];
        off += chunk;
    }
}

int lebtls_aead_seal(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t in_len, uint8_t *out) {
    uint8_t rk[11][16];
    uint8_t h[16];
    uint8_t s0[16];
    uint8_t tag[16];
    uint8_t zero[16];

    if (!key || !nonce || !out)
        return -1;
    if (!aad && aad_len > 0)
        return -1;
    if (!in && in_len > 0)
        return -1;
    gcm_key_expand(key, rk);
    memset(zero, 0, 16);
    gcm_block_encrypt(rk, zero, h);
    gcm_ctr(rk, nonce, in, in_len, out);
    gcm_ghash(h, aad, aad_len, out, in_len, tag);
    memcpy(s0, nonce, 12);
    memset(s0 + 12, 0, 4);
    s0[15] = 1;
    gcm_block_encrypt(rk, s0, s0);
    gcm_xor_block(tag, tag, s0);
    memcpy(out + in_len, tag, 16);
    memset(rk, 0, sizeof(rk));
    memset(h, 0, sizeof(h));
    memset(tag, 0, sizeof(tag));
    return 0;
}

int lebtls_aead_open(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t in_len, uint8_t *out) {
    uint8_t rk[11][16];
    uint8_t h[16];
    uint8_t s0[16];
    uint8_t tag[16];
    uint8_t zero[16];

    if (!key || !nonce || !out)
        return -1;
    if (!aad && aad_len > 0)
        return -1;
    if (!in && in_len > 0)
        return -1;
    if (in_len < 16)
        return -1;
    gcm_key_expand(key, rk);
    memset(zero, 0, 16);
    gcm_block_encrypt(rk, zero, h);
    gcm_ghash(h, aad, aad_len, in, in_len - 16, tag);
    memcpy(s0, nonce, 12);
    memset(s0 + 12, 0, 4);
    s0[15] = 1;
    gcm_block_encrypt(rk, s0, s0);
    gcm_xor_block(tag, tag, s0);
    if (crypto_constant_compare(tag, in + in_len - 16, 16) != 0) {
        memset(rk, 0, sizeof(rk));
        return -1;
    }
    gcm_ctr(rk, nonce, in, in_len - 16, out);
    memset(rk, 0, sizeof(rk));
    memset(h, 0, sizeof(h));
    memset(tag, 0, sizeof(tag));
    return 0;
}

int lebtls_aead_selftest(void) {
    static const uint8_t key[16] = { 0 };
    static const uint8_t nonce[12] = { 0 };
    static const uint8_t pt[16] = { 0 };
    static const uint8_t expect_ct[16] = {
        0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
        0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78
    };
    static const uint8_t expect_tag[16] = {
        0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
        0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf
    };
    uint8_t buf[32];
    uint8_t dec[16];

    memset(buf, 0, sizeof(buf));
    if (lebtls_aead_seal(key, nonce, NULL, 0, pt, 16, buf) < 0)
        return -1;
    if (memcmp(buf, expect_ct, 16) != 0)
        return -1;
    if (memcmp(buf + 16, expect_tag, 16) != 0)
        return -1;
    if (lebtls_aead_open(key, nonce, NULL, 0, buf, 32, dec) < 0)
        return -1;
    if (memcmp(dec, pt, 16) != 0)
        return -1;
    buf[0] ^= 1;
    if (lebtls_aead_open(key, nonce, NULL, 0, buf, 32, dec) == 0)
        return -1;
    memset(buf, 0, sizeof(buf));
    memset(dec, 0, sizeof(dec));
    return 0;
}

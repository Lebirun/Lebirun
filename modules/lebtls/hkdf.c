#include <lebirun/crypto.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include "tls_int.h"

int lebtls_hkdf_extract(const uint8_t *salt, size_t salt_len,
                        const uint8_t *ikm, size_t ikm_len, uint8_t out[32]) {
    uint8_t zeros[CRYPTO_SHA256_DIGEST_SIZE];

    if (!out)
        return -1;
    if (!salt || salt_len == 0) {
        memset(zeros, 0, sizeof(zeros));
        salt = zeros;
        salt_len = sizeof(zeros);
    }
    if (!ikm || ikm_len == 0)
        return -1;
    hmac_sha256(salt, salt_len, ikm, ikm_len, out);
    return 0;
}

int lebtls_hkdf_expand(const uint8_t secret[32], const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len) {
    uint8_t t[CRYPTO_SHA256_DIGEST_SIZE];
    uint8_t *msg;
    size_t prev;
    size_t msg_len;
    size_t done;
    size_t take;
    unsigned int i;
    unsigned int n;

    if (!secret || !out)
        return -1;
    if (out_len == 0)
        return 0;
    if (!info && info_len > 0)
        return -1;
    n = (unsigned int)((out_len + CRYPTO_SHA256_DIGEST_SIZE - 1) / CRYPTO_SHA256_DIGEST_SIZE);
    if (n == 0 || n > 255)
        return -1;
    done = 0;
    for (i = 1; i <= n; i++) {
        prev = (i > 1) ? CRYPTO_SHA256_DIGEST_SIZE : 0;
        msg_len = prev + info_len + 1;
        if (msg_len < prev)
            return -1;
        msg = kmalloc(msg_len);
        if (!msg)
            return -1;
        if (prev > 0)
            memcpy(msg, t, prev);
        if (info_len > 0)
            memcpy(msg + prev, info, info_len);
        msg[prev + info_len] = (uint8_t)i;
        hmac_sha256(secret, CRYPTO_SHA256_DIGEST_SIZE, msg, msg_len, t);
        kfree(msg);
        take = out_len - done;
        if (take > CRYPTO_SHA256_DIGEST_SIZE)
            take = CRYPTO_SHA256_DIGEST_SIZE;
        memcpy(out + done, t, take);
        done += take;
    }
    memset(t, 0, sizeof(t));
    return 0;
}

int lebtls_hkdf_selftest(void) {
    static const uint8_t ikm[22] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
    };
    static const uint8_t salt[13] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c
    };
    static const uint8_t info[10] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
        0xf8, 0xf9
    };
    static const uint8_t expect[42] = {
        0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a,
        0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
        0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c,
        0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf,
        0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18,
        0x58, 0x65
    };
    uint8_t prk[32];
    uint8_t okm[42];

    if (lebtls_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk) < 0)
        return -1;
    if (lebtls_hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm)) < 0) {
        memset(prk, 0, sizeof(prk));
        return -1;
    }
    memset(prk, 0, sizeof(prk));
    if (memcmp(okm, expect, sizeof(expect)) != 0) {
        memset(okm, 0, sizeof(okm));
        return -1;
    }
    memset(okm, 0, sizeof(okm));
    return 0;
}

int lebtls_hkdf_label(const uint8_t secret[32], const char *label,
                      const uint8_t *ctx, size_t ctx_len,
                      uint8_t *out, size_t out_len) {
    static const char prefix[] = "tls13 ";
    uint8_t *info;
    size_t lab_len;
    size_t info_len;
    size_t pos;
    int r;

    if (!secret || !label || !out)
        return -1;
    if (!ctx && ctx_len > 0)
        return -1;
    if (out_len == 0 || out_len > 255)
        return -1;
    lab_len = strlen(label);
    if (lab_len > 255 - sizeof(prefix))
        return -1;
    if (ctx_len > 255)
        return -1;
    info_len = 2 + 1 + (sizeof(prefix) - 1) + lab_len + 1 + ctx_len;
    info = kmalloc(info_len);
    if (!info)
        return -1;
    pos = 0;
    info[pos++] = (uint8_t)((out_len >> 8) & 0xFF);
    info[pos++] = (uint8_t)(out_len & 0xFF);
    info[pos++] = (uint8_t)(sizeof(prefix) - 1 + lab_len);
    memcpy(info + pos, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;
    memcpy(info + pos, label, lab_len);
    pos += lab_len;
    info[pos++] = (uint8_t)ctx_len;
    if (ctx_len > 0)
        memcpy(info + pos, ctx, ctx_len);
    r = lebtls_hkdf_expand(secret, info, info_len, out, out_len);
    memset(info, 0, info_len);
    kfree(info);
    return r;
}

#include <kernel/crypto.h>
#include <stddef.h>
#include <stdint.h>

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[32])
{
    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t tk[32];
    sha256_ctx_t ctx;
    size_t i;

    if (key_len > 64) {
        sha256_hash(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    for (i = 0; i < 64; i++) {
        k_ipad[i] = (i < key_len) ? key[i] : 0x00;
        k_opad[i] = (i < key_len) ? key[i] : 0x00;
    }
    for (i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, out);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, out, 32);
    sha256_final(&ctx, out);
}

void hmac_sha512(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[64])
{
    uint8_t k_ipad[128];
    uint8_t k_opad[128];
    uint8_t tk[64];
    sha512_ctx_t ctx;
    size_t i;

    if (key_len > 128) {
        sha512_hash(key, key_len, tk);
        key = tk;
        key_len = 64;
    }

    for (i = 0; i < 128; i++) {
        k_ipad[i] = (i < key_len) ? key[i] : 0x00;
        k_opad[i] = (i < key_len) ? key[i] : 0x00;
    }
    for (i = 0; i < 128; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    sha512_init(&ctx);
    sha512_update(&ctx, k_ipad, 128);
    sha512_update(&ctx, data, data_len);
    sha512_final(&ctx, out);

    sha512_init(&ctx);
    sha512_update(&ctx, k_opad, 128);
    sha512_update(&ctx, out, 64);
    sha512_final(&ctx, out);
}

int crypto_constant_compare(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t result;
    size_t i;

    result = 0;
    for (i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return (result == 0) ? 0 : -1;
}

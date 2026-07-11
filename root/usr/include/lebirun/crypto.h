#ifndef KERNEL_CRYPTO_H
#define KERNEL_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define CRYPTO_SHA256_DIGEST_SIZE 32
#define CRYPTO_SHA256_BLOCK_SIZE  64
#define CRYPTO_SHA512_DIGEST_SIZE 64
#define CRYPTO_SHA512_BLOCK_SIZE  128

#define CRYPTO_OP_SHA256       0
#define CRYPTO_OP_SHA512       1
#define CRYPTO_OP_HMAC_SHA256  2
#define CRYPTO_OP_HMAC_SHA512  3
#define CRYPTO_OP_COMPARE      4

typedef struct {
    uint8_t data[64];
    uint64_t datalen;
    uint64_t bitlen;
    uint64_t state[8];
} sha256_ctx_t;

typedef struct {
    uint8_t data[128];
    uint64_t datalen;
    uint64_t bitlen[2];
    uint64_t state[8];
} sha512_ctx_t;

struct crypto_request {
    int operation;
    const uint8_t *input;
    size_t input_len;
    const uint8_t *key;
    size_t key_len;
    uint8_t *output;
    size_t output_len;
};

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]);
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]);

void sha512_init(sha512_ctx_t *ctx);
void sha512_update(sha512_ctx_t *ctx, const uint8_t *data, size_t len);
void sha512_final(sha512_ctx_t *ctx, uint8_t hash[64]);
void sha512_hash(const uint8_t *data, size_t len, uint8_t hash[64]);

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[32]);
void hmac_sha512(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[64]);

int crypto_constant_compare(const uint8_t *a, const uint8_t *b, size_t len);

#endif

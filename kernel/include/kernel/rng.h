#ifndef KERNEL_RNG_H
#define KERNEL_RNG_H

#include <stdint.h>
#include <stddef.h>

#define RNG_ENTROPY_POOL_SIZE  256
#define RNG_CHACHA_KEY_SIZE     32
#define RNG_CHACHA_NONCE_SIZE   12
#define RNG_CHACHA_BLOCK_SIZE   64
#define RNG_NUM_POOLS           8
#define RNG_POOL_SIZE           32
#define RNG_RESEED_THRESHOLD  1024
#define RNG_JITTER_SAMPLES      64
#define RNG_OUTPUT_BUF_SIZE    256

void rng_init(void);
void rng_add_entropy(const void *data, size_t len);
void rng_reseed(void);

uint8_t  rng_get_u8(void);
uint16_t rng_get_u16(void);
uint32_t rng_get_u32(void);
uint64_t rng_get_u64(void);
void     rng_fill(void *buf, size_t len);

#endif

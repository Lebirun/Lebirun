#include <lebirun/rng.h>
#include <lebirun/crypto.h>
#include <lebirun/spinlock.h>
#include <lebirun/pit.h>
#include <lebirun/rtc.h>
#include <lebirun/io.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CHACHA_ROUNDS 20

typedef struct {
    uint32_t state[16];
    uint32_t key[8];
    uint32_t counter;
    uint32_t nonce[3];
} chacha20_ctx_t;

typedef struct {
    sha256_ctx_t hash_ctx;
    uint8_t data[RNG_POOL_SIZE];
    uint32_t count;
} entropy_pool_t;

typedef struct {
    chacha20_ctx_t chacha;
    entropy_pool_t pools[RNG_NUM_POOLS];
    uint32_t pool_index;
    uint32_t reseed_counter;
    uint32_t generation_counter;
    uint8_t output_buf[RNG_OUTPUT_BUF_SIZE];
    size_t output_pos;
    spinlock_t lock;
    int initialized;
    uint64_t last_tsc;
    uint32_t last_pit;
} rng_state_t;

static rng_state_t g_rng;

static uint64_t rng_irqsave(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void rng_irqrestore(uint64_t flags)
{
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
}

static inline uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void chacha_quarter_round(uint32_t *a, uint32_t *b,
                                 uint32_t *c, uint32_t *d)
{
    *a += *b; *d ^= *a; *d = rotl32(*d, 16);
    *c += *d; *b ^= *c; *b = rotl32(*b, 12);
    *a += *b; *d ^= *a; *d = rotl32(*d, 8);
    *c += *d; *b ^= *c; *b = rotl32(*b, 7);
}

static void chacha20_block(const chacha20_ctx_t *ctx, uint32_t out[16])
{
    int i;

    for (i = 0; i < 16; i++)
        out[i] = ctx->state[i];

    for (i = 0; i < CHACHA_ROUNDS; i += 2) {
        chacha_quarter_round(&out[0], &out[4], &out[8],  &out[12]);
        chacha_quarter_round(&out[1], &out[5], &out[9],  &out[13]);
        chacha_quarter_round(&out[2], &out[6], &out[10], &out[14]);
        chacha_quarter_round(&out[3], &out[7], &out[11], &out[15]);
        chacha_quarter_round(&out[0], &out[5], &out[10], &out[15]);
        chacha_quarter_round(&out[1], &out[6], &out[11], &out[12]);
        chacha_quarter_round(&out[2], &out[7], &out[8],  &out[13]);
        chacha_quarter_round(&out[3], &out[4], &out[9],  &out[14]);
    }

    for (i = 0; i < 16; i++)
        out[i] += ctx->state[i];
}

static void chacha20_init(chacha20_ctx_t *ctx,
                          const uint8_t key[32],
                          const uint8_t nonce[12])
{
    int i;

    ctx->state[0] = 0x61707865;
    ctx->state[1] = 0x3320646e;
    ctx->state[2] = 0x79622d32;
    ctx->state[3] = 0x6b206574;

    for (i = 0; i < 8; i++) {
        ctx->key[i] = (uint32_t)key[i * 4]
                    | ((uint32_t)key[i * 4 + 1] << 8)
                    | ((uint32_t)key[i * 4 + 2] << 16)
                    | ((uint32_t)key[i * 4 + 3] << 24);
        ctx->state[4 + i] = ctx->key[i];
    }

    ctx->counter = 0;
    ctx->state[12] = 0;

    for (i = 0; i < 3; i++) {
        ctx->nonce[i] = (uint32_t)nonce[i * 4]
                      | ((uint32_t)nonce[i * 4 + 1] << 8)
                      | ((uint32_t)nonce[i * 4 + 2] << 16)
                      | ((uint32_t)nonce[i * 4 + 3] << 24);
        ctx->state[13 + i] = ctx->nonce[i];
    }
}

static void chacha20_generate(chacha20_ctx_t *ctx, uint8_t *out, size_t len)
{
    uint32_t block[16];
    uint8_t *bp;
    size_t chunk;
    size_t i;

    while (len > 0) {
        ctx->state[12] = ctx->counter;
        chacha20_block(ctx, block);
        ctx->counter++;

        bp = (uint8_t *)block;
        chunk = len < 64 ? len : 64;
        for (i = 0; i < chunk; i++)
            out[i] = bp[i];
        out += chunk;
        len -= chunk;
    }

    memset(block, 0, sizeof(block));
}

static uint64_t jitter_collect_one(void)
{
    uint64_t t1, t2, t3;
    volatile uint32_t dummy;
    int i;

    t1 = read_tsc();

    dummy = 0;
    for (i = 0; i < 100; i++)
        dummy += i * i;

    t2 = read_tsc();

    dummy = 0;
    for (i = 0; i < 50; i++) {
        dummy ^= (dummy << 3) | (dummy >> 29);
        dummy += 0x9e3779b9;
    }

    t3 = read_tsc();

    return (t2 - t1) ^ ((t3 - t2) << 16) ^ (t3 - t1);
}

static void jitter_harvest(uint8_t *out, size_t len)
{
    sha256_ctx_t ctx;
    uint8_t hash[32];
    uint64_t sample;
    size_t filled;
    size_t i;
    size_t chunk;

    filled = 0;
    while (filled < len) {
        sha256_init(&ctx);
        for (i = 0; i < RNG_JITTER_SAMPLES; i++) {
            sample = jitter_collect_one();
            sha256_update(&ctx, (const uint8_t *)&sample, sizeof(sample));
        }
        sha256_final(&ctx, hash);

        chunk = len - filled;
        if (chunk > 32)
            chunk = 32;
        memcpy(out + filled, hash, chunk);
        filled += chunk;
    }

    memset(hash, 0, sizeof(hash));
    memset(&ctx, 0, sizeof(ctx));
}

static void pool_init(entropy_pool_t *pool)
{
    memset(pool->data, 0, RNG_POOL_SIZE);
    pool->count = 0;
}

static void pool_add(entropy_pool_t *pool, const uint8_t *data, size_t len)
{
    sha256_ctx_t ctx;
    uint8_t hash[32];

    sha256_init(&ctx);
    sha256_update(&ctx, pool->data, RNG_POOL_SIZE);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);

    memcpy(pool->data, hash, RNG_POOL_SIZE);
    pool->count++;

    memset(hash, 0, sizeof(hash));
    memset(&ctx, 0, sizeof(ctx));
}

static void collect_boot_entropy(uint8_t *buf, size_t len)
{
    sha256_ctx_t ctx;
    uint8_t hash[32];
    uint64_t tsc_val;
    uint32_t pit_val;
    uint32_t rtc_val;
    uint32_t stack_addr;
    uint64_t jitter_vals[8];
    uint32_t pit_sub;
    size_t filled;
    size_t chunk;
    int i;

    filled = 0;
    while (filled < len) {
        sha256_init(&ctx);

        tsc_val = read_tsc();
        sha256_update(&ctx, (const uint8_t *)&tsc_val, sizeof(tsc_val));

        pit_val = pit_get_ticks();
        sha256_update(&ctx, (const uint8_t *)&pit_val, sizeof(pit_val));

        rtc_val = rtc_get_time();
        sha256_update(&ctx, (const uint8_t *)&rtc_val, sizeof(rtc_val));

        stack_addr = (uint32_t)(uintptr_t)&stack_addr;
        sha256_update(&ctx, (const uint8_t *)&stack_addr, sizeof(stack_addr));

        for (i = 0; i < 8; i++)
            jitter_vals[i] = jitter_collect_one();
        sha256_update(&ctx, (const uint8_t *)jitter_vals, sizeof(jitter_vals));

        tsc_val = read_tsc();
        sha256_update(&ctx, (const uint8_t *)&tsc_val, sizeof(tsc_val));

        pit_sub = pit_get_ticks();
        sha256_update(&ctx, (const uint8_t *)&pit_sub, sizeof(pit_sub));

        sha256_final(&ctx, hash);

        chunk = len - filled;
        if (chunk > 32)
            chunk = 32;
        memcpy(buf + filled, hash, chunk);
        filled += chunk;
    }

    memset(hash, 0, sizeof(hash));
    memset(&ctx, 0, sizeof(ctx));
    memset(jitter_vals, 0, sizeof(jitter_vals));
}

static void rng_do_reseed(void)
{
    uint8_t seed_material[RNG_CHACHA_KEY_SIZE + RNG_CHACHA_NONCE_SIZE];
    sha256_ctx_t ctx;
    uint8_t hash[32];
    uint8_t jitter_buf[32];
    uint64_t tsc_val;
    uint32_t pit_val;
    uint32_t mask;
    int i;

    sha256_init(&ctx);

    for (i = 0; i < RNG_NUM_POOLS; i++) {
        mask = (1U << i) - 1;
        if ((g_rng.reseed_counter & mask) == 0) {
            sha256_update(&ctx, g_rng.pools[i].data, RNG_POOL_SIZE);
        }
    }

    tsc_val = read_tsc();
    sha256_update(&ctx, (const uint8_t *)&tsc_val, sizeof(tsc_val));
    pit_val = pit_get_ticks();
    sha256_update(&ctx, (const uint8_t *)&pit_val, sizeof(pit_val));

    jitter_harvest(jitter_buf, 32);
    sha256_update(&ctx, jitter_buf, 32);

    sha256_update(&ctx, (const uint8_t *)&g_rng.reseed_counter,
                  sizeof(g_rng.reseed_counter));

    sha256_final(&ctx, hash);

    memcpy(seed_material, hash, 32);

    sha256_init(&ctx);
    sha256_update(&ctx, hash, 32);
    tsc_val = read_tsc();
    sha256_update(&ctx, (const uint8_t *)&tsc_val, sizeof(tsc_val));
    sha256_final(&ctx, hash);
    memcpy(seed_material + 32, hash, RNG_CHACHA_NONCE_SIZE);

    chacha20_init(&g_rng.chacha, seed_material, seed_material + 32);

    g_rng.reseed_counter++;
    g_rng.output_pos = RNG_OUTPUT_BUF_SIZE;

    memset(seed_material, 0, sizeof(seed_material));
    memset(hash, 0, sizeof(hash));
    memset(jitter_buf, 0, sizeof(jitter_buf));
    memset(&ctx, 0, sizeof(ctx));
}

static void rng_fill_buffer(void)
{
    chacha20_generate(&g_rng.chacha, g_rng.output_buf, RNG_OUTPUT_BUF_SIZE);
    g_rng.output_pos = 0;
    g_rng.generation_counter += RNG_OUTPUT_BUF_SIZE;

    if (g_rng.generation_counter >= RNG_RESEED_THRESHOLD) {
        g_rng.generation_counter = 0;
        rng_do_reseed();
    }
}

static void rng_extract(uint8_t *out, size_t len)
{
    size_t avail;
    size_t chunk;

    while (len > 0) {
        if (g_rng.output_pos >= RNG_OUTPUT_BUF_SIZE)
            rng_fill_buffer();

        avail = RNG_OUTPUT_BUF_SIZE - g_rng.output_pos;
        chunk = len < avail ? len : avail;
        memcpy(out, g_rng.output_buf + g_rng.output_pos, chunk);
        g_rng.output_pos += chunk;
        out += chunk;
        len -= chunk;
    }
}

void rng_init(void)
{
    uint8_t boot_entropy[RNG_ENTROPY_POOL_SIZE];
    uint8_t initial_key[32];
    uint8_t initial_nonce[12];
    int i;

    spinlock_init(&g_rng.lock);
    g_rng.pool_index = 0;
    g_rng.reseed_counter = 0;
    g_rng.generation_counter = 0;
    g_rng.output_pos = RNG_OUTPUT_BUF_SIZE;
    g_rng.last_tsc = 0;
    g_rng.last_pit = 0;

    for (i = 0; i < RNG_NUM_POOLS; i++)
        pool_init(&g_rng.pools[i]);

    collect_boot_entropy(boot_entropy, RNG_ENTROPY_POOL_SIZE);

    for (i = 0; i < RNG_NUM_POOLS; i++) {
        pool_add(&g_rng.pools[i],
                 boot_entropy + (i * (RNG_ENTROPY_POOL_SIZE / RNG_NUM_POOLS)),
                 RNG_ENTROPY_POOL_SIZE / RNG_NUM_POOLS);
    }

    jitter_harvest(initial_key, 32);
    jitter_harvest(initial_nonce, 12);

    chacha20_init(&g_rng.chacha, initial_key, initial_nonce);
    g_rng.reseed_counter = 1;

    rng_do_reseed();

    g_rng.initialized = 1;

    memset(boot_entropy, 0, sizeof(boot_entropy));
    memset(initial_key, 0, sizeof(initial_key));
    memset(initial_nonce, 0, sizeof(initial_nonce));
}

void rng_add_entropy(const void *data, size_t len)
{
    uint64_t tsc_val;
    uint64_t flags;
    uint32_t idx;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);

    tsc_val = read_tsc();
    idx = g_rng.pool_index % RNG_NUM_POOLS;

    pool_add(&g_rng.pools[idx], (const uint8_t *)data, len);
    pool_add(&g_rng.pools[idx], (const uint8_t *)&tsc_val, sizeof(tsc_val));

    g_rng.pool_index++;

    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
}

void rng_reseed(void)
{
    uint64_t flags;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);
    rng_do_reseed();
    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
}

uint8_t rng_get_u8(void)
{
    uint8_t val;
    uint64_t flags;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);
    rng_extract(&val, sizeof(val));
    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
    return val;
}

uint16_t rng_get_u16(void)
{
    uint16_t val;
    uint64_t flags;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);
    rng_extract((uint8_t *)&val, sizeof(val));
    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
    return val;
}

uint32_t rng_get_u32(void)
{
    uint32_t val;
    uint64_t flags;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);
    rng_extract((uint8_t *)&val, sizeof(val));
    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
    return val;
}

uint64_t rng_get_u64(void)
{
    uint64_t val;
    uint64_t flags;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);
    rng_extract((uint8_t *)&val, sizeof(val));
    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
    return val;
}

void rng_fill(void *buf, size_t len)
{
    uint64_t flags;

    flags = rng_irqsave();
    spin_lock(&g_rng.lock);
    rng_extract((uint8_t *)buf, len);
    spin_unlock(&g_rng.lock);
    rng_irqrestore(flags);
}

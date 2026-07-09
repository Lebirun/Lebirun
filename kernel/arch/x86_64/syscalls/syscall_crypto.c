#include "syscall_defs.h"
#include <lebirun/crypto.h>

extern void **syscall_table;

static int sys_crypto(int req_ptr, int unused1, int unused2, int unused3, int unused4, int unused5)
{
    struct crypto_request *req;
    uint8_t hash256[32];
    uint8_t hash512[64];

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;

    req = (struct crypto_request *)(uintptr_t)req_ptr;
    if (!req) return -EFAULT;
    if (!req->input) return -EFAULT;
    if (!req->output) return -EFAULT;

    switch (req->operation) {
    case CRYPTO_OP_SHA256:
        if (req->output_len < 32) return -EINVAL;
        sha256_hash(req->input, req->input_len, hash256);
        memcpy(req->output, hash256, 32);
        return 0;

    case CRYPTO_OP_SHA512:
        if (req->output_len < 64) return -EINVAL;
        sha512_hash(req->input, req->input_len, hash512);
        memcpy(req->output, hash512, 64);
        return 0;

    case CRYPTO_OP_HMAC_SHA256:
        if (!req->key) return -EFAULT;
        if (req->output_len < 32) return -EINVAL;
        hmac_sha256(req->key, req->key_len, req->input, req->input_len, hash256);
        memcpy(req->output, hash256, 32);
        return 0;

    case CRYPTO_OP_HMAC_SHA512:
        if (!req->key) return -EFAULT;
        if (req->output_len < 64) return -EINVAL;
        hmac_sha512(req->key, req->key_len, req->input, req->input_len, hash512);
        memcpy(req->output, hash512, 64);
        return 0;

    case CRYPTO_OP_COMPARE:
        if (!req->key) return -EFAULT;
        if (req->input_len != req->key_len) return -EINVAL;
        return crypto_constant_compare(req->input, req->key, req->input_len);

    default:
        return -ENOSYS;
    }
}

void syscalls_crypto_init(void)
{
    syscall_table[SYSCALL_CRYPTO] = sys_crypto;
}

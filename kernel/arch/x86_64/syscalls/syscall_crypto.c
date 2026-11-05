#include "syscall_defs.h"
#include <lebirun/crypto.h>


static int sys_crypto(uint64_t req_ptr, int unused1, int unused2, int unused3,
                      int unused4, int unused5)
{
    struct crypto_request r;
    uint8_t hash[64];
    size_t n;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;

    if (!req_ptr || copy_from_user(&r, (const void *)(uintptr_t)req_ptr,
                                   sizeof(r)) < 0)
        return -EFAULT;
    if (!r.input || !r.output) return -EFAULT;
    if (!user_access_ok(r.input, r.input_len, UACCESS_READ))
        return -EFAULT;
    if (r.operation >= CRYPTO_OP_HMAC_SHA256 &&
        (!r.key || !user_access_ok(r.key, r.key_len, UACCESS_READ)))
        return -EFAULT;

    switch (r.operation) {
    case CRYPTO_OP_SHA256:
        n = 32;
        if (r.output_len < n) return -EINVAL;
        sha256_hash(r.input, r.input_len, hash);
        break;

    case CRYPTO_OP_SHA512:
        n = 64;
        if (r.output_len < n) return -EINVAL;
        sha512_hash(r.input, r.input_len, hash);
        break;

    case CRYPTO_OP_HMAC_SHA256:
        n = 32;
        if (r.output_len < n) return -EINVAL;
        hmac_sha256(r.key, r.key_len, r.input, r.input_len, hash);
        break;

    case CRYPTO_OP_HMAC_SHA512:
        n = 64;
        if (r.output_len < n) return -EINVAL;
        hmac_sha512(r.key, r.key_len, r.input, r.input_len, hash);
        break;

    case CRYPTO_OP_COMPARE:
        if (r.input_len != r.key_len) return -EINVAL;
        return crypto_constant_compare(r.input, r.key, r.input_len);

    default:
        return -ENOSYS;
    }
    return copy_to_user(r.output, hash, n) < 0 ? -EFAULT : 0;
}

void syscalls_crypto_init(void)
{
    syscall_table_set(SYSCALL_CRYPTO, (void *)(sys_crypto));
}

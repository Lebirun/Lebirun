#include <lebirun/mem_map.h>
#include <lebirun/crypto.h>
#include <lebirun/rtc.h>
#include <string.h>
#include "tls_int.h"

#define LEBTLS_RSA_MAX_LIMBS 128
#define LEBTLS_RSA_MAX_BYTES (LEBTLS_RSA_MAX_LIMBS * 4)

typedef struct {
    const uint8_t *p;
    uint64_t len;
} der_buf_t;

static int der_tlv(const uint8_t *buf, uint64_t len, uint8_t *tag_out,
                   const uint8_t **val_out, uint64_t *val_len_out,
                   uint64_t *hdr_len_out) {
    uint64_t pos;
    uint64_t llen;
    uint64_t vlen;
    int i;

    if (!buf || len < 2)
        return -1;
    pos = 1;
    if (buf[1] & 0x80) {
        llen = buf[1] & 0x7F;
        if (llen == 0 || llen > 4)
            return -1;
        if (len < 2 + llen)
            return -1;
        vlen = 0;
        for (i = 0; i < (int)llen; i++)
            vlen = (vlen << 8) | buf[2 + i];
        pos = 2 + llen;
    } else {
        vlen = buf[1];
        pos = 2;
    }
    if (vlen > len - pos)
        return -1;
    if (tag_out)
        *tag_out = buf[0];
    if (val_out)
        *val_out = buf + pos;
    if (val_len_out)
        *val_len_out = vlen;
    if (hdr_len_out)
        *hdr_len_out = pos;
    return 0;
}

static int der_child_at(const uint8_t *seq, uint64_t seq_len, int index,
                        uint8_t *tag_out, const uint8_t **val_out,
                        uint64_t *val_len_out, uint64_t *total_len_out) {
    uint64_t off;
    uint8_t tag;
    const uint8_t *val;
    uint64_t vlen;
    uint64_t hdr;
    int i;

    off = 0;
    for (i = 0; i <= index; i++) {
        if (off >= seq_len)
            return -1;
        if (der_tlv(seq + off, seq_len - off, &tag, &val, &vlen, &hdr) < 0)
            return -1;
        if (hdr > seq_len - off || vlen > seq_len - off - hdr)
            return -1;
        if (i == index) {
            if (tag_out)
                *tag_out = tag;
            if (val_out)
                *val_out = val;
            if (val_len_out)
                *val_len_out = vlen;
            if (total_len_out)
                *total_len_out = hdr + vlen;
            return 0;
        }
        off += hdr + vlen;
    }
    return -1;
}

static int der_is_oid(const uint8_t *val, uint64_t len,
                      const uint8_t *oid, uint64_t oid_len) {
    if (len != oid_len)
        return 0;
    return crypto_constant_compare(val, oid, (size_t)len) == 0;
}

static const uint8_t oid_rsa_enc[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const uint8_t oid_rsassa_pss[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0A };
static const uint8_t oid_sha256_rsa[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
static const uint8_t oid_ec_pubkey[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const uint8_t oid_secp256r1[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const uint8_t oid_secp384r1[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t oid_ecdsa_sha256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const uint8_t oid_ecdsa_sha384[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03 };
static const uint8_t oid_sha256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const uint8_t oid_mgf1[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x08 };
static const uint8_t oid_san[] = { 0x55, 0x1D, 0x11 };
static const uint8_t oid_cn[] = { 0x55, 0x04, 0x03 };

static const uint8_t sha256_digestinfo_prefix[] = {
    0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

void bn_from_bin(uint32_t *r, const uint8_t *bin, size_t len, size_t limbs) {
    size_t i;
    size_t j;
    size_t off;

    for (i = 0; i < limbs; i++)
        r[i] = 0;
    for (i = 0; i < len; i++) {
        j = (len - 1 - i) / 4;
        off = ((len - 1 - i) % 4) * 8;
        if (j < limbs)
            r[j] |= (uint32_t)bin[i] << off;
    }
}

static int bn_bitlen(const uint32_t *a, size_t limbs) {
    size_t i;
    int b;

    for (i = limbs; i > 0; i--) {
        if (a[i - 1] == 0)
            continue;
        b = 32;
        while (b > 0 && ((a[i - 1] >> (b - 1)) & 1) == 0)
            b--;
        return (int)((i - 1) * 32 + (size_t)b);
    }
    return 0;
}

int bn_cmp(const uint32_t *a, const uint32_t *b, size_t limbs) {
    size_t i;

    for (i = limbs; i > 0; i--) {
        if (a[i - 1] != b[i - 1])
            return a[i - 1] < b[i - 1] ? -1 : 1;
    }
    return 0;
}

void bn_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, size_t n) {
    uint64_t acc;
    size_t i;
    size_t j;

    for (i = 0; i < 2 * n; i++)
        r[i] = 0;
    for (i = 0; i < n; i++) {
        acc = 0;
        for (j = 0; j < n; j++) {
            acc += (uint64_t)r[i + j] + (uint64_t)a[i] * (uint64_t)b[j];
            r[i + j] = (uint32_t)acc;
            acc >>= 32;
        }
        r[i + n] = (uint32_t)acc;
    }
}

static void bn_shl1(uint32_t *r, size_t limbs) {
    size_t i;
    uint32_t carry;

    carry = 0;
    for (i = 0; i < limbs; i++) {
        uint32_t next = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = next;
    }
}

static int bn_sub_inplace(uint32_t *a, const uint32_t *b, size_t limbs) {
    size_t i;
    uint64_t borrow;
    uint64_t t;

    borrow = 0;
    for (i = 0; i < limbs; i++) {
        t = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
    return borrow ? -1 : 0;
}

int bn_mod(uint32_t *r, const uint32_t *a, const uint32_t *m, size_t n,
                  uint32_t *work_a, uint32_t *work_m) {
    int abl;
    int mbl;
    int shift;
    int i;

    if (bn_cmp(a, m, n) < 0) {
        size_t k;

        for (k = n; k < 2 * n; k++) {
            if (a[k] != 0)
                break;
        }
        if (k == 2 * n) {
            memcpy(r, a, n * sizeof(uint32_t));
            return 0;
        }
    }
    memcpy(work_a, a, 2 * n * sizeof(uint32_t));
    abl = bn_bitlen(work_a, 2 * n);
    mbl = bn_bitlen(m, n);
    if (mbl <= 0 || abl < mbl)
        return -1;
    shift = abl - mbl;
    memcpy(work_m, m, n * sizeof(uint32_t));
    memset(work_m + n, 0, n * sizeof(uint32_t));
    for (i = 0; i < shift; i++)
        bn_shl1(work_m, 2 * n);
    for (i = shift; i >= 0; i--) {
        if (bn_cmp(work_a, work_m, 2 * n) >= 0)
            bn_sub_inplace(work_a, work_m, 2 * n);
        if (i > 0) {
            size_t k;
            uint32_t carry = 0;
            for (k = 2 * n; k > 0; k--) {
                uint32_t next = work_m[k - 1] & 1;
                work_m[k - 1] = (work_m[k - 1] >> 1) | (carry << 31);
                carry = next;
            }
        }
    }
    memcpy(r, work_a, n * sizeof(uint32_t));
    return 0;
}

static int bn_modexp(uint32_t *r, const uint32_t *base, const uint8_t *exp,
                     size_t exp_len, const uint32_t *mod, size_t n) {
    uint32_t *acc;
    uint32_t *tmp;
    uint32_t *wa;
    uint32_t *wm;
    size_t i;
    int bit;

    acc = kmalloc(n * sizeof(uint32_t));
    tmp = kmalloc(2 * n * sizeof(uint32_t));
    wa = kmalloc(2 * n * sizeof(uint32_t));
    wm = kmalloc(2 * n * sizeof(uint32_t));
    if (!acc || !tmp || !wa || !wm) {
        if (acc)
            kfree(acc);
        if (tmp)
            kfree(tmp);
        if (wa)
            kfree(wa);
        if (wm)
            kfree(wm);
        return -1;
    }
    memset(acc, 0, n * sizeof(uint32_t));
    acc[0] = 1;
    for (i = 0; i < exp_len; i++) {
        int b;

        for (b = 7; b >= 0; b--) {
            bit = (exp[i] >> b) & 1;
            bn_mul(tmp, acc, acc, n);
            if (bn_mod(acc, tmp, mod, n, wa, wm) < 0)
                goto fail;
            if (bit) {
                bn_mul(tmp, acc, base, n);
                if (bn_mod(acc, tmp, mod, n, wa, wm) < 0)
                    goto fail;
            }
        }
    }
    memcpy(r, acc, n * sizeof(uint32_t));
    kfree(acc);
    kfree(tmp);
    kfree(wa);
    kfree(wm);
    return 0;

fail:
    memset(acc, 0, n * sizeof(uint32_t));
    memset(tmp, 0, 2 * n * sizeof(uint32_t));
    kfree(acc);
    kfree(tmp);
    kfree(wa);
    kfree(wm);
    return -1;
}

static int bn_strip(const uint8_t *bin, size_t len, const uint8_t **out,
                    size_t *out_len) {
    while (len > 0 && bin[0] == 0) {
        bin++;
        len--;
    }
    if (len == 0)
        return -1;
    *out = bin;
    *out_len = len;
    return 0;
}

static void mgf1_sha256(const uint8_t *seed, size_t seed_len,
                        uint8_t *out, size_t out_len) {
    uint8_t *msg;
    uint8_t digest[32];
    uint32_t counter;
    size_t done;
    size_t take;

    if (!seed || !out)
        return;
    if (out_len == 0)
        return;
    msg = kmalloc(seed_len + 4);
    if (!msg)
        return;
    memcpy(msg, seed, seed_len);
    counter = 0;
    done = 0;
    while (done < out_len) {
        msg[seed_len] = (uint8_t)(counter >> 24);
        msg[seed_len + 1] = (uint8_t)(counter >> 16);
        msg[seed_len + 2] = (uint8_t)(counter >> 8);
        msg[seed_len + 3] = (uint8_t)counter;
        sha256_hash(msg, seed_len + 4, digest);
        take = out_len - done;
        if (take > 32)
            take = 32;
        memcpy(out + done, digest, take);
        done += take;
        counter++;
    }
    memset(digest, 0, sizeof(digest));
    memset(msg, 0, seed_len + 4);
    kfree(msg);
}

static int rsa_pub_op(const uint8_t *n_bin, size_t n_len, const uint8_t *e_bin,
                      size_t e_len, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_len) {
    const uint8_t *n;
    const uint8_t *e;
    size_t nn;
    size_t ne;
    uint32_t *mod;
    uint32_t *base;
    uint32_t *res;
    size_t nlimbs;
    size_t i;

    if (bn_strip(n_bin, n_len, &n, &nn) < 0)
        return -1;
    if (bn_strip(e_bin, e_len, &e, &ne) < 0)
        return -1;
    if (nn == 0 || nn > LEBTLS_RSA_MAX_BYTES || ne == 0 || ne > 8)
        return -1;
    if (in_len != nn || out_len != nn)
        return -1;
    nlimbs = (nn + 3) / 4;
    mod = kmalloc(nlimbs * sizeof(uint32_t));
    base = kmalloc(nlimbs * sizeof(uint32_t));
    res = kmalloc(nlimbs * sizeof(uint32_t));
    if (!mod || !base || !res) {
        if (mod)
            kfree(mod);
        if (base)
            kfree(base);
        if (res)
            kfree(res);
        return -1;
    }
    bn_from_bin(mod, n, nn, nlimbs);
    bn_from_bin(base, in, in_len, nlimbs);
    if (bn_cmp(base, mod, nlimbs) >= 0) {
        memset(mod, 0, nlimbs * sizeof(uint32_t));
        memset(base, 0, nlimbs * sizeof(uint32_t));
        kfree(mod);
        kfree(base);
        kfree(res);
        return -1;
    }
    if (bn_modexp(res, base, e, ne, mod, nlimbs) < 0) {
        memset(mod, 0, nlimbs * sizeof(uint32_t));
        memset(base, 0, nlimbs * sizeof(uint32_t));
        kfree(mod);
        kfree(base);
        kfree(res);
        return -1;
    }
    memset(out, 0, out_len);
    for (i = 0; i < nlimbs; i++) {
        size_t k;

        for (k = 0; k < 4; k++) {
            size_t pos = out_len - 1 - (i * 4 + k);
            if (pos >= out_len)
                break;
            out[pos] = (uint8_t)(res[i] >> (k * 8));
        }
    }
    memset(mod, 0, nlimbs * sizeof(uint32_t));
    memset(base, 0, nlimbs * sizeof(uint32_t));
    memset(res, 0, nlimbs * sizeof(uint32_t));
    kfree(mod);
    kfree(base);
    kfree(res);
    return 0;
}

static int rsa_verify_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e,
                            size_t e_len, const uint8_t *msg, size_t msg_len,
                            const uint8_t *sig, size_t sig_len) {
    uint8_t *em;
    uint8_t hash[32];
    size_t ps_len;
    size_t i;
    int ok;

    if (!n || !e || !msg || !sig)
        return -1;
    em = kmalloc(sig_len);
    if (!em)
        return -1;
    sha256_hash(msg, msg_len, hash);
    if (rsa_pub_op(n, n_len, e, e_len, sig, sig_len, em, sig_len) < 0) {
        memset(em, 0, sig_len);
        kfree(em);
        return -1;
    }
    ok = -1;
    if (sig_len >= sizeof(sha256_digestinfo_prefix) + 32 + 11 &&
        em[0] == 0x00 && em[1] == 0x01) {
        ps_len = sig_len - sizeof(sha256_digestinfo_prefix) - 32 - 3;
        if (em[2 + ps_len] == 0x00 &&
            crypto_constant_compare(em + 2 + ps_len + 1, sha256_digestinfo_prefix,
                                    sizeof(sha256_digestinfo_prefix)) == 0 &&
            crypto_constant_compare(em + sig_len - 32, hash, 32) == 0) {
            ok = 0;
            for (i = 0; i < ps_len; i++) {
                if (em[2 + i] != 0xFF) {
                    ok = -1;
                    break;
                }
            }
        }
    }
    memset(em, 0, sig_len);
    memset(hash, 0, sizeof(hash));
    kfree(em);
    return ok;
}

static int rsa_verify_pss(const uint8_t *n, size_t n_len, const uint8_t *e,
                          size_t e_len, const uint8_t *msg, size_t msg_len,
                          const uint8_t *sig, size_t sig_len, size_t salt_len) {
    uint8_t *em;
    uint8_t mhash[32];
    uint8_t h[32];
    uint8_t *db;
    uint8_t *mm;
    size_t em_bits;
    size_t em_len;
    size_t db_len;
    size_t i;
    int ok;

    if (!n || !e || !msg || !sig)
        return -1;
    if (sig_len < 32 + salt_len + 2)
        return -1;
    em_len = sig_len;
    em = kmalloc(em_len);
    db = kmalloc(em_len);
    mm = kmalloc(8 + 32 + salt_len);
    if (!em || !db || !mm) {
        if (em)
            kfree(em);
        if (db)
            kfree(db);
        if (mm)
            kfree(mm);
        return -1;
    }
    if (rsa_pub_op(n, n_len, e, e_len, sig, sig_len, em, em_len) < 0)
        goto fail;
    if (em[em_len - 1] != 0xBC)
        goto fail;
    em_bits = em_len * 8;
    if (em_bits >= 8 && (em[0] & 0x80) != 0)
        goto fail;
    sha256_hash(msg, msg_len, mhash);
    db_len = em_len - 32 - 1;
    memcpy(db, em, db_len);
    memcpy(h, em + db_len, 32);
    mgf1_sha256(h, 32, db, db_len);
    for (i = 0; i < db_len; i++)
        db[i] ^= em[i];
    db[0] &= 0x7F;
    ok = -1;
    if (db_len >= salt_len + 1) {
        ok = 0;
        for (i = 0; i < db_len - salt_len - 1; i++) {
            if (db[i] != 0) {
                ok = -1;
                break;
            }
        }
        if (ok == 0 && db[db_len - salt_len - 1] != 0x01)
            ok = -1;
    }
    if (ok < 0)
        goto fail;
    memset(mm, 0, 8);
    memcpy(mm + 8, mhash, 32);
    memcpy(mm + 8 + 32, db + db_len - salt_len, salt_len);
    sha256_hash(mm, 8 + 32 + salt_len, mhash);
    if (crypto_constant_compare(mhash, h, 32) != 0)
        goto fail;
    memset(em, 0, em_len);
    memset(db, 0, em_len);
    memset(mm, 0, 8 + 32 + salt_len);
    memset(mhash, 0, sizeof(mhash));
    memset(h, 0, sizeof(h));
    kfree(em);
    kfree(db);
    kfree(mm);
    return 0;

fail:
    memset(em, 0, em_len);
    memset(db, 0, em_len);
    memset(mm, 0, 8 + 32 + salt_len);
    memset(mhash, 0, sizeof(mhash));
    memset(h, 0, sizeof(h));
    kfree(em);
    kfree(db);
    kfree(mm);
    return -1;
}

typedef struct {
    const uint8_t *tbs;
    uint64_t tbs_len;
    uint64_t tbs_hdr_len;
    const uint8_t *sig_alg;
    uint64_t sig_alg_len;
    const uint8_t *sig;
    uint64_t sig_len;
    const uint8_t *issuer;
    uint64_t issuer_len;
    const uint8_t *subject;
    uint64_t subject_len;
    int key_type;
    const uint8_t *n;
    uint64_t n_len;
    const uint8_t *e;
    uint64_t e_len;
    int ec_bits;
    uint8_t qx[48];
    uint8_t qy[48];
    const uint8_t *san;
    uint64_t san_len;
    const uint8_t *cn;
    uint64_t cn_len;
    uint64_t not_before;
    uint64_t not_after;
} cert_info_t;

static int parse_sig_alg(const uint8_t *seq, uint64_t len, int *kind_out) {
    uint8_t tag;
    const uint8_t *oid;
    uint64_t oid_len;

    if (der_child_at(seq, len, 0, &tag, &oid, &oid_len, NULL) < 0)
        return -1;
    if (tag != 0x06)
        return -1;
    if (der_is_oid(oid, oid_len, oid_sha256_rsa, sizeof(oid_sha256_rsa))) {
        *kind_out = 1;
        return 0;
    }
    if (der_is_oid(oid, oid_len, oid_rsassa_pss, sizeof(oid_rsassa_pss))) {
        *kind_out = 2;
        return 0;
    }
    if (der_is_oid(oid, oid_len, oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256))) {
        *kind_out = 3;
        return 0;
    }
    if (der_is_oid(oid, oid_len, oid_ecdsa_sha384, sizeof(oid_ecdsa_sha384))) {
        *kind_out = 4;
        return 0;
    }
    return -1;
}

static int parse_ec_key(const uint8_t *spki, uint64_t spki_len,
                        int *bits_out, uint8_t qx[48], uint8_t qy[48]) {
    uint8_t tag;
    const uint8_t *alg;
    uint64_t alg_len;
    const uint8_t *bits;
    uint64_t bits_len;
    uint8_t t2;
    const uint8_t *o2;
    uint64_t o2l;
    size_t coord;

    if (der_child_at(spki, spki_len, 0, &tag, &alg, &alg_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(alg, alg_len, 0, &t2, &o2, &o2l, NULL) < 0)
        return -1;
    if (t2 != 0x06 || !der_is_oid(o2, o2l, oid_ec_pubkey, sizeof(oid_ec_pubkey)))
        return -1;
    if (der_child_at(alg, alg_len, 1, &t2, &o2, &o2l, NULL) < 0)
        return -1;
    if (t2 != 0x06)
        return -1;
    if (der_is_oid(o2, o2l, oid_secp256r1, sizeof(oid_secp256r1))) {
        coord = 32;
    } else if (der_is_oid(o2, o2l, oid_secp384r1, sizeof(oid_secp384r1))) {
        coord = 48;
    } else {
        return -1;
    }
    if (der_child_at(spki, spki_len, 1, &tag, &bits, &bits_len, NULL) < 0)
        return -1;
    if (tag != 0x03 || bits_len != 2 * coord + 2 || bits[0] != 0x00 ||
        bits[1] != 0x04)
        return -1;
    memset(qx, 0, 48);
    memset(qy, 0, 48);
    memcpy(qx, bits + 2, coord);
    memcpy(qy, bits + 2 + coord, coord);
    *bits_out = coord == 32 ? 256 : 384;
    return 0;
}

static int parse_rsa_key(const uint8_t *spki, uint64_t spki_len,
                         const uint8_t **n_out, uint64_t *n_len_out,
                         const uint8_t **e_out, uint64_t *e_len_out) {
    uint8_t tag;
    const uint8_t *alg;
    uint64_t alg_len;
    const uint8_t *bits;
    uint64_t bits_len;
    const uint8_t *seq;
    uint64_t seq_len;
    const uint8_t *n;
    uint64_t n_len;
    const uint8_t *e;
    uint64_t e_len;

    if (der_child_at(spki, spki_len, 0, &tag, &alg, &alg_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    {
        uint8_t t2;
        const uint8_t *o2;
        uint64_t o2l;

        if (der_child_at(alg, alg_len, 0, &t2, &o2, &o2l, NULL) < 0)
            return -1;
        if (t2 != 0x06 ||
            !der_is_oid(o2, o2l, oid_rsa_enc, sizeof(oid_rsa_enc)))
            return -1;
    }
    if (der_child_at(spki, spki_len, 1, &tag, &bits, &bits_len, NULL) < 0)
        return -1;
    if (tag != 0x03 || bits_len < 1 || bits[0] != 0x00)
        return -1;
    if (der_tlv(bits + 1, bits_len - 1, &tag, &seq, &seq_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(seq, seq_len, 0, &tag, &n, &n_len, NULL) < 0)
        return -1;
    if (tag != 0x02)
        return -1;
    if (der_child_at(seq, seq_len, 1, &tag, &e, &e_len, NULL) < 0)
        return -1;
    if (tag != 0x02)
        return -1;
    *n_out = n;
    *n_len_out = n_len;
    *e_out = e;
    *e_len_out = e_len;
    return 0;
}

static uint64_t parse_time_val(uint8_t tag, const uint8_t *val, uint64_t len) {
    uint64_t y;
    uint64_t mo;
    uint64_t d;
    uint64_t h;
    uint64_t mi;
    uint64_t s;
    uint64_t v[6];
    int i;

    if (!val)
        return 0;
    if (tag == 0x17 && len >= 13) {
        y = (uint64_t)(val[0] - '0') * 10 + (val[1] - '0');
        y += y >= 50 ? 1900 : 2000;
        val += 2;
    } else if (tag == 0x18 && len >= 15) {
        y = (uint64_t)(val[0] - '0') * 1000 + (val[1] - '0') * 100 +
            (val[2] - '0') * 10 + (val[3] - '0');
        val += 4;
    } else {
        return 0;
    }
    v[0] = y;
    for (i = 1; i < 6; i++) {
        if (val[0] < '0' || val[0] > '9' || val[1] < '0' || val[1] > '9')
            return 0;
        v[i] = (uint64_t)(val[0] - '0') * 10 + (val[1] - '0');
        val += 2;
    }
    mo = v[1];
    d = v[2];
    h = v[3];
    mi = v[4];
    s = v[5];
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59)
        return 0;
    return (((((y * 100 + mo) * 100 + d) * 100 + h) * 100 + mi) * 100 + s);
}

static int find_cn(const uint8_t *name, uint64_t name_len,
                   const uint8_t **cn_out, uint64_t *cn_len_out) {
    uint64_t off;

    off = 0;
    while (off < name_len) {
        uint8_t tag;
        const uint8_t *val;
        uint64_t vlen;
        uint64_t hdr;
        uint8_t st;
        const uint8_t *sv;
        uint64_t sl;

        if (der_tlv(name + off, name_len - off, &tag, &val, &vlen, &hdr) < 0)
            return -1;
        if (tag != 0x31)
            return -1;
        off += hdr + vlen;
        if (der_child_at(val, vlen, 0, &st, &sv, &sl, NULL) < 0)
            continue;
        if (st != 0x30)
            continue;
        {
            int nth;

            nth = 0;
            for (;;) {
                uint8_t tt;
                const uint8_t *tv;
                uint64_t tl;
                uint8_t vt;
                const uint8_t *vv;
                uint64_t vl;

                if (der_child_at(sv, sl, nth, &tt, &tv, &tl, NULL) < 0)
                    break;
                if (tt == 0x06 && der_is_oid(tv, tl, oid_cn, sizeof(oid_cn))) {
                    if (der_child_at(sv, sl, nth + 1, &vt, &vv, &vl, NULL) == 0 &&
                        (vt == 0x13 || vt == 0x0C || vt == 0x16)) {
                        *cn_out = vv;
                        *cn_len_out = vl;
                        return 0;
                    }
                    nth += 2;
                    continue;
                }
                nth++;
            }
        }
    }
    return -1;
}

static void epoch_to_parts(uint64_t t, uint64_t *y, uint64_t *mo, uint64_t *d,
                           uint64_t *h, uint64_t *mi, uint64_t *s) {
    uint64_t days;
    uint64_t rem;
    uint64_t era;
    uint64_t doe;
    uint64_t yoe;
    uint64_t y2;
    uint64_t doy;
    uint64_t mp;

    days = t / 86400;
    rem = t % 86400;
    *h = rem / 3600;
    *mi = (rem % 3600) / 60;
    *s = rem % 60;
    days += 719468;
    era = days / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y2 = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp + (mp < 10 ? 3 : (uint64_t)(0 - 9));
    *y = y2 + (*mo <= 2 ? 1 : 0);
}

static uint64_t time_now_num(void) {
    uint64_t y;
    uint64_t mo;
    uint64_t d;
    uint64_t h;
    uint64_t mi;
    uint64_t s;

    epoch_to_parts(rtc_get_time(), &y, &mo, &d, &h, &mi, &s);
    return (((((y * 100 + mo) * 100 + d) * 100 + h) * 100 + mi) * 100 + s);
}

static int host_match(const char *host, const uint8_t *pat, uint64_t pat_len) {
    size_t host_len;
    size_t i;
    const char *dot;

    if (!host || !pat || pat_len == 0)
        return 0;
    host_len = strlen(host);
    if (pat_len == host_len && memcmp(pat, host, (size_t)pat_len) == 0)
        return 1;
    if (pat_len > 2 && pat[0] == '*' && pat[1] == '.') {
        dot = NULL;
        for (i = 0; host[i]; i++) {
            if (host[i] == '.') {
                dot = host + i;
                break;
            }
        }
        if (!dot || dot == host)
            return 0;
        if (host_len - (size_t)(dot - host) - 1 != (size_t)(pat_len - 2))
            return 0;
        return memcmp(dot + 1, pat + 2, (size_t)(pat_len - 2)) == 0;
    }
    return 0;
}

static int cert_host_ok(const cert_info_t *cert, const char *host) {
    uint64_t off;

    if (cert->san && cert->san_len > 0) {
        uint8_t tag;
        const uint8_t *seq;
        uint64_t seq_len;

        if (der_tlv(cert->san, cert->san_len, &tag, &seq, &seq_len, NULL) < 0)
            return -1;
        if (tag != 0x30)
            return -1;
        off = 0;
        while (off < seq_len) {
            uint8_t gt;
            const uint8_t *gv;
            uint64_t gl;
            uint64_t gh;

            if (der_tlv(seq + off, seq_len - off, &gt, &gv, &gl, &gh) < 0)
                return -1;
            off += gh + gl;
            if (gt == 0x82 && host_match(host, gv, gl))
                return 0;
        }
        return -1;
    }
    if (cert->cn && cert->cn_len > 0)
        return host_match(host, cert->cn, cert->cn_len) ? 0 : -1;
    return -1;
}

static int parse_cert(const uint8_t *der, uint64_t der_len, cert_info_t *out) {
    uint8_t tag;
    const uint8_t *outer;
    uint64_t outer_len;
    const uint8_t *tbs;
    uint64_t tbs_len;
    const uint8_t *sigalg;
    uint64_t sigalg_len;
    const uint8_t *sigbits;
    uint64_t sigbits_len;
    uint64_t shift;
    const uint8_t *v;
    uint64_t vl;
    uint64_t nch;
    uint64_t i;
    uint64_t tbs_total;

    memset(out, 0, sizeof(*out));
    if (der_tlv(der, der_len, &tag, &outer, &outer_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(outer, outer_len, 0, &tag, &tbs, &tbs_len, &tbs_total) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (tbs_total < tbs_len)
        return -1;
    out->tbs_hdr_len = tbs_total - tbs_len;
    if (der_child_at(outer, outer_len, 1, &tag, &sigalg, &sigalg_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(outer, outer_len, 2, &tag, &sigbits, &sigbits_len, NULL) < 0)
        return -1;
    if (tag != 0x03 || sigbits_len < 1 || sigbits[0] != 0x00)
        return -1;
    out->tbs = tbs;
    out->tbs_len = tbs_len;
    out->sig_alg = sigalg;
    out->sig_alg_len = sigalg_len;
    out->sig = sigbits + 1;
    out->sig_len = sigbits_len - 1;

    shift = 0;
    if (der_child_at(tbs, tbs_len, 0, &tag, NULL, NULL, NULL) < 0)
        return -1;
    if (tag == 0xA0)
        shift = 1;
    if (der_child_at(tbs, tbs_len, 2 + shift, &tag, &out->issuer, &out->issuer_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(tbs, tbs_len, 3 + shift, &tag, &v, &vl, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    {
        uint8_t tt;
        const uint8_t *tv;
        uint64_t tl;

        if (der_child_at(v, vl, 0, &tt, &tv, &tl, NULL) < 0)
            return -1;
        out->not_before = parse_time_val(tt, tv, tl);
        if (out->not_before == 0)
            return -1;
        if (der_child_at(v, vl, 1, &tt, &tv, &tl, NULL) < 0)
            return -1;
        out->not_after = parse_time_val(tt, tv, tl);
        if (out->not_after == 0)
            return -1;
    }
    if (der_child_at(tbs, tbs_len, 4 + shift, &tag, &out->subject, &out->subject_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(tbs, tbs_len, 5 + shift, &tag, &v, &vl, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    out->key_type = 0;
    out->ec_bits = 0;
    if (parse_rsa_key(v, vl, &out->n, &out->n_len, &out->e, &out->e_len) == 0) {
        out->key_type = 1;
    } else if (parse_ec_key(v, vl, &out->ec_bits, out->qx, out->qy) == 0) {
        out->key_type = 2;
    } else {
        return -1;
    }
    nch = 6 + shift;
    for (i = nch; i < nch + 4; i++) {
        uint8_t et;
        const uint8_t *ev;
        uint64_t el;
        uint8_t it;
        const uint8_t *iv;
        uint64_t il;
        uint64_t off;

        if (der_child_at(tbs, tbs_len, (int)i, &et, &ev, &el, NULL) < 0)
            break;
        if (et != 0xA0)
            continue;
        if (der_child_at(ev, el, 0, &it, &iv, &il, NULL) < 0)
            continue;
        if (it != 0x30)
            continue;
        off = 0;
        while (off < il) {
            uint8_t xt;
            const uint8_t *xv;
            uint64_t xl;
            uint64_t xh;
            uint8_t ot;
            const uint8_t *ov;
            uint64_t ol;

            if (der_tlv(iv + off, il - off, &xt, &xv, &xl, &xh) < 0)
                break;
            off += xh + xl;
            if (xt != 0x30)
                continue;
            if (der_child_at(xv, xl, 0, &ot, &ov, &ol, NULL) < 0)
                continue;
            if (ot != 0x06 || !der_is_oid(ov, ol, oid_san, sizeof(oid_san)))
                continue;
            if (der_child_at(xv, xl, 2, &ot, &ov, &ol, NULL) < 0) {
                if (der_child_at(xv, xl, 1, &ot, &ov, &ol, NULL) < 0)
                    continue;
            }
            if (ot != 0x04)
                continue;
            out->san = ov;
            out->san_len = ol;
        }
    }
    if (find_cn(out->subject, out->subject_len, &out->cn, &out->cn_len) < 0) {
        out->cn = NULL;
        out->cn_len = 0;
    }
    return 0;
}

static int sig_verify(cert_info_t *cert, const cert_info_t *issuer) {
    int kind;
    const uint8_t *tbs_full;
    size_t tbs_full_len;

    if (!cert || !issuer)
        return -1;
    if (parse_sig_alg(cert->sig_alg, cert->sig_alg_len, &kind) < 0)
        return -1;
    if (!cert->tbs || cert->tbs_hdr_len > cert->tbs_len + 16)
        return -1;
    tbs_full = cert->tbs - cert->tbs_hdr_len;
    tbs_full_len = (size_t)(cert->tbs_len + cert->tbs_hdr_len);
    if ((kind == 1 || kind == 2) && issuer->key_type != 1)
        return -1;
    if (kind == 3 && (issuer->key_type != 2 || issuer->ec_bits != 256))
        return -1;
    if (kind == 4 && (issuer->key_type != 2 || issuer->ec_bits != 384))
        return -1;
    if (kind == 1) {
        return rsa_verify_pkcs1(issuer->n, issuer->n_len, issuer->e,
                                issuer->e_len, tbs_full, tbs_full_len,
                                cert->sig, cert->sig_len);
    }
    if (kind == 2) {
        uint8_t tag;
        const uint8_t *params;
        uint64_t params_len;
        size_t salt_len;

        salt_len = 20;
        if (der_child_at(cert->sig_alg, cert->sig_alg_len, 1, &tag, &params,
                         &params_len, NULL) == 0 && tag == 0x30) {
            uint8_t ht;
            const uint8_t *hv;
            uint64_t hl;
            uint8_t mt;
            const uint8_t *mv;
            uint64_t ml;
            uint8_t pt;
            const uint8_t *pv;
            uint64_t pl;

            if (der_child_at(params, params_len, 0, &ht, &hv, &hl, NULL) == 0 &&
                ht == 0x30) {
                uint8_t ho;
                const uint8_t *ho_v;
                uint64_t ho_l;

                if (der_child_at(hv, hl, 0, &ho, &ho_v, &ho_l, NULL) < 0 ||
                    ho != 0x06 ||
                    !der_is_oid(ho_v, ho_l, oid_sha256, sizeof(oid_sha256)))
                    return -1;
            }
            if (der_child_at(params, params_len, 1, &mt, &mv, &ml, NULL) == 0 &&
                mt == 0x30) {
                uint8_t mo;
                const uint8_t *mo_v;
                uint64_t mo_l;

                if (der_child_at(mv, ml, 0, &mo, &mo_v, &mo_l, NULL) < 0 ||
                    mo != 0x06 ||
                    !der_is_oid(mo_v, mo_l, oid_mgf1, sizeof(oid_mgf1)))
                    return -1;
                if (der_child_at(mv, ml, 1, &mo, &mo_v, &mo_l, NULL) == 0 &&
                    mo == 0x30) {
                    uint8_t mho;
                    const uint8_t *mho_v;
                    uint64_t mho_l;

                    if (der_child_at(mo_v, mo_l, 0, &mho, &mho_v, &mho_l,
                                     NULL) < 0 || mho != 0x06 ||
                        !der_is_oid(mho_v, mho_l, oid_sha256,
                                    sizeof(oid_sha256)))
                        return -1;
                }
            }
            if (der_child_at(params, params_len, 2, &pt, &pv, &pl, NULL) == 0 &&
                pt == 0x02 && pl >= 1 && pl <= 4) {
                size_t k;
                size_t v;

                v = 0;
                for (k = 0; k < pl; k++)
                    v = (v << 8) | pv[k];
                if (v > 64)
                    return -1;
                salt_len = v;
            }
        }
        return rsa_verify_pss(issuer->n, issuer->n_len, issuer->e,
                              issuer->e_len, tbs_full, tbs_full_len,
                              cert->sig, cert->sig_len, salt_len);
    }
    if (kind == 3 || kind == 4) {
        uint8_t tag;
        const uint8_t *seq;
        uint64_t seq_len;
        const uint8_t *rv;
        uint64_t rv_len;
        const uint8_t *sv;
        uint64_t sv_len;
        uint8_t r[48];
        uint8_t s[48];
        uint8_t hash[48];
        size_t hash_len;
        size_t coord;

        coord = kind == 3 ? 32 : 48;
        if (der_tlv(cert->sig, cert->sig_len, &tag, &seq, &seq_len, NULL) < 0)
            return -1;
        if (tag != 0x30)
            return -1;
        if (der_child_at(seq, seq_len, 0, &tag, &rv, &rv_len, NULL) < 0)
            return -1;
        if (tag != 0x02)
            return -1;
        if (der_child_at(seq, seq_len, 1, &tag, &sv, &sv_len, NULL) < 0)
            return -1;
        if (tag != 0x02)
            return -1;
        if (rv_len == 0 || rv_len > coord + 1 || sv_len == 0 ||
            sv_len > coord + 1)
            return -1;
        memset(r, 0, sizeof(r));
        memset(s, 0, sizeof(s));
        memcpy(r + (coord - (rv_len > coord ? coord : rv_len)),
               rv + (rv_len > coord ? rv_len - coord : 0),
               rv_len > coord ? coord : rv_len);
        memcpy(s + (coord - (sv_len > coord ? coord : sv_len)),
               sv + (sv_len > coord ? sv_len - coord : 0),
               sv_len > coord ? coord : sv_len);
        if (kind == 3) {
            sha256_hash(tbs_full, tbs_full_len, hash);
            hash_len = 32;
        } else {
            sha384_hash(tbs_full, tbs_full_len, hash);
            hash_len = 48;
        }
        {
            int rc;

            if (kind == 3) {
                rc = lebtls_ecdsa_verify(issuer->qx, issuer->qy, hash,
                                         hash_len, r, s);
            } else {
                rc = lebtls_ecdsa384_verify(issuer->qx, issuer->qy, hash,
                                            hash_len, r, s);
            }
            memset(hash, 0, sizeof(hash));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            return rc;
        }
    }
    return -1;
}

static int name_equal(const uint8_t *a, uint64_t a_len,
                      const uint8_t *b, uint64_t b_len) {
    if (a_len != b_len)
        return 0;
    return crypto_constant_compare(a, b, (size_t)a_len) == 0;
}

static int b64_val(uint8_t c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static const char pem_begin[] = "-----BEGIN CERTIFICATE-----";

static int pem_next(const uint8_t *bundle, uint64_t bundle_len, uint64_t *off,
                    uint8_t **der_out, uint64_t *der_len_out) {
    uint64_t i;
    uint64_t start;
    uint64_t digits;
    uint8_t *der;
    uint64_t dlen;
    uint64_t v;
    int bits;
    int k;

    i = *off;
    while (i + sizeof(pem_begin) - 1 <= bundle_len) {
        if (memcmp(bundle + i, pem_begin, sizeof(pem_begin) - 1) == 0)
            break;
        i++;
    }
    if (i + sizeof(pem_begin) - 1 > bundle_len)
        return -1;
    i += sizeof(pem_begin) - 1;
    while (i < bundle_len && bundle[i] != '\n')
        i++;
    start = i;
    digits = 0;
    for (k = 0; (uint64_t)k + start < bundle_len; k++) {
        uint8_t c = bundle[start + k];
        if (c == '-')
            break;
        if (b64_val(c) >= 0 || c == '=')
            digits++;
        else if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            break;
    }
    dlen = digits / 4 * 3;
    if (dlen == 0)
        return -1;
    der = kmalloc((size_t)dlen + 3);
    if (!der)
        return -1;
    dlen = 0;
    v = 0;
    bits = 0;
    for (k = 0; start + (uint64_t)k < bundle_len; k++) {
        uint8_t c = bundle[start + (uint64_t)k];
        int b;

        if (c == '-')
            break;
        if (c == '=')
            break;
        b = b64_val(c);
        if (b < 0)
            continue;
        v = (v << 6) | (uint64_t)b;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            der[dlen++] = (uint8_t)((v >> bits) & 0xFF);
        }
    }
    *off = start + (uint64_t)k;
    *der_out = der;
    *der_len_out = dlen;
    return 0;
}


static int anchor_key_for(const uint8_t *bundle, uint64_t bundle_len,
                          const uint8_t *issuer, uint64_t issuer_len,
                          cert_info_t *key_out, uint8_t **keep_out) {
    uint64_t off;
    uint8_t *der;
    uint64_t der_len;
    cert_info_t anchor;
    int found;

    off = 0;
    found = -1;
    *keep_out = NULL;
    memset(key_out, 0, sizeof(*key_out));
    while (pem_next(bundle, bundle_len, &off, &der, &der_len) == 0) {
        if (parse_cert(der, der_len, &anchor) == 0) {
            if (name_equal(anchor.subject, anchor.subject_len, issuer, issuer_len)) {
                key_out->key_type = anchor.key_type;
                key_out->ec_bits = anchor.ec_bits;
                key_out->n = anchor.n;
                key_out->n_len = anchor.n_len;
                key_out->e = anchor.e;
                key_out->e_len = anchor.e_len;
                memcpy(key_out->qx, anchor.qx, sizeof(key_out->qx));
                memcpy(key_out->qy, anchor.qy, sizeof(key_out->qy));
                *keep_out = der;
                found = 0;
                break;
            }
        }
        kfree(der);
        if (off >= bundle_len)
            break;
    }
    return found;
}

int lebtls_rsa_pss_verify_msg(const uint8_t *n, size_t n_len,
                              const uint8_t *e, size_t e_len,
                              const uint8_t *msg, size_t msg_len,
                              const uint8_t *sig, size_t sig_len) {
    return rsa_verify_pss(n, n_len, e, e_len, msg, msg_len, sig, sig_len, 32);
}

int lebtls_ecdsa_sig_parse(const uint8_t *sig, size_t sig_len,
                           uint8_t r_out[48], uint8_t s_out[48],
                           size_t coord) {
    uint8_t tag;
    const uint8_t *seq;
    uint64_t seq_len;
    const uint8_t *rv;
    uint64_t rv_len;
    const uint8_t *sv;
    uint64_t sv_len;

    if (!sig || !r_out || !s_out)
        return -1;
    if (coord != 32 && coord != 48)
        return -1;
    if (der_tlv(sig, (uint64_t)sig_len, &tag, &seq, &seq_len, NULL) < 0)
        return -1;
    if (tag != 0x30)
        return -1;
    if (der_child_at(seq, seq_len, 0, &tag, &rv, &rv_len, NULL) < 0)
        return -1;
    if (tag != 0x02)
        return -1;
    if (der_child_at(seq, seq_len, 1, &tag, &sv, &sv_len, NULL) < 0)
        return -1;
    if (tag != 0x02)
        return -1;
    if (rv_len == 0 || rv_len > coord + 1 || sv_len == 0 ||
        sv_len > coord + 1)
        return -1;
    memset(r_out, 0, coord);
    memset(s_out, 0, coord);
    memcpy(r_out + (coord - (rv_len > coord ? coord : rv_len)),
           rv + (rv_len > coord ? rv_len - coord : 0),
           rv_len > coord ? coord : rv_len);
    memcpy(s_out + (coord - (sv_len > coord ? coord : sv_len)),
           sv + (sv_len > coord ? sv_len - coord : 0),
           sv_len > coord ? coord : sv_len);
    return 0;
}

int lebtls_verify_server_certs(const uint8_t *cert_msg, size_t cert_msg_len,
                               const char *host, int *type_out,
                               int *ec_bits_out,
                               uint8_t **n_out, size_t *n_len_out,
                               uint8_t **e_out, size_t *e_len_out,
                               uint8_t qx_out[48], uint8_t qy_out[48]) {
    uint64_t count;
    uint64_t off;
    cert_info_t *certs;
    uint8_t *bundle;
    uint64_t bundle_len;
    uint64_t now;
    uint64_t i;
    cert_info_t anchor_key;
    uint8_t *keep;
    uint8_t *ln;
    uint8_t *le;
    int rc;

    if (!cert_msg || cert_msg_len < 4 || !host || !type_out || !ec_bits_out ||
        !n_out || !n_len_out || !e_out || !e_len_out || !qx_out || !qy_out)
        return -1;
    *type_out = 0;
    *ec_bits_out = 0;
    *n_out = NULL;
    *n_len_out = 0;
    *e_out = NULL;
    *e_len_out = 0;
    memset(qx_out, 0, 48);
    memset(qy_out, 0, 48);
    count = 0;
    off = 4;
    while (off + 3 <= cert_msg_len) {
        uint64_t clen = ((uint64_t)cert_msg[off] << 16) |
                        ((uint64_t)cert_msg[off + 1] << 8) | cert_msg[off + 2];
        uint64_t elen;

        if (clen == 0 || clen > cert_msg_len - off - 3 - 2)
            return -1;
        off += 3 + clen;
        if (off + 2 > cert_msg_len)
            return -1;
        elen = ((uint64_t)cert_msg[off] << 8) | cert_msg[off + 1];
        if (elen > cert_msg_len - off - 2)
            return -1;
        off += 2 + elen;
        count++;
        if (count > cert_msg_len / 4 + 1)
            return -1;
    }
    if (count == 0 || off != cert_msg_len)
        return -1;
    certs = kmalloc(count * sizeof(cert_info_t));
    if (!certs)
        return -1;
    off = 4;
    for (i = 0; i < count; i++) {
        uint64_t clen = ((uint64_t)cert_msg[off] << 16) |
                        ((uint64_t)cert_msg[off + 1] << 8) | cert_msg[off + 2];
        uint64_t elen;

        if (parse_cert(cert_msg + off + 3, clen, &certs[i]) < 0) {
            kfree(certs);
            return -1;
        }
        off += 3 + clen;
        elen = ((uint64_t)cert_msg[off] << 8) | cert_msg[off + 1];
        off += 2 + elen;
    }
    now = time_now_num();
    for (i = 0; i < count; i++) {
        if (certs[i].not_before > now || now > certs[i].not_after) {
            kfree(certs);
            return -1;
        }
    }
    if (cert_host_ok(&certs[0], host) < 0) {
        kfree(certs);
        return -1;
    }
    for (i = 0; i + 1 < count; i++) {
        if (!name_equal(certs[i].issuer, certs[i].issuer_len,
                        certs[i + 1].subject, certs[i + 1].subject_len)) {
            kfree(certs);
            return -1;
        }
        if (sig_verify(&certs[i], &certs[i + 1]) < 0) {
            kfree(certs);
            return -1;
        }
    }
    bundle = lebtls_load_ca(&bundle_len);
    if (!bundle) {
        kfree(certs);
        return -1;
    }
    rc = -1;
    keep = NULL;
    memset(&anchor_key, 0, sizeof(anchor_key));
    for (i = count; i-- > 0;) {
        if (name_equal(certs[i].subject, certs[i].subject_len,
                       certs[i].issuer, certs[i].issuer_len)) {
            if (anchor_key_for(bundle, bundle_len, certs[i].subject,
                               certs[i].subject_len, &anchor_key,
                               &keep) == 0) {
                rc = 0;
                break;
            }
        } else if (anchor_key_for(bundle, bundle_len, certs[i].issuer,
                                  certs[i].issuer_len, &anchor_key,
                                  &keep) == 0) {
            if (sig_verify(&certs[i], &anchor_key) == 0) {
                rc = 0;
                break;
            }
            if (keep) {
                kfree(keep);
                keep = NULL;
            }
            memset(&anchor_key, 0, sizeof(anchor_key));
        }
        if (keep) {
            kfree(keep);
            keep = NULL;
        }
        memset(&anchor_key, 0, sizeof(anchor_key));
    }
    if (keep)
        kfree(keep);
    kfree(bundle);
    if (rc < 0) {
        kfree(certs);
        return -1;
    }
    *type_out = certs[0].key_type;
    if (certs[0].key_type == 1) {
        ln = kmalloc(certs[0].n_len);
        le = kmalloc(certs[0].e_len);
        if (!ln || !le) {
            if (ln)
                kfree(ln);
            if (le)
                kfree(le);
            kfree(certs);
            return -1;
        }
        memcpy(ln, certs[0].n, certs[0].n_len);
        memcpy(le, certs[0].e, certs[0].e_len);
        *n_out = ln;
        *n_len_out = certs[0].n_len;
        *e_out = le;
        *e_len_out = certs[0].e_len;
    } else if (certs[0].key_type == 2) {
        size_t coord;

        *n_out = NULL;
        *n_len_out = 0;
        *e_out = NULL;
        *e_len_out = 0;
        *ec_bits_out = certs[0].ec_bits;
        coord = certs[0].ec_bits == 384 ? 48 : 32;
        memcpy(qx_out, certs[0].qx, coord);
        memcpy(qy_out, certs[0].qy, coord);
    } else {
        kfree(certs);
        return -1;
    }
    kfree(certs);
    return 0;
}

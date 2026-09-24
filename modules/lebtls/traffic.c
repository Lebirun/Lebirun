#include <lebirun/mem_map.h>
#include <string.h>
#include "tls_int.h"

static void traffic_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    int i;

    memcpy(nonce, iv, 12);
    for (i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)((seq >> (i * 8)) & 0xFF);
}

int lebtls_traffic_seal(uint64_t *seq_io, const uint8_t *key, const uint8_t *iv,
                        uint8_t outer_type, const uint8_t *in, uint64_t in_len,
                        uint8_t **rec_out, uint64_t *rec_len_out) {
    uint8_t nonce[12];
    uint8_t hdr[5];
    uint8_t *pt;
    uint8_t *rec;

    if (!seq_io || !key || !iv || !rec_out || !rec_len_out)
        return -1;
    if (in_len > 16384)
        return -1;
    if (!in && in_len > 0)
        return -1;
    pt = kmalloc((size_t)in_len + 1);
    rec = kmalloc((size_t)in_len + 1 + 16 + 5);
    if (!pt || !rec) {
        if (pt)
            kfree(pt);
        if (rec)
            kfree(rec);
        return -1;
    }
    if (in_len > 0)
        memcpy(pt, in, (size_t)in_len);
    pt[in_len] = outer_type;
    traffic_nonce(iv, *seq_io, nonce);
    (*seq_io)++;
    hdr[0] = LEBTLS_RT_APP_DATA;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    hdr[3] = (uint8_t)(((in_len + 1 + 16) >> 8) & 0xFF);
    hdr[4] = (uint8_t)((in_len + 1 + 16) & 0xFF);
    if (lebtls_aead_seal(key, nonce, hdr, sizeof(hdr), pt, in_len + 1,
                         rec + 5) < 0) {
        memset(pt, 0, (size_t)in_len + 1);
        kfree(pt);
        kfree(rec);
        return -1;
    }
    memset(pt, 0, (size_t)in_len + 1);
    kfree(pt);
    memcpy(rec, hdr, sizeof(hdr));
    *rec_out = rec;
    *rec_len_out = (uint64_t)(in_len + 1 + 16 + 5);
    return 0;
}

int lebtls_traffic_open(uint64_t *seq_io, const uint8_t *key, const uint8_t *iv,
                        const uint8_t *rec, uint64_t rec_len,
                        uint8_t **pt_out, uint64_t *pt_len_out,
                        uint8_t *inner_type_out) {
    uint8_t nonce[12];
    uint8_t hdr[5];
    uint8_t *pt;
    uint64_t pt_len;

    if (!seq_io || !key || !iv || !rec || !pt_out || !pt_len_out ||
        !inner_type_out)
        return -1;
    if (rec_len < 16)
        return -1;
    pt_len = rec_len - 16;
    if (pt_len == 0)
        return -1;
    pt = kmalloc((size_t)pt_len);
    if (!pt)
        return -1;
    hdr[0] = LEBTLS_RT_APP_DATA;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    hdr[3] = (uint8_t)((rec_len >> 8) & 0xFF);
    hdr[4] = (uint8_t)(rec_len & 0xFF);
    traffic_nonce(iv, *seq_io, nonce);
    if (lebtls_aead_open(key, nonce, hdr, sizeof(hdr), rec, rec_len, pt) < 0) {
        kfree(pt);
        return -1;
    }
    (*seq_io)++;
    *inner_type_out = pt[pt_len - 1];
    *pt_out = pt;
    *pt_len_out = pt_len - 1;
    return 0;
}

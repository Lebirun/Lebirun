#include <lebirun/mem_map.h>
#include <lebirun/crypto.h>
#include <lebirun/rng.h>
#include <lebirun/task.h>
#include <string.h>
#include "tls_int.h"

static const uint8_t hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xC3, 0xFD, 0x77, 0x49,
    0x20, 0x6D, 0x5D, 0x60, 0x1D, 0xF2, 0xEE, 0xF7,
    0x07, 0xD0, 0x42, 0xA9, 0x37, 0x8E, 0xCD, 0x1F,
    0x38, 0x29, 0xB3, 0x73, 0x0B, 0x37, 0x8E, 0x3E
};

static const uint8_t empty_hash[32] = {
    0xE3, 0xB0, 0xC4, 0x42, 0x98, 0xFC, 0x1C, 0x14,
    0x9A, 0xFB, 0xF4, 0xC8, 0x99, 0x6F, 0xB9, 0x24,
    0x27, 0xAE, 0x41, 0xE4, 0x64, 0x9B, 0x93, 0x4C,
    0xA4, 0x95, 0x99, 0x1B, 0x78, 0x52, 0xB8, 0x55
};

int lebtls_transcript_append(tls_conn_t *conn, const uint8_t *data, uint64_t len) {
    uint8_t *grown;
    uint64_t need;
    uint64_t cap;

    if (!conn)
        return -1;
    if (len == 0)
        return 0;
    if (!data)
        return -1;
    need = conn->transcript_len + len;
    if (need < conn->transcript_len)
        return -1;
    if (need > conn->transcript_cap) {
        cap = conn->transcript_cap ? conn->transcript_cap : 4096;
        while (cap < need) {
            if (cap > UINT64_MAX / 2)
                return -1;
            cap *= 2;
        }
        grown = krealloc(conn->transcript, (size_t)cap);
        if (!grown)
            return -1;
        conn->transcript = grown;
        conn->transcript_cap = cap;
    }
    memcpy(conn->transcript + conn->transcript_len, data, (size_t)len);
    conn->transcript_len = need;
    return 0;
}

typedef struct {
    uint8_t *buf;
    uint64_t len;
    uint64_t cap;
} hs_buf_t;

static void hs_free(hs_buf_t *b) {
    if (b->buf) {
        memset(b->buf, 0, b->len);
        kfree(b->buf);
    }
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

static int hs_reserve(hs_buf_t *b, uint64_t extra) {
    uint8_t *grown;
    uint64_t need;
    uint64_t cap;

    need = b->len + extra;
    if (need < b->len)
        return -1;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > UINT64_MAX / 2)
            return -1;
        cap *= 2;
    }
    grown = krealloc(b->buf, (size_t)cap);
    if (!grown)
        return -1;
    b->buf = grown;
    b->cap = cap;
    return 0;
}

static int hs_u8(hs_buf_t *b, uint8_t v) {
    if (hs_reserve(b, 1) < 0)
        return -1;
    b->buf[b->len++] = v;
    return 0;
}

static int hs_u16(hs_buf_t *b, uint16_t v) {
    if (hs_reserve(b, 2) < 0)
        return -1;
    b->buf[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->buf[b->len++] = (uint8_t)(v & 0xFF);
    return 0;
}

static int hs_bytes(hs_buf_t *b, const uint8_t *data, uint64_t len) {
    if (len > 0 && !data)
        return -1;
    if (hs_reserve(b, len) < 0)
        return -1;
    if (len > 0)
        memcpy(b->buf + b->len, data, (size_t)len);
    b->len += len;
    return 0;
}

static int hs_drain(hs_buf_t *b, uint64_t len) {
    if (len > b->len)
        return -1;
    if (len == b->len) {
        memset(b->buf, 0, b->len);
        b->len = 0;
        return 0;
    }
    memmove(b->buf, b->buf + len, (size_t)(b->len - len));
    b->len -= len;
    return 0;
}

static int lebtls_build_client_hello(tls_conn_t *conn, hs_buf_t *out) {
    hs_buf_t body;
    hs_buf_t ext;
    hs_buf_t entry;
    uint8_t share[32];
    uint8_t rnd[32];
    size_t host_len;
    unsigned int i;
    int k;

    memset(&body, 0, sizeof(body));
    memset(&ext, 0, sizeof(ext));
    memset(&entry, 0, sizeof(entry));
    memset(out, 0, sizeof(*out));
    host_len = strlen(conn->host);
    if (host_len == 0 || host_len > 253)
        goto fail;
    for (i = 0; i < 32; i += 8) {
        uint64_t r = rng_get_u64();

        for (k = 0; k < 8; k++) {
            conn->eph_priv[i + (size_t)k] = (uint8_t)(r & 0xFF);
            r >>= 8;
        }
    }
    for (i = 0; i < 32; i += 8) {
        uint64_t r = rng_get_u64();

        for (k = 0; k < 8; k++) {
            rnd[i + (size_t)k] = (uint8_t)(r & 0xFF);
            r >>= 8;
        }
    }
    if (lebtls_x25519_pub(conn->eph_priv, share) < 0)
        goto fail;

    if (hs_u16(&body, 0x0303) < 0)
        goto fail;
    if (hs_bytes(&body, rnd, sizeof(rnd)) < 0)
        goto fail;
    if (hs_u8(&body, 0) < 0)
        goto fail;
    if (hs_u16(&body, 2) < 0 || hs_u16(&body, LEBTLS_SUITE_AES_128_GCM_SHA256) < 0)
        goto fail;
    if (hs_u8(&body, 1) < 0 || hs_u8(&body, 0) < 0)
        goto fail;

    if (hs_u8(&entry, 0) < 0 || hs_u16(&entry, (uint16_t)host_len) < 0)
        goto fail;
    if (hs_bytes(&entry, (const uint8_t *)conn->host, host_len) < 0)
        goto fail;
    if (hs_u16(&ext, 0) < 0)
        goto fail;
    if (hs_u16(&ext, (uint16_t)(entry.len + 2)) < 0)
        goto fail;
    if (hs_u16(&ext, (uint16_t)entry.len) < 0)
        goto fail;
    if (hs_bytes(&ext, entry.buf, entry.len) < 0)
        goto fail;

    if (hs_u16(&ext, 43) < 0 || hs_u16(&ext, 3) < 0)
        goto fail;
    if (hs_u8(&ext, 2) < 0 || hs_u16(&ext, 0x0304) < 0)
        goto fail;

    if (hs_u16(&ext, 13) < 0 || hs_u16(&ext, 8) < 0)
        goto fail;
    if (hs_u16(&ext, 6) < 0 || hs_u16(&ext, 0x0804) < 0 ||
        hs_u16(&ext, 0x0403) < 0 || hs_u16(&ext, 0x0503) < 0)
        goto fail;

    if (hs_u16(&ext, 10) < 0 || hs_u16(&ext, 4) < 0)
        goto fail;
    if (hs_u16(&ext, 2) < 0 || hs_u16(&ext, 0x001D) < 0)
        goto fail;

    if (hs_u16(&ext, 51) < 0)
        goto fail;
    if (hs_u16(&ext, 38) < 0)
        goto fail;
    if (hs_u16(&ext, 36) < 0)
        goto fail;
    if (hs_u16(&ext, 0x001D) < 0 || hs_u16(&ext, 32) < 0)
        goto fail;
    if (hs_bytes(&ext, share, sizeof(share)) < 0)
        goto fail;

    if (ext.len > 0xFFFF)
        goto fail;
    if (hs_u16(&body, (uint16_t)ext.len) < 0)
        goto fail;
    if (hs_bytes(&body, ext.buf, ext.len) < 0)
        goto fail;

    if (body.len > 0xFFFFFF)
        goto fail;
    if (hs_u8(out, LEBTLS_HS_CLIENT_HELLO) < 0)
        goto fail;
    if (hs_u8(out, (uint8_t)((body.len >> 16) & 0xFF)) < 0)
        goto fail;
    if (hs_u16(out, (uint16_t)(body.len & 0xFFFF)) < 0)
        goto fail;
    if (hs_bytes(out, body.buf, body.len) < 0)
        goto fail;

    memset(share, 0, sizeof(share));
    memset(rnd, 0, sizeof(rnd));
    hs_free(&body);
    hs_free(&ext);
    hs_free(&entry);
    return 0;

fail:
    memset(share, 0, sizeof(share));
    memset(rnd, 0, sizeof(rnd));
    hs_free(&body);
    hs_free(&ext);
    hs_free(&entry);
    hs_free(out);
    return -1;
}

static int transcript_hash(tls_conn_t *conn, uint8_t out[32]) {
    if (!conn || !out)
        return -1;
    if (conn->transcript_len > 0 && !conn->transcript)
        return -1;
    sha256_hash(conn->transcript ? conn->transcript : (const uint8_t *)"",
                (size_t)conn->transcript_len, out);
    return 0;
}

static int transcript_hash_n(tls_conn_t *conn, uint64_t n, uint8_t out[32]) {
    if (!conn || !out)
        return -1;
    if (n > conn->transcript_len)
        return -1;
    if (n > 0 && !conn->transcript)
        return -1;
    sha256_hash(conn->transcript ? conn->transcript : (const uint8_t *)"",
                (size_t)n, out);
    return 0;
}

static int parse_server_hello(const uint8_t *msg, uint64_t len,
                              uint8_t peer_key[32]) {
    const uint8_t *body;
    uint64_t blen;
    uint64_t off;
    uint64_t slen;
    uint64_t elen;
    uint64_t eoff;
    int saw_share;
    int saw_ver;

    if (!msg || len < 4 || !peer_key)
        return -1;
    if (msg[0] != 2)
        return -1;
    blen = ((uint64_t)msg[1] << 16) | ((uint64_t)msg[2] << 8) | msg[3];
    if (blen + 4 != len)
        return -1;
    body = msg + 4;
    if (blen < 2 + 32 + 1 + 2 + 1 + 2)
        return -1;
    if (body[0] != 0x03 || body[1] != 0x03)
        return -1;
    if (crypto_constant_compare(body + 2, hrr_random, 32) == 0)
        return -1;
    off = 2 + 32;
    slen = body[off];
    off += 1 + slen;
    if (off + 2 + 1 + 2 > blen)
        return -1;
    if (body[off] != 0x13 || body[off + 1] != 0x01)
        return -1;
    off += 2;
    if (body[off] != 0x00)
        return -1;
    off += 1;
    if (off + 2 > blen)
        return -1;
    elen = ((uint64_t)body[off] << 8) | body[off + 1];
    off += 2;
    if (elen > blen - off)
        return -1;
    saw_share = 0;
    saw_ver = 0;
    eoff = 0;
    while (eoff + 4 <= elen) {
        uint16_t et = (uint16_t)((body[off + eoff] << 8) | body[off + eoff + 1]);
        uint16_t el = (uint16_t)((body[off + eoff + 2] << 8) | body[off + eoff + 3]);
        const uint8_t *ed;

        if (el > elen - eoff - 4)
            return -1;
        ed = body + off + eoff + 4;
        if (et == 51 && el == 36) {
            if (ed[0] != 0x00 || ed[1] != 0x1D)
                return -1;
            if (ed[2] != 0x00 || ed[3] != 0x20)
                return -1;
            memcpy(peer_key, ed + 4, 32);
            saw_share = 1;
        } else if (et == 43 && el == 2) {
            if (ed[0] != 0x03 || ed[1] != 0x04)
                return -1;
            saw_ver = 1;
        }
        eoff += 4 + el;
    }
    if (eoff != elen || !saw_share || !saw_ver)
        return -1;
    return 0;
}

static int hs_next_message(hs_buf_t *stream, uint8_t *type_out,
                           const uint8_t **body_out, uint64_t *body_len_out) {
    uint64_t blen;

    if (!stream || !type_out || !body_out || !body_len_out)
        return -1;
    if (stream->len < 4)
        return 1;
    blen = ((uint64_t)stream->buf[1] << 16) |
           ((uint64_t)stream->buf[2] << 8) | stream->buf[3];
    if (blen > 16384)
        return -1;
    if (stream->len < 4 + blen)
        return 1;
    *type_out = stream->buf[0];
    *body_out = stream->buf + 4;
    *body_len_out = blen;
    return 0;
}

static int hs_consume(hs_buf_t *stream, uint64_t msg_len) {
    return hs_drain(stream, 4 + msg_len);
}

static int hs_append_record(tls_conn_t *conn, hs_buf_t *stream,
                            const uint8_t *rec, uint64_t rec_len) {
    if (lebtls_transcript_append(conn, rec, rec_len) < 0)
        return -1;
    return hs_bytes(stream, rec, rec_len);
}

static const char cert_verify_ctx[] = "TLS 1.3, server CertificateVerify";

int lebtls_skip_post_handshake(const uint8_t *pt, uint64_t pt_len) {
    uint64_t off;

    off = 0;
    while (off < pt_len) {
        uint64_t blen;

        if (pt_len - off < 4)
            return -1;
        if (pt[off] == 24)
            return -1;
        if (pt[off] != 4)
            return -1;
        blen = ((uint64_t)pt[off + 1] << 16) |
               ((uint64_t)pt[off + 2] << 8) | pt[off + 3];
        if (blen > pt_len - off - 4)
            return -1;
        off += 4 + blen;
    }
    return off == pt_len ? 0 : -1;
}

int lebtls_handshake(tls_conn_t *conn) {
    hs_buf_t hello;
    hs_buf_t stream;
    uint8_t type;
    uint8_t *rec;
    uint64_t rec_len;
    uint8_t peer_key[32];
    uint8_t shared[32];
    uint8_t early[32];
    uint8_t derived[32];
    uint8_t hs_secret[32];
    uint8_t zeros[32];
    uint8_t th[32];
    uint8_t fin_key[32];
    uint8_t cfin_key[32];
    const uint8_t *msg_body;
    uint64_t msg_len;
    uint8_t *leaf_n;
    size_t leaf_n_len;
    uint8_t *leaf_e;
    size_t leaf_e_len;
    int leaf_type;
    int leaf_bits;
    uint8_t leaf_qx[48];
    uint8_t leaf_qy[48];
    unsigned int rounds;
    int r;
    int stage;

    if (!conn)
        return -1;
    memset(&hello, 0, sizeof(hello));
    memset(&stream, 0, sizeof(stream));
    leaf_n = NULL;
    leaf_e = NULL;
    leaf_n_len = 0;
    leaf_e_len = 0;
    leaf_type = 0;
    leaf_bits = 0;
    memset(leaf_qx, 0, sizeof(leaf_qx));
    memset(leaf_qy, 0, sizeof(leaf_qy));
    memset(shared, 0, sizeof(shared));
    stage = 0;

    if (lebtls_build_client_hello(conn, &hello) < 0)
        goto fail;
    if (lebtls_transcript_append(conn, hello.buf, hello.len) < 0)
        goto fail;
    if (lebtls_rec_send(conn->tcp, LEBTLS_RT_HANDSHAKE, hello.buf,
                        hello.len) < 0)
        goto fail;
    hs_free(&hello);

    r = lebtls_rec_recv(conn->tcp, &type, &rec, &rec_len, 15000);
    if (r <= 0)
        goto fail;
    if (type != LEBTLS_RT_HANDSHAKE) {
        kfree(rec);
        goto fail;
    }
    if (rec_len < 4 || rec[0] != 2) {
        kfree(rec);
        goto fail;
    }
    {
        uint64_t sh_len = ((uint64_t)rec[1] << 16) |
                          ((uint64_t)rec[2] << 8) | rec[3];
        if (4 + sh_len != rec_len) {
            kfree(rec);
            goto fail;
        }
        if (parse_server_hello(rec, rec_len, peer_key) < 0) {
            kfree(rec);
            goto fail;
        }
        if (lebtls_transcript_append(conn, rec, rec_len) < 0) {
            kfree(rec);
            goto fail;
        }
        kfree(rec);
    }
    if (lebtls_x25519_shared(conn->eph_priv, peer_key, shared) < 0)
        goto fail;
    memset(peer_key, 0, sizeof(peer_key));

    memset(zeros, 0, sizeof(zeros));
    if (lebtls_hkdf_extract(NULL, 0, zeros, sizeof(zeros), early) < 0)
        goto fail;
    if (lebtls_hkdf_label(early, "derived", empty_hash, sizeof(empty_hash),
                          derived, 32) < 0)
        goto fail;
    if (lebtls_hkdf_extract(derived, sizeof(derived), shared,
                            sizeof(shared), hs_secret) < 0)
        goto fail;
    memset(shared, 0, sizeof(shared));
    memset(derived, 0, sizeof(derived));
    if (transcript_hash(conn, th) < 0)
        goto fail;
    {
        uint8_t secret[32];

        if (lebtls_hkdf_label(hs_secret, "s hs traffic", th, sizeof(th),
                              secret, sizeof(secret)) < 0)
            goto fail;
        if (lebtls_hkdf_label(secret, "key", NULL, 0, conn->hs_key_rx, 16) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        if (lebtls_hkdf_label(secret, "iv", NULL, 0, conn->hs_iv_rx, 12) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        if (lebtls_hkdf_label(secret, "finished", NULL, 0, fin_key,
                              sizeof(fin_key)) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        memset(secret, 0, sizeof(secret));
        if (lebtls_hkdf_label(hs_secret, "c hs traffic", th, sizeof(th),
                              secret, sizeof(secret)) < 0)
            goto fail;
        if (lebtls_hkdf_label(secret, "key", NULL, 0, conn->hs_key_tx, 16) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        if (lebtls_hkdf_label(secret, "iv", NULL, 0, conn->hs_iv_tx, 12) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        if (lebtls_hkdf_label(secret, "finished", NULL, 0, cfin_key,
                              sizeof(cfin_key)) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        memset(secret, 0, sizeof(secret));
    }
    memset(conn->eph_priv, 0, sizeof(conn->eph_priv));

    rounds = 0;
    stage = 0;
    {
        unsigned int idle_count;

        idle_count = 0;
        for (;;) {
        uint8_t inner;
        uint8_t *pt;
        uint64_t pt_len;

        if (rounds >= 128)
            goto fail;
        rounds++;
        if (task_has_pending_signals())
            goto fail;
        r = lebtls_rec_recv(conn->tcp, &type, &rec, &rec_len, 15000);
        if (r < 0)
            goto fail;
        if (r == 0) {
            idle_count++;
            if (idle_count > 3)
                goto fail;
            continue;
        }
        idle_count = 0;
        if (type == 0x14) {
            kfree(rec);
            continue;
        }
        if (type != LEBTLS_RT_APP_DATA) {
            kfree(rec);
            goto fail;
        }
        pt = NULL;
        pt_len = 0;
        if (lebtls_traffic_open(&conn->hs_read_seq, conn->hs_key_rx,
                                conn->hs_iv_rx, rec, rec_len, &pt, &pt_len,
                                &inner) < 0) {
            kfree(rec);
            goto fail;
        }
        kfree(rec);
        if (inner != LEBTLS_RT_HANDSHAKE) {
            if (pt)
                kfree(pt);
            goto fail;
        }
        if (hs_append_record(conn, &stream, pt, pt_len) < 0) {
            if (pt)
                kfree(pt);
            goto fail;
        }
        if (pt)
            kfree(pt);
        for (;;) {
            r = hs_next_message(&stream, &type, &msg_body, &msg_len);
            if (r < 0)
                goto fail;
            if (r > 0)
                break;
            if (stage == 0 && type != 8)
                goto fail;
            if (stage == 1 && type != 11)
                goto fail;
            if (stage == 2 && type != 15)
                goto fail;
            if (stage == 3 && type != 20)
                goto fail;
            if (stage > 3)
                goto fail;
            if (stage == 0) {
                if (hs_consume(&stream, msg_len) < 0)
                    goto fail;
                stage = 1;
                continue;
            }
            if (stage == 1) {
                if (lebtls_verify_server_certs(msg_body, (size_t)msg_len,
                                               conn->host, &leaf_type,
                                               &leaf_bits, &leaf_n,
                                               &leaf_n_len, &leaf_e,
                                               &leaf_e_len, leaf_qx,
                                               leaf_qy) < 0)
                    goto fail;
                if (hs_consume(&stream, msg_len) < 0)
                    goto fail;
                stage = 2;
                continue;
            }
            if (stage == 2) {
                uint8_t *content;
                size_t content_len;
                uint8_t cvhash[32];
                uint8_t *sig;
                uint64_t sig_len;
                uint16_t cv_alg;

                if (msg_len < 4)
                    goto fail;
                cv_alg = (uint16_t)(((uint16_t)msg_body[0] << 8) | msg_body[1]);
                if (cv_alg != 0x0804 && cv_alg != 0x0403 && cv_alg != 0x0503)
                    goto fail;
                sig_len = ((uint64_t)msg_body[2] << 8) | msg_body[3];
                if (sig_len + 4 != msg_len)
                    goto fail;
                sig = (uint8_t *)msg_body + 4;
                if (conn->transcript_len < stream.len ||
                    transcript_hash_n(conn, conn->transcript_len - stream.len,
                                      cvhash) < 0)
                    goto fail;
                content_len = 64 + sizeof(cert_verify_ctx) - 1 + 1 + 32;
                content = kmalloc(content_len);
                if (!content)
                    goto fail;
                memset(content, 0x20, 64);
                memcpy(content + 64, cert_verify_ctx,
                       sizeof(cert_verify_ctx) - 1);
                content[64 + sizeof(cert_verify_ctx) - 1] = 0x00;
                memcpy(content + 64 + sizeof(cert_verify_ctx), cvhash, 32);
                memset(cvhash, 0, sizeof(cvhash));
                if (cv_alg == 0x0804) {
                    if (leaf_type != 1) {
                        memset(content, 0, content_len);
                        kfree(content);
                        goto fail;
                    }
                    r = lebtls_rsa_pss_verify_msg(leaf_n, leaf_n_len, leaf_e,
                                                  leaf_e_len, content,
                                                  content_len, sig,
                                                  (size_t)sig_len);
                } else {
                    uint8_t rr[48];
                    uint8_t ss[48];
                    size_t coord;
                    int ec_ok;

                    ec_ok = leaf_type == 2 &&
                            ((cv_alg == 0x0403 && leaf_bits == 256) ||
                             (cv_alg == 0x0503 && leaf_bits == 384));
                    if (ec_ok) {
                        uint8_t digest[48];

                        coord = leaf_bits == 384 ? 48 : 32;
                        r = lebtls_ecdsa_sig_parse(sig, (size_t)sig_len, rr,
                                                   ss, coord);
                        if (r == 0) {
                            if (leaf_bits == 384) {
                                sha384_hash(content, content_len, digest);
                                r = lebtls_ecdsa384_verify(leaf_qx, leaf_qy,
                                                           digest,
                                                           sizeof(digest), rr,
                                                           ss);
                            } else {
                                sha256_hash(content, content_len, digest);
                                r = lebtls_ecdsa_verify(leaf_qx, leaf_qy,
                                                        digest, 32, rr, ss);
                            }
                            memset(digest, 0, sizeof(digest));
                        }
                    } else {
                        r = -1;
                    }
                    memset(rr, 0, sizeof(rr));
                    memset(ss, 0, sizeof(ss));
                }
                memset(content, 0, content_len);
                kfree(content);
                if (r < 0)
                    goto fail;
                if (leaf_n) {
                    memset(leaf_n, 0, leaf_n_len);
                    kfree(leaf_n);
                    leaf_n = NULL;
                }
                if (leaf_e) {
                    memset(leaf_e, 0, leaf_e_len);
                    kfree(leaf_e);
                    leaf_e = NULL;
                }
                memset(leaf_qx, 0, sizeof(leaf_qx));
                memset(leaf_qy, 0, sizeof(leaf_qy));
                if (hs_consume(&stream, msg_len) < 0)
                    goto fail;
                stage = 3;
                continue;
            }
            {
                uint8_t expect[32];
                uint8_t fh[32];

                if (msg_len != 32)
                    goto fail;
                if (conn->transcript_len < stream.len ||
                    transcript_hash_n(conn, conn->transcript_len - stream.len,
                                      fh) < 0)
                    goto fail;
                hmac_sha256(fin_key, sizeof(fin_key), fh, sizeof(fh), expect);
                memset(fh, 0, sizeof(fh));
                if (crypto_constant_compare(expect, msg_body, 32) != 0) {
                    memset(expect, 0, sizeof(expect));
                    goto fail;
                }
                memset(expect, 0, sizeof(expect));
                if (hs_consume(&stream, msg_len) < 0)
                    goto fail;
                stage = 4;
                break;
            }
        }
        if (stage == 4)
            break;
    }
    }
    {
        uint64_t fin_end;

        if (conn->transcript_len < stream.len)
            goto fail;
        fin_end = conn->transcript_len - stream.len;
        hs_free(&stream);
        if (transcript_hash_n(conn, fin_end, th) < 0)
            goto fail;
        uint8_t cf[32];
        uint8_t secret[32];
        uint8_t master[32];
        uint8_t *fin;
        uint64_t fin_len;
        hs_buf_t fm;

        if (lebtls_hkdf_label(hs_secret, "derived", empty_hash,
                              sizeof(empty_hash), derived, 32) < 0)
            goto fail;
        if (lebtls_hkdf_extract(derived, sizeof(derived), zeros,
                                sizeof(zeros), master) < 0) {
            memset(derived, 0, sizeof(derived));
            goto fail;
        }
        memset(derived, 0, sizeof(derived));
        if (lebtls_hkdf_label(master, "c ap traffic", th, sizeof(th),
                              secret, sizeof(secret)) < 0) {
            memset(master, 0, sizeof(master));
            goto fail;
        }
        if (lebtls_hkdf_label(secret, "key", NULL, 0, conn->app_key_tx, 16) < 0 ||
            lebtls_hkdf_label(secret, "iv", NULL, 0, conn->app_iv_tx, 12) < 0) {
            memset(secret, 0, sizeof(secret));
            memset(master, 0, sizeof(master));
            goto fail;
        }
        memset(secret, 0, sizeof(secret));
        if (lebtls_hkdf_label(master, "s ap traffic", th, sizeof(th),
                              secret, sizeof(secret)) < 0) {
            memset(master, 0, sizeof(master));
            goto fail;
        }
        memset(master, 0, sizeof(master));
        if (lebtls_hkdf_label(secret, "key", NULL, 0, conn->app_key_rx, 16) < 0 ||
            lebtls_hkdf_label(secret, "iv", NULL, 0, conn->app_iv_rx, 12) < 0) {
            memset(secret, 0, sizeof(secret));
            goto fail;
        }
        memset(secret, 0, sizeof(secret));

        hmac_sha256(cfin_key, sizeof(cfin_key), th, sizeof(th), cf);
        memset(cfin_key, 0, sizeof(cfin_key));
        memset(&fm, 0, sizeof(fm));
        if (hs_u8(&fm, 20) < 0 || hs_u8(&fm, 0) < 0 ||
            hs_u8(&fm, 0) < 0 || hs_u8(&fm, 32) < 0 ||
            hs_bytes(&fm, cf, sizeof(cf)) < 0) {
            memset(cf, 0, sizeof(cf));
            hs_free(&fm);
            goto fail;
        }
        memset(cf, 0, sizeof(cf));
        fin = NULL;
        fin_len = 0;
        if (lebtls_traffic_seal(&conn->hs_write_seq, conn->hs_key_tx,
                                conn->hs_iv_tx, LEBTLS_RT_HANDSHAKE,
                                fm.buf, fm.len, &fin, &fin_len) < 0) {
            hs_free(&fm);
            goto fail;
        }
        if (lebtls_transcript_append(conn, fm.buf, fm.len) < 0) {
            hs_free(&fm);
            kfree(fin);
            goto fail;
        }
        hs_free(&fm);
        if (tcp_send(conn->tcp, fin, fin_len) < 0) {
            kfree(fin);
            goto fail;
        }
        kfree(fin);
    }
    memset(conn->hs_key_tx, 0, sizeof(conn->hs_key_tx));
    memset(conn->hs_key_rx, 0, sizeof(conn->hs_key_rx));
    memset(conn->hs_iv_tx, 0, sizeof(conn->hs_iv_tx));
    memset(conn->hs_iv_rx, 0, sizeof(conn->hs_iv_rx));
    memset(early, 0, sizeof(early));
    memset(hs_secret, 0, sizeof(hs_secret));
    memset(zeros, 0, sizeof(zeros));
    memset(th, 0, sizeof(th));
    memset(cfin_key, 0, sizeof(cfin_key));
    if (conn->transcript) {
        memset(conn->transcript, 0, conn->transcript_len);
        kfree(conn->transcript);
        conn->transcript = NULL;
        conn->transcript_len = 0;
        conn->transcript_cap = 0;
    }
    if (conn->host) {
        kfree(conn->host);
        conn->host = NULL;
    }
    conn->handshake_done = 1;
    return 0;

fail:
    hs_free(&hello);
    hs_free(&stream);
    if (leaf_n) {
        memset(leaf_n, 0, leaf_n_len);
        kfree(leaf_n);
    }
    if (leaf_e) {
        memset(leaf_e, 0, leaf_e_len);
        kfree(leaf_e);
    }
    memset(leaf_qx, 0, sizeof(leaf_qx));
    memset(leaf_qy, 0, sizeof(leaf_qy));
    memset(shared, 0, sizeof(shared));
    memset(early, 0, sizeof(early));
    memset(derived, 0, sizeof(derived));
    memset(hs_secret, 0, sizeof(hs_secret));
    memset(zeros, 0, sizeof(zeros));
    memset(th, 0, sizeof(th));
    memset(fin_key, 0, sizeof(fin_key));
    memset(cfin_key, 0, sizeof(cfin_key));
    memset(conn->hs_key_tx, 0, sizeof(conn->hs_key_tx));
    memset(conn->hs_key_rx, 0, sizeof(conn->hs_key_rx));
    memset(conn->hs_iv_tx, 0, sizeof(conn->hs_iv_tx));
    memset(conn->hs_iv_rx, 0, sizeof(conn->hs_iv_rx));
    memset(conn->eph_priv, 0, sizeof(conn->eph_priv));
    return -1;
}

#include <lebirun/lke.h>
#include <lebirun/mem_map.h>
#include <lebirun/drivers/net/tls.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/tty.h>
#include <lebirun/task.h>
#include <string.h>
#include "tls_int.h"

static uint64_t lebtls_active_conns;

void lebtls_conn_free(tls_conn_t *conn) {
    if (!conn)
        return;
    memset(conn->hs_key_tx, 0, sizeof(conn->hs_key_tx));
    memset(conn->hs_key_rx, 0, sizeof(conn->hs_key_rx));
    memset(conn->hs_iv_tx, 0, sizeof(conn->hs_iv_tx));
    memset(conn->hs_iv_rx, 0, sizeof(conn->hs_iv_rx));
    memset(conn->app_key_tx, 0, sizeof(conn->app_key_tx));
    memset(conn->app_key_rx, 0, sizeof(conn->app_key_rx));
    memset(conn->app_iv_tx, 0, sizeof(conn->app_iv_tx));
    memset(conn->app_iv_rx, 0, sizeof(conn->app_iv_rx));
    memset(conn->eph_priv, 0, sizeof(conn->eph_priv));
    if (conn->transcript) {
        memset(conn->transcript, 0, conn->transcript_len);
        kfree(conn->transcript);
    }
    if (conn->stash) {
        memset(conn->stash, 0, conn->stash_len);
        kfree(conn->stash);
    }
    if (conn->host)
        kfree(conn->host);
    __sync_fetch_and_sub(&lebtls_active_conns, 1);
    kfree(conn);
}

static tls_conn_t *lebtls_connect(tcp_socket_t *tcp, const char *host) {
    tls_conn_t *conn;
    size_t host_len;

    if (!tcp || !host)
        return NULL;
    conn = kmalloc(sizeof(*conn));
    if (!conn)
        return NULL;
    memset(conn, 0, sizeof(*conn));
    host_len = strlen(host);
    conn->host = kmalloc(host_len + 1);
    if (!conn->host) {
        kfree(conn);
        return NULL;
    }
    memcpy(conn->host, host, host_len + 1);
    conn->tcp = tcp;
    __sync_fetch_and_add(&lebtls_active_conns, 1);
    if (lebtls_handshake(conn) < 0) {
        lebtls_conn_free(conn);
        return NULL;
    }
    return conn;
}

static int lebtls_stash_drain(tls_conn_t *conn, uint8_t *buf, uint64_t len) {
    uint64_t take;

    if (!conn || !buf || conn->stash_len == 0)
        return 0;
    take = conn->stash_len < len ? conn->stash_len : len;
    memcpy(buf, conn->stash, (size_t)take);
    conn->stash_len -= take;
    if (conn->stash_len > 0)
        memmove(conn->stash, conn->stash + take, (size_t)conn->stash_len);
    return (int)take;
}

static int lebtls_stash_store(tls_conn_t *conn, const uint8_t *data, uint64_t len) {
    uint8_t *grown;
    uint64_t need;
    uint64_t cap;

    if (len == 0)
        return 0;
    need = conn->stash_len + len;
    if (need < conn->stash_len)
        return -1;
    if (need > conn->stash_cap) {
        cap = conn->stash_cap ? conn->stash_cap : 4096;
        while (cap < need) {
            if (cap > UINT64_MAX / 2)
                return -1;
            cap *= 2;
        }
        grown = krealloc(conn->stash, (size_t)cap);
        if (!grown)
            return -1;
        conn->stash = grown;
        conn->stash_cap = cap;
    }
    memcpy(conn->stash + conn->stash_len, data, (size_t)len);
    conn->stash_len = need;
    return 0;
}

static int lebtls_send(tls_conn_t *conn, const uint8_t *data, uint64_t len) {
    uint64_t off;
    uint64_t chunk;

    if (!conn || !data)
        return -1;
    if (!conn->handshake_done)
        return -1;
    off = 0;
    while (off < len) {
        uint8_t *rec;
        uint64_t rec_len;

        chunk = len - off;
        if (chunk > 16384)
            chunk = 16384;
        rec = NULL;
        rec_len = 0;
        if (lebtls_traffic_seal(&conn->app_write_seq, conn->app_key_tx,
                                conn->app_iv_tx, LEBTLS_RT_APP_DATA,
                                data + off, chunk, &rec, &rec_len) < 0)
            return -1;
        if (tcp_send(conn->tcp, rec, rec_len) < 0) {
            kfree(rec);
            return -1;
        }
        kfree(rec);
        off += chunk;
    }
    return (int)len;
}

static int lebtls_recv(tls_conn_t *conn, uint8_t *buf, uint64_t len, uint64_t timeout_ms) {
    uint8_t type;
    uint8_t *rec;
    uint64_t rec_len;
    int drained;
    int r;

    if (!conn || !buf || len == 0)
        return -1;
    if (!conn->handshake_done)
        return -1;
    drained = lebtls_stash_drain(conn, buf, len);
    if (drained > 0)
        return drained;
    for (;;) {
        uint8_t *pt;
        uint64_t pt_len;
        uint8_t inner;

        r = lebtls_rec_recv(conn->tcp, &type, &rec, &rec_len, timeout_ms);
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        if (type != LEBTLS_RT_APP_DATA) {
            kfree(rec);
            return -1;
        }
        pt = NULL;
        pt_len = 0;
        if (lebtls_traffic_open(&conn->app_read_seq, conn->app_key_rx,
                                conn->app_iv_rx, rec, rec_len, &pt, &pt_len,
                                &inner) < 0) {
            kfree(rec);
            return -1;
        }
        kfree(rec);
        if (inner == LEBTLS_RT_HANDSHAKE) {
            r = lebtls_skip_post_handshake(pt, pt_len);
            memset(pt, 0, pt_len > 0 ? (size_t)pt_len : 1);
            kfree(pt);
            if (r < 0)
                return -1;
            continue;
        }
        if (inner != LEBTLS_RT_APP_DATA) {
            memset(pt, 0, pt_len > 0 ? (size_t)pt_len : 1);
            kfree(pt);
            return -1;
        }
        if (pt_len > len) {
            memcpy(buf, pt, (size_t)len);
            if (lebtls_stash_store(conn, pt + len, pt_len - len) < 0) {
                memset(pt, 0, (size_t)pt_len);
                kfree(pt);
                return -1;
            }
            memset(pt, 0, (size_t)pt_len);
            kfree(pt);
            return (int)len;
        }
        if (pt_len > 0)
            memcpy(buf, pt, (size_t)pt_len);
        memset(pt, 0, pt_len > 0 ? (size_t)pt_len : 1);
        kfree(pt);
        return (int)pt_len;
    }
}

static void lebtls_close(tls_conn_t *conn) {
    if (!conn)
        return;
    lebtls_conn_free(conn);
}

static const tls_provider_t lebtls_provider = {
    lebtls_connect,
    lebtls_send,
    lebtls_recv,
    lebtls_close,
};

static int lebtls_init(void) {
    if (lebtls_hkdf_selftest() < 0)
        return -1;
    if (lebtls_x25519_selftest() < 0)
        return -1;
    if (lebtls_p256_selftest() < 0)
        return -1;
    if (lebtls_p384_selftest() < 0)
        return -1;
    if (lebtls_aead_selftest() < 0)
        return -1;
    tls_register_provider(&lebtls_provider);
    return 0;
}

static void lebtls_exit(void) {
    while (lebtls_active_conns > 0)
        schedule();
    tls_unregister_provider(&lebtls_provider);
}

LKE_NAME("lebtls");
LKE_DESC("TLS 1.3 client for HTTPS");
LKE_LICENSE("GPLv2");
LKE_VERSION_STR("0.1.0");

module_init(lebtls_init);
module_exit(lebtls_exit);

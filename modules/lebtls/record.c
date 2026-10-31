#include <lebirun/mem_map.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/task.h>
#include <string.h>
#include "tls_int.h"

#define LEBTLS_REC_HEADER_LEN 5
#define LEBTLS_REC_MAX_PLAINTEXT 16384

int lebtls_rec_send(tcp_socket_t *tcp, uint8_t type, const uint8_t *data, uint64_t len) {
    uint8_t *rec;
    uint64_t total;

    if (!tcp)
        return -1;
    if (len > LEBTLS_REC_MAX_PLAINTEXT)
        return -1;
    if (len > 0 && !data)
        return -1;
    total = LEBTLS_REC_HEADER_LEN + len;
    rec = kmalloc((size_t)total);
    if (!rec)
        return -1;
    rec[0] = type;
    rec[1] = 0x03;
    rec[2] = 0x03;
    rec[3] = (uint8_t)((len >> 8) & 0xFF);
    rec[4] = (uint8_t)(len & 0xFF);
    if (len > 0)
        memcpy(rec + LEBTLS_REC_HEADER_LEN, data, (size_t)len);
    if (tcp_send(tcp, rec, total) < 0) {
        kfree(rec);
        return -1;
    }
    kfree(rec);
    return 0;
}

static int lebtls_recv_fill(tcp_socket_t *tcp, uint8_t *buf, uint64_t len,
                            uint64_t *got, uint64_t timeout_ms) {
    uint64_t rounds;
    uint64_t max_rounds;
    int n;

    rounds = 0;
    max_rounds = timeout_ms / 1000 + 2;
    while (*got < len) {
        if (task_has_pending_signals())
            return -1;
        n = tcp_recv(tcp, buf + *got, len - *got, 1000, 0);
        if (n < 0)
            return -1;
        if (n == 0) {
            if (tcp->state == TCP_STATE_CLOSE_WAIT || tcp->state == TCP_STATE_CLOSED)
                return -1;
            if (++rounds >= max_rounds)
                return 0;
            continue;
        }
        rounds = 0;
        *got += (uint64_t)n;
    }
    return 1;
}

int lebtls_rec_recv(tls_conn_t *conn, uint8_t *type_out, uint8_t **data_out,
                    uint64_t *len_out, uint64_t timeout_ms) {
    uint64_t len;
    uint64_t body;
    int r;

    if (!conn || !conn->tcp || !type_out || !data_out || !len_out)
        return -1;
    if (conn->rec_got < LEBTLS_REC_HEADER_LEN) {
        r = lebtls_recv_fill(conn->tcp, conn->rec_hdr, LEBTLS_REC_HEADER_LEN,
                             &conn->rec_got, timeout_ms);
        if (r <= 0)
            return r;
        len = ((uint64_t)conn->rec_hdr[3] << 8) | conn->rec_hdr[4];
        if (len > LEBTLS_REC_MAX_PLAINTEXT + 256)
            return -1;
        if (len > 0) {
            conn->rec_buf = kmalloc((size_t)len);
            if (!conn->rec_buf)
                return -1;
        }
    }
    len = ((uint64_t)conn->rec_hdr[3] << 8) | conn->rec_hdr[4];
    body = conn->rec_got - LEBTLS_REC_HEADER_LEN;
    r = lebtls_recv_fill(conn->tcp, conn->rec_buf, len, &body, timeout_ms);
    conn->rec_got = LEBTLS_REC_HEADER_LEN + body;
    if (r <= 0)
        return r;
    *type_out = conn->rec_hdr[0];
    *data_out = conn->rec_buf;
    *len_out = len;
    conn->rec_buf = NULL;
    conn->rec_got = 0;
    return 1;
}

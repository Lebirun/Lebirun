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

static int lebtls_recv_exact(tcp_socket_t *tcp, uint8_t *buf, uint64_t len,
                             uint64_t timeout_ms) {
    uint64_t got;
    uint64_t rounds;
    uint64_t max_rounds;
    int n;

    got = 0;
    rounds = 0;
    max_rounds = timeout_ms / 1000 + 2;
    while (got < len) {
        if (task_has_pending_signals())
            return -1;
        n = tcp_recv(tcp, buf + got, len - got, 1000, 0);
        if (n < 0)
            return -1;
        if (n == 0) {
            rounds++;
            if (rounds >= max_rounds)
                return -1;
            continue;
        }
        rounds = 0;
        got += (uint64_t)n;
    }
    return 0;
}

int lebtls_rec_recv(tcp_socket_t *tcp, uint8_t *type_out, uint8_t **data_out,
                    uint64_t *len_out, uint64_t timeout_ms) {
    uint8_t hdr[LEBTLS_REC_HEADER_LEN];
    uint64_t len;
    uint8_t *data;

    if (!tcp || !type_out || !data_out || !len_out)
        return -1;
    if (lebtls_recv_exact(tcp, hdr, LEBTLS_REC_HEADER_LEN, timeout_ms) < 0)
        return 0;
    len = ((uint64_t)hdr[3] << 8) | hdr[4];
    if (len > LEBTLS_REC_MAX_PLAINTEXT + 256)
        return -1;
    if (len == 0) {
        *data_out = NULL;
        *len_out = 0;
        *type_out = hdr[0];
        return 1;
    }
    data = kmalloc((size_t)len);
    if (!data)
        return -1;
    if (lebtls_recv_exact(tcp, data, len, timeout_ms) < 0) {
        kfree(data);
        return -1;
    }
    *type_out = hdr[0];
    *data_out = data;
    *len_out = len;
    return 1;
}

#ifndef TLS_H
#define TLS_H

#include <lebirun/drivers/net/net_types.h>
#include <lebirun/drivers/net/tcp.h>

typedef struct tls_conn tls_conn_t;

typedef struct tls_provider {
    tls_conn_t *(*connect)(tcp_socket_t *tcp, const char *host);
    int (*send)(tls_conn_t *conn, const uint8_t *data, uint64_t len);
    int (*recv)(tls_conn_t *conn, uint8_t *buf, uint64_t len, uint64_t timeout_ms);
    void (*close)(tls_conn_t *conn);
} tls_provider_t;

void tls_register_provider(const tls_provider_t *ops);
void tls_unregister_provider(const tls_provider_t *ops);
const tls_provider_t *tls_get_provider(void);

#endif

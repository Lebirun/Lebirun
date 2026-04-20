#ifndef TLS_H
#define TLS_H

#include <lebirun/drivers/net/net_types.h>
#include <lebirun/drivers/net/tcp.h>

typedef struct tls_conn tls_conn_t;

void tls_init(void);
tls_conn_t *tls_connect(tcp_socket_t *tcp, const char *host);
int tls_send(tls_conn_t *conn, const uint8_t *data, uint64_t len);
int tls_recv(tls_conn_t *conn, uint8_t *buf, uint64_t len, uint64_t timeout_ms);
void tls_close(tls_conn_t *conn);
int tls_load_ca_certs(const uint8_t *pem, size_t pem_len);

#endif

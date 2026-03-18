#ifndef TCP_H
#define TCP_H

#include <kernel/drivers/net/net_types.h>

void tcp_init(void);
void tcp_receive(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint64_t len);
void tcp_tick(void);

tcp_socket_t *tcp_socket_create(void);
void tcp_socket_close(tcp_socket_t *sock);
int tcp_connect(tcp_socket_t *sock, ipv4_addr_t dest, uint16_t port, uint64_t timeout_ms);
int tcp_send(tcp_socket_t *sock, uint8_t *data, uint64_t len);
int tcp_recv(tcp_socket_t *sock, uint8_t *buffer, uint64_t len, uint64_t timeout_ms);
int tcp_disconnect(tcp_socket_t *sock, uint64_t timeout_ms);

#endif

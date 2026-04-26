#ifndef UDP_H
#define UDP_H

#include <lebirun/drivers/net/net_types.h>

void udp_init(void);
void udp_receive(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint64_t len);
int udp_send(netif_t *netif, ipv4_addr_t dest, uint16_t src_port, uint16_t dest_port, uint8_t *data, uint64_t len);
int udp_send_from(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint16_t src_port, uint16_t dest_port, uint8_t *data, uint64_t len);
int udp_send6(netif_t *netif, ipv6_addr_t dest, uint16_t src_port, uint16_t dest_port, uint8_t *data, uint64_t len);
void udp_receive6(netif_t *netif, ipv6_addr_t src, ipv6_addr_t dest, uint8_t *data, uint64_t len);

udp_socket_t *udp_socket_create(uint16_t port);
void udp_socket_close(udp_socket_t *sock);
int udp_socket_send(udp_socket_t *sock, ipv4_addr_t dest, uint16_t port, uint8_t *data, uint64_t len);
int udp_socket_recv(udp_socket_t *sock, uint8_t *buffer, uint64_t len, ipv4_addr_t *from_ip, uint16_t *from_port, uint64_t timeout_ms);

#endif

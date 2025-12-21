#ifndef IPV6_H
#define IPV6_H

#include <kernel/drivers/net/net_types.h>

void ipv6_init(void);
void ipv6_receive(netif_t *netif, uint8_t *packet, uint32_t len);
int ipv6_send(netif_t *netif, ipv6_addr_t dest, uint8_t next_header, uint8_t *data, uint32_t len);
void ipv6_create_link_local(mac_addr_t mac, ipv6_addr_t *out);
uint16_t ipv6_checksum(ipv6_addr_t *src, ipv6_addr_t *dest, uint8_t next_header, uint8_t *data, uint32_t len);

#endif

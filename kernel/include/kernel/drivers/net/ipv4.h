#ifndef IPV4_H
#define IPV4_H

#include <kernel/drivers/net/net_types.h>

void ipv4_init(void);
void ipv4_receive(netif_t *netif, uint8_t *packet, uint32_t len);
int ipv4_send(netif_t *netif, ipv4_addr_t dest, uint8_t protocol, uint8_t *data, uint32_t len);
uint16_t ipv4_checksum(void *data, uint32_t len);
int ipv4_is_local(netif_t *netif, ipv4_addr_t ip);

#endif

#ifndef ICMPV6_H
#define ICMPV6_H

#include <kernel/drivers/net/net_types.h>

void icmpv6_receive(netif_t *netif, ipv6_addr_t *src, uint8_t *data, uint64_t len);
int icmpv6_send_echo_request(netif_t *netif, ipv6_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint64_t len);
int icmpv6_send_neighbor_solicitation(netif_t *netif, ipv6_addr_t target);
int icmpv6_send_router_solicitation(netif_t *netif);

#endif

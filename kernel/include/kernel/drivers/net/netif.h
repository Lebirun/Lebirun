#ifndef NETIF_H
#define NETIF_H

#include <kernel/drivers/net/net_types.h>

void netif_init(void);
netif_t *netif_alloc(void);
void netif_register(netif_t *netif);
netif_t *netif_get_default(void);
netif_t *netif_find(const char *name);
void netif_set_default(netif_t *netif);
void netif_set_ipv4(netif_t *netif, ipv4_addr_t ip, ipv4_addr_t netmask, ipv4_addr_t gateway);
void netif_set_dns(netif_t *netif, ipv4_addr_t dns1, ipv4_addr_t dns2);
void netif_set_ipv6(netif_t *netif, ipv6_addr_t ip, uint8_t prefix, ipv6_addr_t gateway);
int netif_send(netif_t *netif, uint8_t *data, uint32_t len);
void netif_poll_all(void);
void netif_print_info(netif_t *netif);

#endif

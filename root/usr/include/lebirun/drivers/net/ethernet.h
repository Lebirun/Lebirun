#ifndef ETHERNET_H
#define ETHERNET_H

#include <lebirun/drivers/net/net_types.h>

int eth_send(netif_t *netif, mac_addr_t dest, uint16_t ethertype, uint8_t *data, uint64_t len);
void eth_receive(netif_t *netif, uint8_t *frame, uint64_t len);

#endif

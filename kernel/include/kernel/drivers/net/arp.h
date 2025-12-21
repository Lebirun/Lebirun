#ifndef ARP_H
#define ARP_H

#include <kernel/drivers/net/net_types.h>

void arp_init(void);
void arp_receive(netif_t *netif, arp_packet_t *arp);
int arp_resolve(netif_t *netif, ipv4_addr_t ip, mac_addr_t *mac_out);
void arp_request(netif_t *netif, ipv4_addr_t target_ip);
void arp_add_entry(ipv4_addr_t ip, mac_addr_t mac);
void arp_print_cache(void);
int arp_get_cache(uint32_t *ips, uint8_t *macs, int max_entries);

#endif

#ifndef NET_H
#define NET_H

#include <lebirun/drivers/net/net_types.h>
#include <lebirun/drivers/net/netif.h>
#include <lebirun/drivers/net/ethernet.h>
#include <lebirun/drivers/net/arp.h>
#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/ipv6.h>
#include <lebirun/drivers/net/icmp.h>
#include <lebirun/drivers/net/icmpv6.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/drivers/net/dhcp.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/drivers/net/http.h>

void net_init(void);
void net_ensure_hw(void);
void net_poll(void);
void net_tick(void);
uint64_t net_get_ticks(void);

#endif

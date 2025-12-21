#ifndef NET_H
#define NET_H

#include <kernel/drivers/net/net_types.h>
#include <kernel/drivers/net/netif.h>
#include <kernel/drivers/net/ethernet.h>
#include <kernel/drivers/net/arp.h>
#include <kernel/drivers/net/ipv4.h>
#include <kernel/drivers/net/ipv6.h>
#include <kernel/drivers/net/icmp.h>
#include <kernel/drivers/net/icmpv6.h>
#include <kernel/drivers/net/udp.h>
#include <kernel/drivers/net/tcp.h>
#include <kernel/drivers/net/dhcp.h>
#include <kernel/drivers/net/dns.h>
#include <kernel/drivers/net/http.h>

void net_init(void);
void net_poll(void);
void net_tick(void);
uint32_t net_get_ticks(void);

#endif

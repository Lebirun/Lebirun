#ifndef DHCP_H
#define DHCP_H

#include <kernel/drivers/net/net_types.h>

#define DHCP_STATE_INIT       0
#define DHCP_STATE_SELECTING  1
#define DHCP_STATE_REQUESTING 2
#define DHCP_STATE_BOUND      3
#define DHCP_STATE_RENEWING   4
#define DHCP_STATE_REBINDING  5

#define DHCP_RETRY_INTERVAL   1000
#define DHCP_MAX_RETRIES      5

typedef struct {
    uint8_t state;
    uint32_t xid;
    ipv4_addr_t offered_ip;
    ipv4_addr_t server_ip;
    ipv4_addr_t subnet_mask;
    ipv4_addr_t gateway;
    ipv4_addr_t dns1;
    ipv4_addr_t dns2;
    uint32_t lease_time;
    uint32_t t1_time;
    uint32_t t2_time;
    uint32_t lease_start;
    uint32_t last_send_time;
    uint32_t retries;
    netif_t *netif;
} dhcp_state_t;

void dhcp_init(netif_t *netif);
void dhcp_start(netif_t *netif);
void dhcp_stop(netif_t *netif);
void dhcp_receive(netif_t *netif, uint8_t *data, uint32_t len);
void dhcp_tick(void);
int dhcp_is_bound(netif_t *netif);
int dhcp_is_negotiating(void);

#endif

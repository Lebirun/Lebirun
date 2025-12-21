#ifndef ICMP_H
#define ICMP_H

#include <kernel/drivers/net/net_types.h>

void icmp_receive(netif_t *netif, ipv4_addr_t src, uint8_t *data, uint32_t len);
int icmp_send_echo_request(netif_t *netif, ipv4_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint32_t len);
int icmp_send_echo_reply(netif_t *netif, ipv4_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint32_t len);

typedef struct {
    ipv4_addr_t target;
    uint16_t id;
    uint16_t seq;
    uint8_t received;
    uint32_t rtt;
    uint32_t send_time;
} ping_state_t;

int ping(ipv4_addr_t target, uint32_t count, uint32_t timeout_ms);
int ping_one(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms);

#endif

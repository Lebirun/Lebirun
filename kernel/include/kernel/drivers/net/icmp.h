#ifndef ICMP_H
#define ICMP_H

#include <kernel/drivers/net/net_types.h>

void icmp_receive(netif_t *netif, ipv4_addr_t src, uint8_t *data, uint64_t len);
int icmp_send_echo_request(netif_t *netif, ipv4_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint64_t len);
int icmp_send_echo_reply(netif_t *netif, ipv4_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint64_t len);

typedef struct {
    ipv4_addr_t target;
    uint16_t id;
    uint16_t seq;
    uint8_t received;
    uint64_t rtt;
    uint64_t send_time;
} ping_state_t;

int ping(ipv4_addr_t target, uint64_t count, uint64_t timeout_ms);
int ping_one(ipv4_addr_t target, uint16_t seq, uint64_t timeout_ms);

#endif

#ifndef ICMP_H
#define ICMP_H

#include <lebirun/drivers/net/net_types.h>

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

typedef void (*icmp_error_hook_t)(uint8_t proto, uint16_t local_port,
                                  int error);
void icmp_register_error_hook(icmp_error_hook_t hook);

#define ICMP_ERR_MSGSIZE 90
#define ICMP_ERR_NET_UNREACH 101
#define ICMP_ERR_TIMEDOUT 110
#define ICMP_ERR_CONN_REFUSED 111
#define ICMP_ERR_HOST_UNREACH 113

#endif

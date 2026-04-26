#ifndef IPV67_H
#define IPV67_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/drivers/net/net_types.h>

#define IPV67_ZONE_MAX    6
#define IPV67_DOMAIN_MAX  32
#define IPV67_NODE_MAX    6

#define IPV67_ADDR_STR_MAX  (IPV67_ZONE_MAX + 1 + IPV67_ZONE_MAX + 1 + IPV67_DOMAIN_MAX + 1 + IPV67_NODE_MAX + 1 + IPV67_NODE_MAX + 1)

#define IPV67_PROTO_VERSION    1
#define IPV67_MAGIC            0x6937u

#define IPV67_TYPE_DATA        0x01
#define IPV67_TYPE_HELLO       0x02
#define IPV67_TYPE_ROUTE       0x03
#define IPV67_TYPE_PING        0x04
#define IPV67_TYPE_PONG        0x05
#define IPV67_TYPE_PEER_REQ    0x06
#define IPV67_TYPE_PEER_ACK    0x07

#define IPV67_PORT_DEFAULT     6767
#define IPV67_MAX_HOPS         16
#define IPV67_MAX_PEERS        64
#define IPV67_MAX_ROUTES       256
#define IPV67_MAX_CONTEXTS     64
#define IPV67_BLOCK_BITS       12

#define IPV67_PEER_IPV4        4
#define IPV67_PEER_IPV6        6

#define IPV67_ERR_OK           0
#define IPV67_ERR_INVAL       -1
#define IPV67_ERR_NOROUTE     -2
#define IPV67_ERR_NOPEER      -3
#define IPV67_ERR_TOOLONG     -4
#define IPV67_ERR_NOMEM       -5
#define IPV67_ERR_TIMEOUT     -6

typedef struct {
    char zone1[IPV67_ZONE_MAX + 1];
    char zone2[IPV67_ZONE_MAX + 1];
    char domain[IPV67_DOMAIN_MAX + 1];
    char node1[IPV67_NODE_MAX + 1];
    char node2[IPV67_NODE_MAX + 1];
} ipv67_addr_t;

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t payload_len;
    uint8_t  hop_limit;
    uint8_t  flags;
    char     src[IPV67_ADDR_STR_MAX];
    char     dst[IPV67_ADDR_STR_MAX];
} __attribute__((packed)) ipv67_header_t;

typedef struct {
    uint32_t ipv4;
    ipv6_addr_t ipv6;
    uint16_t port;
    ipv67_addr_t addr;
    uint8_t  family;
    uint8_t  active;
    uint32_t last_seen_ticks;
} ipv67_peer_t;

typedef struct {
    ipv67_addr_t dest;
    uint32_t     next_hop_ipv4;
    ipv6_addr_t  next_hop_ipv6;
    uint16_t     next_hop_port;
    uint8_t      next_hop_family;
    uint8_t      hops;
    uint8_t      valid;
    uint32_t     age_ticks;
} ipv67_route_t;

void        ipv67_init(void);
int         ipv67_addr_parse(const char *str, ipv67_addr_t *out);
int         ipv67_addr_format(const ipv67_addr_t *addr, char *buf, uint64_t bufsz);
int         ipv67_addr_eq(const ipv67_addr_t *a, const ipv67_addr_t *b);
void        ipv67_self_addr(ipv67_addr_t *out);
int         ipv67_set_self_addr(const ipv67_addr_t *addr);
int         ipv67_send(const ipv67_addr_t *dst, const uint8_t *data, uint64_t len);
void        ipv67_receive(uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len);
void        ipv67_receive_on_port(uint16_t local_port, uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len);
int         ipv67_add_peer(uint32_t ipv4, uint16_t port);
int         ipv67_add_peer_with_addr(uint32_t ipv4, uint16_t port, const ipv67_addr_t *addr);
int         ipv67_add_peer6(const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr);
int         ipv67_remove_peer(uint32_t ipv4, uint16_t port);
int         ipv67_remove_peer_by_addr(const ipv67_addr_t *addr);
int         ipv67_peer_count(void);
int         ipv67_get_peers(ipv67_peer_t *out, int max);
int         ipv67_route_count_get(void);
int         ipv67_get_routes(ipv67_route_t *out, int max);
int         ipv67_ping(const ipv67_addr_t *dst, uint32_t timeout_ms);
int         ipv67_probe_peers(void);
int         ipv67_set_port(uint16_t port);
uint16_t    ipv67_get_port(void);
void        ipv67_receive6(const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len);
void        ipv67_receive6_on_port(uint16_t local_port, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len);
int         ipv67_use_port(uint16_t port);
int         ipv67_port_active(uint16_t port);

#endif

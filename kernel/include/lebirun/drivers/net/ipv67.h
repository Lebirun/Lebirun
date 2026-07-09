#ifndef IPV67_H
#define IPV67_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/drivers/net/net_types.h>

#define IPV67_ZONE_MAX    6
#define IPV67_DOMAIN_MAX  32
#define IPV67_NODE_MAX    6

#define IPV67_ADDR_STR_MAX  (IPV67_ZONE_MAX + 1 + IPV67_ZONE_MAX + 1 + IPV67_DOMAIN_MAX + 1 + IPV67_NODE_MAX + 1 + IPV67_NODE_MAX + 1)

#define IPV67_PROTO_VERSION    3
#define IPV67_MAGIC            0x6937u

#define IPV67_TYPE_DATA        0x01
#define IPV67_TYPE_HELLO       0x02
#define IPV67_TYPE_ROUTE       0x03
#define IPV67_TYPE_PING        0x04
#define IPV67_TYPE_PONG        0x05
#define IPV67_TYPE_PEER_REQ    0x06
#define IPV67_TYPE_PEER_ACK    0x07
#define IPV67_TYPE_ROUTE_ADV   0x08
#define IPV67_TYPE_AUTH_HELLO  0x09
#define IPV67_TYPE_AUTH_REPLY  0x0a
#define IPV67_TYPE_AUTH_DONE   0x0b
#define IPV67_TYPE_ASN_ADV     0x0c
#define IPV67_TYPE_PUNCH       0x0f

#define IPV67_PORT_DEFAULT     6767
#define IPV67_MAX_HOPS         16
#define IPV67_BLOCK_BITS       12
#define IPV67_AUTH_KEY_SIZE    32
#define IPV67_IDENTITY_SIZE    32
#define IPV67_SESSION_KEY_SIZE 32
#define IPV67_SIG_SIZE         32
#define IPV67_ALIAS_SIZE       16
#define IPV67_REPLAY_BITS      64
#define IPV67_ROUTE_TTL_TICKS  180000
#define IPV67_PEER_TTL_TICKS   240000
#define IPV67_ROUTE_METRIC_MAX 255
#define IPV67_ASN_NAME_SIZE    32
#define IPV67_ASN_LABEL_SIZE   32
#define IPV67_ASN_COUNTRY_SIZE 3
#define IPV67_ASN_SOURCE_SIZE  16
#define IPV67_ASN_FLAG_LOCAL   0x01
#define IPV67_ASN_FLAG_BROAD   0x02
#define IPV67_ASN_FLAG_CONFLICT 0x04
#define IPV67_ASN_FLAG_AUTH    0x08
#define IPV67_ASN_FLAG_RELAY   0x10
#define IPV67_ASN_FLAG_QUARANTINE 0x20

#define IPV67_PEER_IPV4        4
#define IPV67_PEER_IPV6        6

#define IPV67_ERR_OK           0
#define IPV67_ERR_INVAL       -1
#define IPV67_ERR_NOROUTE     -2
#define IPV67_ERR_NOPEER      -3
#define IPV67_ERR_TOOLONG     -4
#define IPV67_ERR_NOMEM       -5
#define IPV67_ERR_TIMEOUT     -6
#define IPV67_ERR_BUSY        -11

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
    uint64_t sequence;
    uint64_t nonce;
    uint64_t packet_id;
    uint8_t  signature[IPV67_SIG_SIZE];
    char     alias[IPV67_ALIAS_SIZE];
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
    uint8_t  authenticated;
    uint8_t  session_established;
    char     alias[IPV67_ALIAS_SIZE];
    uint64_t highest_sequence;
    uint64_t replay_window;
    uint8_t  public_key[IPV67_IDENTITY_SIZE];
    uint8_t  session_key[IPV67_SESSION_KEY_SIZE];
    uint64_t local_challenge;
    uint64_t remote_challenge;
    uint32_t handshake_ticks;
    uint8_t  missed_probes;
    uint32_t last_seen_ticks;
} ipv67_peer_t;

typedef struct {
    ipv67_addr_t dest;
    uint32_t     next_hop_ipv4;
    ipv6_addr_t  next_hop_ipv6;
    uint16_t     next_hop_port;
    uint8_t      next_hop_family;
    uint8_t      hops;
    uint8_t      metric;
    uint8_t      valid;
    uint32_t     sequence;
    uint32_t     age_ticks;
    uint8_t      public_key[IPV67_IDENTITY_SIZE];
} ipv67_route_t;

typedef struct {
    uint32_t asn;
    ipv67_addr_t start;
    ipv67_addr_t end;
    uint8_t specificity;
    uint8_t flags;
    uint8_t valid;
    uint8_t conflict_count;
    uint32_t age_ticks;
    uint8_t source_key[IPV67_IDENTITY_SIZE];
    char country[IPV67_ASN_COUNTRY_SIZE];
    char source[IPV67_ASN_SOURCE_SIZE];
    char name[IPV67_ASN_NAME_SIZE];
    char label[IPV67_ASN_LABEL_SIZE];
} ipv67_asn_claim_t;

typedef struct {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t forwarded_packets;
    uint64_t replay_drops;
    uint64_t auth_drops;
    uint64_t no_route_drops;
    uint64_t malformed_drops;
    uint64_t route_updates;
    uint64_t asn_claims_rx;
    uint64_t asn_claims_tx;
    uint64_t punch_requests_tx;
    uint64_t punch_observed_tx;
    uint64_t punch_packets_rx;
    uint64_t punch_peers_learned;
    uint64_t auth_hello_rx;
    uint64_t auth_reply_rx;
    uint64_t auth_done_rx;
    uint64_t auth_payload_ok;
    uint64_t auth_payload_fail;
    uint64_t auth_sessions;
    uint64_t auth_fail_self;
    uint64_t auth_fail_challenge;
    uint64_t auth_fail_shared;
    uint64_t auth_fail_identity;
    uint64_t auth_session_fail;
} ipv67_stats_t;

int         ipv67_init(void);
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
int         ipv67_peer_addr_verified(const ipv67_peer_t *peer);
int         ipv67_peer_count(void);
int         ipv67_get_peers(ipv67_peer_t *out, int max);
int         ipv67_route_count_get(void);
int         ipv67_get_routes(ipv67_route_t *out, int max);
int         ipv67_asn_count_get(void);
int         ipv67_get_asns(ipv67_asn_claim_t *out, int max);
int         ipv67_set_asn_claim(const ipv67_asn_claim_t *claim);
int         ipv67_remove_asn_claim(uint32_t asn, const ipv67_addr_t *start, const ipv67_addr_t *end);
int         ipv67_get_local_asn(void);
int         ipv67_get_stats(ipv67_stats_t *out);
void        ipv67_clear_stats(void);
int         ipv67_ping(const ipv67_addr_t *dst, uint32_t timeout_ms);
int         ipv67_punch(const ipv67_addr_t *dst);
int         ipv67_probe_peers(void);
int         ipv67_set_auth_key(const uint8_t *key, uint64_t len);
int         ipv67_set_identity_key(const uint8_t *key, uint64_t len);
int         ipv67_set_auth_required(int required);
int         ipv67_get_auth_required(void);
int         ipv67_set_port(uint16_t port);
uint16_t    ipv67_get_port(void);
void        ipv67_receive6(const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len);
void        ipv67_receive6_on_port(uint16_t local_port, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len);
int         ipv67_use_port(uint16_t port);
int         ipv67_use_port_existing(uint16_t port);
int         ipv67_port_active(uint16_t port);
void        ipv67_stack_lock(void);
int         ipv67_stack_trylock(void);
void        ipv67_stack_unlock(void);
void        ipv67_drain_pending_locked(void);

#endif

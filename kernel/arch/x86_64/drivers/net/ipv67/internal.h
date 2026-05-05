#ifndef IPV67_INTERNAL_H
#define IPV67_INTERNAL_H

#include <lebirun/drivers/net/ipv67.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/netif.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/crypto.h>
#include <lebirun/rng.h>
#include <lebirun/mem_map.h>
#include <lebirun/spinlock.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <string.h>

extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern uint64_t net_get_ticks(void);

typedef struct {
    ipv67_addr_t target;
    uint32_t token;
    uint64_t send_time;
    uint64_t rtt;
    int active;
    int received;
} ipv67_ping_state_t;

typedef struct {
    ipv67_addr_t self;
    int self_set;
    uint16_t port;
    uint8_t auth_key[IPV67_AUTH_KEY_SIZE];
    int auth_key_set;
    uint8_t identity_key[IPV67_IDENTITY_SIZE];
    uint8_t identity_public[IPV67_IDENTITY_SIZE];
    int identity_key_set;
    uint64_t next_sequence;
    uint64_t *replay_ids;
    int replay_cap;
    int replay_pos;
    uint32_t route_sequence;
    ipv67_peer_t *peers;
    int peer_cap;
    int peer_count_val;
    ipv67_route_t *routes;
    int route_cap;
    int route_count;
    ipv67_ping_state_t ping_state;
    int io_depth;
    int active;
} ipv67_context_t;

typedef struct {
    uint8_t family;
    uint8_t hops;
    uint8_t metric;
    uint32_t sequence;
    uint16_t port;
    uint32_t ipv4;
    ipv6_addr_t ipv6;
    char addr[IPV67_ADDR_STR_MAX];
    uint8_t public_key[IPV67_IDENTITY_SIZE];
} __attribute__((packed)) ipv67_route_adv_entry_t;

typedef struct {
    uint8_t public_key[IPV67_IDENTITY_SIZE];
    uint64_t challenge;
    uint8_t proof[IPV67_SIG_SIZE];
} __attribute__((packed)) ipv67_auth_payload_t;

#define IPV67_RX_PENDING_MAX 2
#define IPV67_RX_PACKET_MAX 1472

typedef struct ipv67_pending_rx {
    int next;
    int state;
    uint8_t family;
    uint16_t local_port;
    uint16_t src_port;
    uint32_t src_ipv4;
    ipv6_addr_t src_ipv6;
    uint64_t len;
    uint8_t *packet;
} ipv67_pending_rx_t;

extern ipv67_context_t **ipv67_contexts;
extern int ipv67_context_cap;
extern int ipv67_context_count;
extern ipv67_context_t *ipv67_current;
extern const uint8_t ipv67_bootstrap_key[IPV67_AUTH_KEY_SIZE];

#define ipv67_self ipv67_current->self
#define ipv67_self_set ipv67_current->self_set
#define ipv67_port ipv67_current->port
#define ipv67_auth_key ipv67_current->auth_key
#define ipv67_auth_key_set ipv67_current->auth_key_set
#define ipv67_identity_key ipv67_current->identity_key
#define ipv67_identity_public ipv67_current->identity_public
#define ipv67_identity_key_set ipv67_current->identity_key_set
#define ipv67_next_sequence ipv67_current->next_sequence
#define ipv67_replay_ids ipv67_current->replay_ids
#define ipv67_replay_cap ipv67_current->replay_cap
#define ipv67_replay_pos ipv67_current->replay_pos
#define ipv67_route_sequence ipv67_current->route_sequence
#define ipv67_peers ipv67_current->peers
#define ipv67_peer_count_val ipv67_current->peer_count_val
#define ipv67_routes ipv67_current->routes
#define ipv67_route_count ipv67_current->route_count
#define ipv67_ping_state ipv67_current->ping_state
#define IPV67_RESTORE_RETURN(saved_ctx) do { ipv67_current = (saved_ctx); return; } while (0)

void ipv67_make_alias_from_key(const uint8_t *key, char alias[IPV67_ALIAS_SIZE]);
void ipv67_derive_public_identity(const uint8_t *key, uint8_t *public_key);
void ipv67_derive_session_key(ipv67_peer_t *peer);
const uint8_t *ipv67_signing_key_for_peer(const ipv67_peer_t *peer);
int ipv67_is_bootstrap_type(uint8_t type);
void ipv67_make_alias(const ipv67_addr_t *addr, char alias[IPV67_ALIAS_SIZE]);
void ipv67_prepare_header(ipv67_header_t *hdr);
void ipv67_sign_header_key(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, const uint8_t *key);
void ipv67_sign_header(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen);
int ipv67_verify_header(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, ipv67_peer_t *peer);
int ipv67_packet_replay_seen(ipv67_peer_t *peer, uint8_t type, uint64_t sequence, uint64_t packet_id);

void ipv67_context_reset(ipv67_context_t *ctx, uint16_t port);
ipv67_context_t *ipv67_context_get(uint16_t port, int create);
int ipv67_ensure_peer_cap(int needed);
int ipv67_ensure_route_cap(int needed);

ipv67_peer_t *find_peer(uint32_t ipv4, uint16_t port);
int ipv6_addr_eq_raw(const ipv6_addr_t *a, const ipv6_addr_t *b);
ipv67_peer_t *find_peer6(const ipv6_addr_t *ipv6, uint16_t port);
ipv67_peer_t *find_peer_addr(const ipv67_addr_t *addr);

void ipv67_cleanup_stale(void);
void ipv67_update_route(const ipv67_addr_t *dest, uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, uint8_t hops);
ipv67_route_t *ipv67_find_route(const ipv67_addr_t *dst);
int ipv67_send_raw(uint32_t dst_ipv4, uint16_t dst_port, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen);
int ipv67_send_raw6(const ipv6_addr_t *dst_ipv6, uint16_t dst_port, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen);
int ipv67_send_to_peer(const ipv67_peer_t *peer, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen);
int ipv67_send_to_route(const ipv67_route_t *route, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen);
ipv67_peer_t *ipv67_peer_for_route(const ipv67_route_t *route);
int ipv67_send_route_unlocked(const ipv67_route_t *route, uint16_t src_port, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen);
uint16_t ipv67_build_route_adv(uint8_t *buf, uint16_t max_len);
void ipv67_apply_route_adv(uint8_t via_family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, const uint8_t *payload, uint16_t plen);

int ipv67_advertise_to_peer(const ipv67_peer_t *peer, uint8_t type);
void ipv67_make_auth_payload(ipv67_peer_t *peer, ipv67_auth_payload_t *auth, uint8_t type);
int ipv67_verify_auth_payload(ipv67_peer_t *peer, const ipv67_auth_payload_t *auth, uint8_t type);
int ipv67_send_auth_to_peer(ipv67_peer_t *peer, uint8_t type);

int ipv67_rx_enqueue(uint8_t family, uint16_t local_port, uint32_t src_ipv4, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len);
void ipv67_rx_flush_port(uint16_t port);
void ipv67_receive_on_port_locked(uint16_t local_port, uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len);
void ipv67_receive6_on_port_locked(uint16_t local_port, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len);

#endif

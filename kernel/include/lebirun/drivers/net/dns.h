#ifndef DNS_H
#define DNS_H

#include <lebirun/drivers/net/net_types.h>

#define DNS_CACHE_TTL 300000

typedef struct {
    char name[256];
    ipv4_addr_t ipv4;
    ipv6_addr_t ipv6;
    uint8_t has_ipv4;
    uint8_t has_ipv6;
    uint64_t timestamp;
    uint64_t ttl;
} dns_cache_entry_t;

void dns_init(void);
int dns_server_count(void);
ipv4_addr_t dns_server_at(int index);
int dns_set_servers(const ipv4_addr_t *servers, int count);
int dns6_server_count(void);
ipv6_addr_t dns6_server_at(int index);
int dns6_set_servers(const ipv6_addr_t *servers, int count);
int dns_resolve(const char *hostname, ipv4_addr_t *out_ipv4);
int dns_resolve6(const char *hostname, ipv6_addr_t *out_ipv6);
int dns_resolve_timeout(const char *hostname, ipv4_addr_t *out_ipv4, uint64_t timeout_ms);
void dns_receive(netif_t *netif, ipv4_addr_t src, uint16_t src_port, uint8_t *data, uint64_t len);
void dns_receive6(netif_t *netif, const ipv6_addr_t *src, uint16_t src_port, uint8_t *data, uint64_t len);
void dns_cache_add(const char *name, ipv4_addr_t ip, uint64_t ttl);
void dns_cache_print(void);

#endif

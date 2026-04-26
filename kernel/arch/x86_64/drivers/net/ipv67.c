#include <lebirun/drivers/net/ipv67.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/netif.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/crypto.h>
#include <lebirun/mem_map.h>
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
    ipv67_peer_t *peers;
    int peer_cap;
    int peer_count_val;
    ipv67_route_t *routes;
    int route_cap;
    int route_count;
    ipv67_ping_state_t ping_state;
    int active;
} ipv67_context_t;

static ipv67_context_t *ipv67_contexts[IPV67_MAX_CONTEXTS];
static ipv67_context_t *ipv67_current;

#define ipv67_self ipv67_current->self
#define ipv67_self_set ipv67_current->self_set
#define ipv67_port ipv67_current->port
#define ipv67_peers ipv67_current->peers
#define ipv67_peer_count_val ipv67_current->peer_count_val
#define ipv67_routes ipv67_current->routes
#define ipv67_route_count ipv67_current->route_count
#define ipv67_ping_state ipv67_current->ping_state

static void ipv67_update_route(const ipv67_addr_t *dest, uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, uint8_t hops);

static void ipv67_context_reset(ipv67_context_t *ctx, uint16_t port) {
    ipv67_peer_t *peers;
    ipv67_route_t *routes;

    if (!ctx) return;
    peers = ctx->peers;
    routes = ctx->routes;
    if (peers) kfree(peers);
    if (routes) kfree(routes);
    memset(ctx, 0, sizeof(ipv67_context_t));
    ctx->port = port ? port : IPV67_PORT_DEFAULT;
    ctx->active = 1;
}

static ipv67_context_t *ipv67_context_find(uint16_t port) {
    int i;

    if (port == 0) port = IPV67_PORT_DEFAULT;
    for (i = 0; i < IPV67_MAX_CONTEXTS; i++) {
        if (ipv67_contexts[i] && ipv67_contexts[i]->active && ipv67_contexts[i]->port == port) return ipv67_contexts[i];
    }
    return NULL;
}

static ipv67_context_t *ipv67_context_get(uint16_t port, int create) {
    ipv67_context_t *ctx;
    int i;

    if (port == 0) port = IPV67_PORT_DEFAULT;
    ctx = ipv67_context_find(port);
    if (ctx) return ctx;
    if (!create) return NULL;
    for (i = 0; i < IPV67_MAX_CONTEXTS; i++) {
        if (!ipv67_contexts[i]) {
            ctx = (ipv67_context_t *)kmalloc(sizeof(ipv67_context_t));
            if (!ctx) return NULL;
            memset(ctx, 0, sizeof(ipv67_context_t));
            ipv67_contexts[i] = ctx;
            ipv67_context_reset(ctx, port);
            return ctx;
        }
    }
    return NULL;
}

int ipv67_use_port(uint16_t port) {
    ipv67_context_t *ctx;

    ctx = ipv67_context_get(port, 1);
    if (!ctx) return IPV67_ERR_NOMEM;
    ipv67_current = ctx;
    return IPV67_ERR_OK;
}

int ipv67_port_active(uint16_t port) {
    return ipv67_context_find(port) != NULL;
}

static int ipv67_ensure_peer_cap(int needed) {
    ipv67_peer_t *next;
    int next_cap;
    int i;

    if (needed <= ipv67_current->peer_cap) return IPV67_ERR_OK;
    next_cap = ipv67_current->peer_cap ? ipv67_current->peer_cap * 2 : 8;
    while (next_cap < needed) next_cap *= 2;
    if (next_cap > IPV67_MAX_PEERS) next_cap = IPV67_MAX_PEERS;
    if (needed > next_cap) return IPV67_ERR_NOMEM;
    next = (ipv67_peer_t *)kmalloc(sizeof(ipv67_peer_t) * next_cap);
    if (!next) return IPV67_ERR_NOMEM;
    memset(next, 0, sizeof(ipv67_peer_t) * next_cap);
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        memcpy(&next[i], &ipv67_peers[i], sizeof(ipv67_peer_t));
    }
    if (ipv67_current->peers) kfree(ipv67_current->peers);
    ipv67_current->peers = next;
    ipv67_current->peer_cap = next_cap;
    return IPV67_ERR_OK;
}

static int ipv67_ensure_route_cap(int needed) {
    ipv67_route_t *next;
    int next_cap;
    int i;

    if (needed <= ipv67_current->route_cap) return IPV67_ERR_OK;
    next_cap = ipv67_current->route_cap ? ipv67_current->route_cap * 2 : 8;
    while (next_cap < needed) next_cap *= 2;
    if (next_cap > IPV67_MAX_ROUTES) next_cap = IPV67_MAX_ROUTES;
    if (needed > next_cap) return IPV67_ERR_NOMEM;
    next = (ipv67_route_t *)kmalloc(sizeof(ipv67_route_t) * next_cap);
    if (!next) return IPV67_ERR_NOMEM;
    memset(next, 0, sizeof(ipv67_route_t) * next_cap);
    for (i = 0; i < ipv67_current->route_cap; i++) {
        memcpy(&next[i], &ipv67_routes[i], sizeof(ipv67_route_t));
    }
    if (ipv67_current->routes) kfree(ipv67_current->routes);
    ipv67_current->routes = next;
    ipv67_current->route_cap = next_cap;
    return IPV67_ERR_OK;
}

static int is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_valid_zone(const char *s) {
    int i;
    int len;
    len = 0;
    for (i = 0; s[i]; i++) {
        if (!is_hex_char(s[i])) return 0;
        len++;
    }
    return len > 0 && len <= IPV67_ZONE_MAX;
}

static int is_valid_domain(const char *s) {
    char c;
    int i;
    int len;

    len = 0;
    for (i = 0; s[i]; i++) {
        c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        len++;
    }
    return len > 0 && len <= IPV67_DOMAIN_MAX;
}

int ipv67_addr_parse(const char *str, ipv67_addr_t *out) {
    const char *p;
    const char *dot;
    int part;
    int i;
    int len;

    if (!str || !out) return IPV67_ERR_INVAL;

    memset(out, 0, sizeof(ipv67_addr_t));
    p = str;
    part = 0;

    for (part = 0; part < 5; part++) {
        dot = p;
        while (*dot && *dot != '.') dot++;
        len = (int)(dot - p);

        if (len == 0) return IPV67_ERR_INVAL;

        switch (part) {
        case 0:
            if (len > IPV67_ZONE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->zone1, p, len);
            out->zone1[len] = '\0';
            break;
        case 1:
            if (len > IPV67_ZONE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->zone2, p, len);
            out->zone2[len] = '\0';
            break;
        case 2:
            if (len > IPV67_DOMAIN_MAX) return IPV67_ERR_TOOLONG;
            memcpy(out->domain, p, len);
            out->domain[len] = '\0';
            break;
        case 3:
            if (len > IPV67_NODE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->node1, p, len);
            out->node1[len] = '\0';
            break;
        case 4:
            if (len > IPV67_NODE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->node2, p, len);
            out->node2[len] = '\0';
            break;
        default:
            return IPV67_ERR_INVAL;
        }

        p = dot;
        if (part < 4) {
            if (*p != '.') return IPV67_ERR_INVAL;
            p++;
        }
    }

    if (*p != '\0') return IPV67_ERR_INVAL;

    if (!is_valid_zone(out->zone1)) return IPV67_ERR_INVAL;
    if (!is_valid_zone(out->zone2)) return IPV67_ERR_INVAL;
    if (!is_valid_domain(out->domain)) return IPV67_ERR_INVAL;
    if (!is_valid_zone(out->node1)) return IPV67_ERR_INVAL;
    if (!is_valid_zone(out->node2)) return IPV67_ERR_INVAL;

    return IPV67_ERR_OK;
}

int ipv67_addr_format(const ipv67_addr_t *addr, char *buf, uint64_t bufsz) {
    int needed;
    int z1, z2, dom, n1, n2;

    if (!addr || !buf || bufsz == 0) return IPV67_ERR_INVAL;

    z1  = (int)strlen(addr->zone1);
    z2  = (int)strlen(addr->zone2);
    dom = (int)strlen(addr->domain);
    n1  = (int)strlen(addr->node1);
    n2  = (int)strlen(addr->node2);
    needed = z1 + 1 + z2 + 1 + dom + 1 + n1 + 1 + n2 + 1;

    if ((uint64_t)needed > bufsz) return IPV67_ERR_TOOLONG;

    snprintf(buf, bufsz, "%s.%s.%s.%s.%s",
             addr->zone1, addr->zone2, addr->domain, addr->node1, addr->node2);
    return IPV67_ERR_OK;
}

int ipv67_addr_eq(const ipv67_addr_t *a, const ipv67_addr_t *b) {
    if (!a || !b) return 0;
    return strcmp(a->zone1, b->zone1) == 0 &&
           strcmp(a->zone2, b->zone2) == 0 &&
           strcmp(a->domain, b->domain) == 0 &&
           strcmp(a->node1, b->node1) == 0 &&
           strcmp(a->node2, b->node2) == 0;
}

void ipv67_self_addr(ipv67_addr_t *out) {
    if (!out) return;
    if (ipv67_self_set) {
        memcpy(out, &ipv67_self, sizeof(ipv67_addr_t));
    } else {
        memset(out, 0, sizeof(ipv67_addr_t));
    }
}

int ipv67_set_self_addr(const ipv67_addr_t *addr) {
    if (!addr) return IPV67_ERR_INVAL;
    memcpy(&ipv67_self, addr, sizeof(ipv67_addr_t));
    ipv67_self_set = 1;
    return IPV67_ERR_OK;
}

static ipv67_peer_t *find_peer(uint32_t ipv4, uint16_t port) {
    int i;

    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active &&
            ipv67_peers[i].family == IPV67_PEER_IPV4 &&
            ipv67_peers[i].ipv4 == ipv4 &&
            ipv67_peers[i].port == port) {
            return &ipv67_peers[i];
        }
    }
    return NULL;
}

static int ipv6_addr_eq_raw(const ipv6_addr_t *a, const ipv6_addr_t *b) {
    int i;

    if (!a || !b) return 0;
    for (i = 0; i < 16; i++) {
        if (a->octets[i] != b->octets[i]) return 0;
    }
    return 1;
}

static int ipv6_addr_is_loopback(const ipv6_addr_t *addr) {
    int i;

    if (!addr) return 0;
    for (i = 0; i < 15; i++) {
        if (addr->octets[i] != 0) return 0;
    }
    return addr->octets[15] == 1;
}

static ipv67_peer_t *find_peer6(const ipv6_addr_t *ipv6, uint16_t port) {
    int i;

    if (!ipv6) return NULL;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active &&
            ipv67_peers[i].family == IPV67_PEER_IPV6 &&
            ipv67_peers[i].port == port &&
            ipv6_addr_eq_raw(&ipv67_peers[i].ipv6, ipv6)) {
            return &ipv67_peers[i];
        }
    }
    return NULL;
}

static int ipv67_find_peer_slot(void) {
    int i;
    int oldest_idx;
    uint32_t oldest_age;
    uint32_t age;

    if (ipv67_current->peer_count_val < IPV67_MAX_PEERS) {
        if (ipv67_ensure_peer_cap(ipv67_current->peer_count_val + 1) < 0) return -1;
    }

    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (!ipv67_peers[i].active) return i;
    }

    oldest_idx = 0;
    oldest_age = 0;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        age = net_get_ticks() - ipv67_peers[i].last_seen_ticks;
        if (age > oldest_age) {
            oldest_age = age;
            oldest_idx = i;
        }
    }
    memset(&ipv67_peers[oldest_idx], 0, sizeof(ipv67_peer_t));
    if (ipv67_peer_count_val > 0) ipv67_peer_count_val--;
    return oldest_idx;
}

int ipv67_add_peer(uint32_t ipv4, uint16_t port) {
    return ipv67_add_peer_with_addr(ipv4, port, NULL);
}

int ipv67_add_peer_with_addr(uint32_t ipv4, uint16_t port, const ipv67_addr_t *addr) {
    int free_idx;
    ipv67_peer_t *peer;
    const ipv67_addr_t *learned_addr;

    learned_addr = addr;
    if (!learned_addr && ipv67_self_set && port == ipv67_port && ipv4 == 0x7f000001u) learned_addr = &ipv67_self;
    peer = find_peer(ipv4, port);
    if (peer) {
        if (learned_addr) {
            memcpy(&peer->addr, learned_addr, sizeof(ipv67_addr_t));
            ipv67_update_route(learned_addr, IPV67_PEER_IPV4, ipv4, NULL, port, 1);
        }
        peer->last_seen_ticks = net_get_ticks();
        return IPV67_ERR_OK;
    }

    free_idx = ipv67_find_peer_slot();

    if (free_idx < 0) return IPV67_ERR_NOMEM;

    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    ipv67_peers[free_idx].ipv4 = ipv4;
    ipv67_peers[free_idx].port = port;
    ipv67_peers[free_idx].family = IPV67_PEER_IPV4;
    if (learned_addr) memcpy(&ipv67_peers[free_idx].addr, learned_addr, sizeof(ipv67_addr_t));
    ipv67_peers[free_idx].active = 1;
    ipv67_peers[free_idx].last_seen_ticks = net_get_ticks();
    if (learned_addr) ipv67_update_route(learned_addr, IPV67_PEER_IPV4, ipv4, NULL, port, 1);
    ipv67_peer_count_val++;
    return IPV67_ERR_OK;
}

int ipv67_add_peer6(const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr) {
    int free_idx;
    ipv67_peer_t *peer;
    const ipv67_addr_t *learned_addr;

    if (!ipv6 || port == 0) return IPV67_ERR_INVAL;

    learned_addr = addr;
    if (!learned_addr && ipv67_self_set && port == ipv67_port && ipv6_addr_is_loopback(ipv6)) learned_addr = &ipv67_self;
    peer = find_peer6(ipv6, port);
    if (peer) {
        if (learned_addr) {
            memcpy(&peer->addr, learned_addr, sizeof(ipv67_addr_t));
            ipv67_update_route(learned_addr, IPV67_PEER_IPV6, 0, ipv6, port, 1);
        }
        peer->last_seen_ticks = net_get_ticks();
        return IPV67_ERR_OK;
    }

    free_idx = ipv67_find_peer_slot();

    if (free_idx < 0) return IPV67_ERR_NOMEM;

    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    memcpy(&ipv67_peers[free_idx].ipv6, ipv6, sizeof(ipv6_addr_t));
    ipv67_peers[free_idx].port = port;
    ipv67_peers[free_idx].family = IPV67_PEER_IPV6;
    if (learned_addr) memcpy(&ipv67_peers[free_idx].addr, learned_addr, sizeof(ipv67_addr_t));
    ipv67_peers[free_idx].active = 1;
    ipv67_peers[free_idx].last_seen_ticks = net_get_ticks();
    if (learned_addr) ipv67_update_route(learned_addr, IPV67_PEER_IPV6, 0, ipv6, port, 1);
    ipv67_peer_count_val++;
    return IPV67_ERR_OK;
}

int ipv67_remove_peer(uint32_t ipv4, uint16_t port) {
    ipv67_peer_t *p;
    p = find_peer(ipv4, port);
    if (!p) return IPV67_ERR_NOPEER;
    memset(p, 0, sizeof(ipv67_peer_t));
    ipv67_peer_count_val--;
    if (ipv67_peer_count_val < 0) ipv67_peer_count_val = 0;
    return IPV67_ERR_OK;
}

int ipv67_remove_peer_by_addr(const ipv67_addr_t *addr) {
    int i;

    if (!addr) return IPV67_ERR_INVAL;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active && ipv67_addr_eq(&ipv67_peers[i].addr, addr)) {
            memset(&ipv67_peers[i], 0, sizeof(ipv67_peer_t));
            ipv67_peer_count_val--;
            if (ipv67_peer_count_val < 0) ipv67_peer_count_val = 0;
            return IPV67_ERR_OK;
        }
    }
    return IPV67_ERR_NOPEER;
}

int ipv67_peer_count(void) {
    return ipv67_peer_count_val;
}

int ipv67_get_peers(ipv67_peer_t *out, int max) {
    int i;
    int count;

    if (!out || max <= 0) return 0;

    count = 0;
    for (i = 0; i < ipv67_current->peer_cap && count < max; i++) {
        if (ipv67_peers[i].active) {
            memcpy(&out[count], &ipv67_peers[i], sizeof(ipv67_peer_t));
            count++;
        }
    }
    return count;
}

int ipv67_route_count_get(void) {
    return ipv67_route_count;
}

int ipv67_get_routes(ipv67_route_t *out, int max) {
    int i;
    int count;

    if (!out || max <= 0) return 0;

    count = 0;
    for (i = 0; i < ipv67_current->route_cap && count < max; i++) {
        if (ipv67_routes[i].valid) {
            memcpy(&out[count], &ipv67_routes[i], sizeof(ipv67_route_t));
            count++;
        }
    }
    return count;
}

static void ipv67_update_route(const ipv67_addr_t *dest, uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, uint8_t hops) {
    int i;
    int free_idx;
    int oldest_idx;
    uint32_t oldest;

    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_addr_eq(&ipv67_routes[i].dest, dest)) {
            if (hops <= ipv67_routes[i].hops) {
                ipv67_routes[i].next_hop_family = family;
                ipv67_routes[i].next_hop_ipv4 = via_ipv4;
                if (via_ipv6) memcpy(&ipv67_routes[i].next_hop_ipv6, via_ipv6, sizeof(ipv6_addr_t));
                ipv67_routes[i].next_hop_port = via_port;
                ipv67_routes[i].hops = hops;
                ipv67_routes[i].age_ticks = net_get_ticks();
            } else {
                ipv67_routes[i].age_ticks = net_get_ticks();
            }
            return;
        }
    }

    free_idx = -1;
    if (ipv67_route_count < IPV67_MAX_ROUTES) {
        if (ipv67_ensure_route_cap(ipv67_route_count + 1) < 0) return;
    }

    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (!ipv67_routes[i].valid) {
            free_idx = i;
            break;
        }
    }

    if (free_idx < 0) {
        oldest = 0;
        oldest_idx = 0;
        for (i = 0; i < ipv67_current->route_cap; i++) {
            if (net_get_ticks() - ipv67_routes[i].age_ticks > oldest) {
                oldest = net_get_ticks() - ipv67_routes[i].age_ticks;
                oldest_idx = i;
            }
        }
        free_idx = oldest_idx;
    } else {
        ipv67_route_count++;
    }

    memset(&ipv67_routes[free_idx], 0, sizeof(ipv67_route_t));
    memcpy(&ipv67_routes[free_idx].dest, dest, sizeof(ipv67_addr_t));
    ipv67_routes[free_idx].next_hop_family = family;
    ipv67_routes[free_idx].next_hop_ipv4 = via_ipv4;
    if (via_ipv6) memcpy(&ipv67_routes[free_idx].next_hop_ipv6, via_ipv6, sizeof(ipv6_addr_t));
    ipv67_routes[free_idx].next_hop_port = via_port;
    ipv67_routes[free_idx].hops = hops;
    ipv67_routes[free_idx].valid = 1;
    ipv67_routes[free_idx].age_ticks = net_get_ticks();
}

static ipv67_route_t *ipv67_find_route(const ipv67_addr_t *dst) {
    int i;
    ipv67_route_t *best;
    int best_hops;

    best = NULL;
    best_hops = IPV67_MAX_HOPS + 1;
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_addr_eq(&ipv67_routes[i].dest, dst)) {
            if (ipv67_routes[i].hops < best_hops) {
                best_hops = ipv67_routes[i].hops;
                best = &ipv67_routes[i];
            }
        }
    }
    return best;
}

static int ipv67_send_raw(uint32_t dst_ipv4, uint16_t dst_port,
                           ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t *buf;
    uint64_t total;
    netif_t *netif;
    ipv4_addr_t dst;
    int ret;

    total = sizeof(ipv67_header_t) + plen;
    buf = (uint8_t *)kmalloc(total);
    if (!buf) return IPV67_ERR_NOMEM;

    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);

    netif = netif_get_default();
    if (!netif) {
        kfree(buf);
        return IPV67_ERR_NOROUTE;
    }

    dst.octets[0] = (uint8_t)((dst_ipv4 >> 24) & 0xFF);
    dst.octets[1] = (uint8_t)((dst_ipv4 >> 16) & 0xFF);
    dst.octets[2] = (uint8_t)((dst_ipv4 >>  8) & 0xFF);
    dst.octets[3] = (uint8_t)( dst_ipv4        & 0xFF);

    ret = udp_send(netif, dst, ipv67_port, dst_port, buf, (uint64_t)total);
    kfree(buf);
    if (ret < 0) return IPV67_ERR_NOROUTE;
    return ret;
}

static int ipv67_send_raw6(const ipv6_addr_t *dst_ipv6, uint16_t dst_port,
                           ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t *buf;
    uint64_t total;
    netif_t *netif;
    int ret;

    if (!dst_ipv6) return IPV67_ERR_INVAL;

    total = sizeof(ipv67_header_t) + plen;
    buf = (uint8_t *)kmalloc(total);
    if (!buf) return IPV67_ERR_NOMEM;

    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);

    netif = netif_get_default();
    if (!netif) {
        kfree(buf);
        return IPV67_ERR_NOROUTE;
    }

    ret = udp_send6(netif, *dst_ipv6, ipv67_port, dst_port, buf, (uint64_t)total);
    kfree(buf);
    if (ret < 0) return IPV67_ERR_NOROUTE;
    return ret;
}

static int ipv67_send_to_peer(const ipv67_peer_t *peer, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    if (!peer) return IPV67_ERR_INVAL;
    if (peer->family == IPV67_PEER_IPV6) {
        return ipv67_send_raw6(&peer->ipv6, peer->port, hdr, payload, plen);
    }
    return ipv67_send_raw(peer->ipv4, peer->port, hdr, payload, plen);
}

static int ipv67_send_to_route(const ipv67_route_t *route, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    if (!route) return IPV67_ERR_INVAL;
    if (route->next_hop_family == IPV67_PEER_IPV6) {
        return ipv67_send_raw6(&route->next_hop_ipv6, route->next_hop_port, hdr, payload, plen);
    }
    return ipv67_send_raw(route->next_hop_ipv4, route->next_hop_port, hdr, payload, plen);
}

int ipv67_probe_peers(void) {
    ipv67_header_t hdr;
    char self_str[IPV67_ADDR_STR_MAX];
    int i;
    int sent;
    int ret;

    if (!ipv67_self_set) return IPV67_ERR_INVAL;

    ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str));
    memset(&hdr, 0, sizeof(ipv67_header_t));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = IPV67_TYPE_PEER_REQ;
    hdr.payload_len = 0;
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, self_str, strlen(self_str) + 1);
    memcpy(hdr.dst, self_str, strlen(self_str) + 1);

    sent = 0;
    ret = IPV67_ERR_NOPEER;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active) {
            if (ipv67_self_set && ipv67_addr_eq(&ipv67_peers[i].addr, &ipv67_self)) {
                sent++;
                ret = IPV67_ERR_OK;
            } else {
                ret = ipv67_send_to_peer(&ipv67_peers[i], &hdr, NULL, 0);
                if (ret >= 0) sent++;
            }
        }
    }
    if (sent > 0) return IPV67_ERR_OK;
    return ret;
}

int ipv67_send(const ipv67_addr_t *dst, const uint8_t *data, uint64_t len) {
    ipv67_header_t hdr;
    ipv67_route_t *route;
    char dst_str[IPV67_ADDR_STR_MAX];
    char src_str[IPV67_ADDR_STR_MAX];
    int i;
    int sent;
    int ret;

    if (!dst || !ipv67_self_set) return IPV67_ERR_INVAL;
    if (len > 65000) return IPV67_ERR_TOOLONG;
    if (ipv67_addr_eq(dst, &ipv67_self)) return IPV67_ERR_OK;

    ipv67_addr_format(dst, dst_str, sizeof(dst_str));
    ipv67_addr_format(&ipv67_self, src_str, sizeof(src_str));

    memset(&hdr, 0, sizeof(ipv67_header_t));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = IPV67_TYPE_DATA;
    hdr.payload_len = (uint16_t)len;
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, src_str, strlen(src_str) + 1);
    memcpy(hdr.dst, dst_str, strlen(dst_str) + 1);

    route = ipv67_find_route(dst);
    if (route) {
        return ipv67_send_to_route(route, &hdr, data, (uint16_t)len);
    }

    sent = 0;
    ret = IPV67_ERR_NOROUTE;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active) {
            ret = ipv67_send_to_peer(&ipv67_peers[i], &hdr, data, (uint16_t)len);
            if (ret >= 0) sent++;
        }
    }
    if (sent > 0) return IPV67_ERR_OK;
    return ret;
}

void ipv67_receive(uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    ipv67_receive_on_port(ipv67_port, src_ipv4, src_port, packet, len);
}

void ipv67_receive_on_port(uint16_t local_port, uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    ipv67_context_t *saved;
    ipv67_context_t *ctx;
    ipv67_header_t hdr;
    ipv67_addr_t src_addr;
    ipv67_addr_t dst_addr;
    ipv67_header_t reply_hdr;
    char src_str[IPV67_ADDR_STR_MAX];
    uint16_t payload_len;
    const uint8_t *payload;
    uint32_t token;
    ipv67_route_t *fwd;
    ipv67_peer_t *p;

    saved = ipv67_current;
    ctx = ipv67_context_get(local_port, 0);
    if (!ctx) return;
    ipv67_current = ctx;

    if (!packet || len < sizeof(ipv67_header_t)) {
        ipv67_current = saved;
        return;
    }

    memcpy(&hdr, packet, sizeof(ipv67_header_t));

    if (hdr.magic != IPV67_MAGIC) {
        ipv67_current = saved;
        return;
    }
    if (hdr.version != IPV67_PROTO_VERSION) {
        ipv67_current = saved;
        return;
    }

    hdr.src[IPV67_ADDR_STR_MAX - 1] = '\0';
    hdr.dst[IPV67_ADDR_STR_MAX - 1] = '\0';

    if (ipv67_addr_parse(hdr.src, &src_addr) != IPV67_ERR_OK) {
        ipv67_current = saved;
        return;
    }
    if (ipv67_addr_parse(hdr.dst, &dst_addr) != IPV67_ERR_OK) {
        ipv67_current = saved;
        return;
    }

    if (hdr.hop_limit == 0) {
        ipv67_current = saved;
        return;
    }

    payload_len = hdr.payload_len;
    if ((uint64_t)payload_len > len - sizeof(ipv67_header_t)) {
        ipv67_current = saved;
        return;
    }
    payload = packet + sizeof(ipv67_header_t);

    ipv67_add_peer(src_ipv4, src_port);

    p = find_peer(src_ipv4, src_port);
    if (p) {
        memcpy(&p->addr, &src_addr, sizeof(ipv67_addr_t));
        p->last_seen_ticks = net_get_ticks();
    }

    ipv67_update_route(&src_addr, IPV67_PEER_IPV4, src_ipv4, NULL, src_port, 1);

    if (hdr.type == IPV67_TYPE_HELLO) {
        memset(&reply_hdr, 0, sizeof(ipv67_header_t));
        reply_hdr.magic = IPV67_MAGIC;
        reply_hdr.version = IPV67_PROTO_VERSION;
        reply_hdr.type = IPV67_TYPE_PEER_ACK;
        reply_hdr.payload_len = 0;
        reply_hdr.hop_limit = IPV67_MAX_HOPS;
        if (ipv67_self_set) {
            ipv67_addr_format(&ipv67_self, reply_hdr.src, IPV67_ADDR_STR_MAX);
        }
        memcpy(reply_hdr.dst, hdr.src, IPV67_ADDR_STR_MAX);
        ipv67_send_raw(src_ipv4, src_port, &reply_hdr, NULL, 0);
        ipv67_current = saved;
        return;
    }

    if (hdr.type == IPV67_TYPE_PING) {
        ipv67_addr_format(&src_addr, src_str, sizeof(src_str));
        memset(&reply_hdr, 0, sizeof(ipv67_header_t));
        reply_hdr.magic = IPV67_MAGIC;
        reply_hdr.version = IPV67_PROTO_VERSION;
        reply_hdr.type = IPV67_TYPE_PONG;
        reply_hdr.payload_len = payload_len;
        reply_hdr.hop_limit = IPV67_MAX_HOPS;
        if (ipv67_self_set) {
            ipv67_addr_format(&ipv67_self, reply_hdr.src, IPV67_ADDR_STR_MAX);
        }
        memcpy(reply_hdr.dst, src_str, strlen(src_str) + 1);
        ipv67_send_raw(src_ipv4, src_port, &reply_hdr, payload, payload_len);
        ipv67_current = saved;
        return;
    }

    if (hdr.type == IPV67_TYPE_PONG) {
        if (ipv67_ping_state.active && !ipv67_ping_state.received &&
            ipv67_addr_eq(&src_addr, &ipv67_ping_state.target) &&
            payload_len >= sizeof(uint32_t)) {
            memcpy(&token, payload, sizeof(uint32_t));
            if (token == ipv67_ping_state.token) {
                ipv67_ping_state.received = 1;
                ipv67_ping_state.rtt = net_get_ticks() - ipv67_ping_state.send_time;
            }
        }
        ipv67_current = saved;
        return;
    }

    if (hdr.type == IPV67_TYPE_PEER_REQ) {
        memset(&reply_hdr, 0, sizeof(ipv67_header_t));
        reply_hdr.magic = IPV67_MAGIC;
        reply_hdr.version = IPV67_PROTO_VERSION;
        reply_hdr.type = IPV67_TYPE_PEER_ACK;
        reply_hdr.payload_len = 0;
        reply_hdr.hop_limit = IPV67_MAX_HOPS;
        if (ipv67_self_set) {
            ipv67_addr_format(&ipv67_self, reply_hdr.src, IPV67_ADDR_STR_MAX);
        }
        memcpy(reply_hdr.dst, hdr.src, IPV67_ADDR_STR_MAX);
        ipv67_send_raw(src_ipv4, src_port, &reply_hdr, NULL, 0);
        ipv67_current = saved;
        return;
    }

    if (ipv67_self_set && !ipv67_addr_eq(&dst_addr, &ipv67_self)) {
        hdr.hop_limit--;
        if (hdr.hop_limit == 0) {
            ipv67_current = saved;
            return;
        }
        fwd = ipv67_find_route(&dst_addr);
        if (fwd) {
            ipv67_send_to_route(fwd, &hdr, packet + sizeof(ipv67_header_t), hdr.payload_len);
        }
    }
    ipv67_current = saved;
}

void ipv67_receive6(const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    ipv67_receive6_on_port(ipv67_port, src_ipv6, src_port, packet, len);
}

void ipv67_receive6_on_port(uint16_t local_port, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    ipv67_context_t *saved;
    ipv67_context_t *ctx;
    ipv67_header_t hdr;
    ipv67_addr_t src_addr;
    ipv67_addr_t dst_addr;
    ipv67_header_t reply_hdr;
    char src_str[IPV67_ADDR_STR_MAX];
    uint16_t payload_len;
    const uint8_t *payload;
    uint32_t token;
    ipv67_route_t *fwd;

    saved = ipv67_current;
    ctx = ipv67_context_get(local_port, 0);
    if (!ctx) return;
    ipv67_current = ctx;

    if (!src_ipv6 || !packet || len < sizeof(ipv67_header_t)) {
        ipv67_current = saved;
        return;
    }

    memcpy(&hdr, packet, sizeof(ipv67_header_t));

    if (hdr.magic != IPV67_MAGIC) {
        ipv67_current = saved;
        return;
    }
    if (hdr.version != IPV67_PROTO_VERSION) {
        ipv67_current = saved;
        return;
    }

    hdr.src[IPV67_ADDR_STR_MAX - 1] = '\0';
    hdr.dst[IPV67_ADDR_STR_MAX - 1] = '\0';

    if (ipv67_addr_parse(hdr.src, &src_addr) != IPV67_ERR_OK) {
        ipv67_current = saved;
        return;
    }
    if (ipv67_addr_parse(hdr.dst, &dst_addr) != IPV67_ERR_OK) {
        ipv67_current = saved;
        return;
    }

    if (hdr.hop_limit == 0) {
        ipv67_current = saved;
        return;
    }

    payload_len = hdr.payload_len;
    if ((uint64_t)payload_len > len - sizeof(ipv67_header_t)) {
        ipv67_current = saved;
        return;
    }
    payload = packet + sizeof(ipv67_header_t);

    ipv67_add_peer6(src_ipv6, src_port, &src_addr);
    ipv67_update_route(&src_addr, IPV67_PEER_IPV6, 0, src_ipv6, src_port, 1);

    if (hdr.type == IPV67_TYPE_HELLO) {
        memset(&reply_hdr, 0, sizeof(ipv67_header_t));
        reply_hdr.magic = IPV67_MAGIC;
        reply_hdr.version = IPV67_PROTO_VERSION;
        reply_hdr.type = IPV67_TYPE_PEER_ACK;
        reply_hdr.payload_len = 0;
        reply_hdr.hop_limit = IPV67_MAX_HOPS;
        if (ipv67_self_set) {
            ipv67_addr_format(&ipv67_self, reply_hdr.src, IPV67_ADDR_STR_MAX);
        }
        memcpy(reply_hdr.dst, hdr.src, IPV67_ADDR_STR_MAX);
        ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, NULL, 0);
        ipv67_current = saved;
        return;
    }

    if (hdr.type == IPV67_TYPE_PING) {
        ipv67_addr_format(&src_addr, src_str, sizeof(src_str));
        memset(&reply_hdr, 0, sizeof(ipv67_header_t));
        reply_hdr.magic = IPV67_MAGIC;
        reply_hdr.version = IPV67_PROTO_VERSION;
        reply_hdr.type = IPV67_TYPE_PONG;
        reply_hdr.payload_len = payload_len;
        reply_hdr.hop_limit = IPV67_MAX_HOPS;
        if (ipv67_self_set) {
            ipv67_addr_format(&ipv67_self, reply_hdr.src, IPV67_ADDR_STR_MAX);
        }
        memcpy(reply_hdr.dst, src_str, strlen(src_str) + 1);
        ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, payload, payload_len);
        ipv67_current = saved;
        return;
    }

    if (hdr.type == IPV67_TYPE_PONG) {
        if (ipv67_ping_state.active && !ipv67_ping_state.received &&
            ipv67_addr_eq(&src_addr, &ipv67_ping_state.target) &&
            payload_len >= sizeof(uint32_t)) {
            memcpy(&token, payload, sizeof(uint32_t));
            if (token == ipv67_ping_state.token) {
                ipv67_ping_state.received = 1;
                ipv67_ping_state.rtt = net_get_ticks() - ipv67_ping_state.send_time;
            }
        }
        ipv67_current = saved;
        return;
    }

    if (hdr.type == IPV67_TYPE_PEER_REQ) {
        memset(&reply_hdr, 0, sizeof(ipv67_header_t));
        reply_hdr.magic = IPV67_MAGIC;
        reply_hdr.version = IPV67_PROTO_VERSION;
        reply_hdr.type = IPV67_TYPE_PEER_ACK;
        reply_hdr.payload_len = 0;
        reply_hdr.hop_limit = IPV67_MAX_HOPS;
        if (ipv67_self_set) {
            ipv67_addr_format(&ipv67_self, reply_hdr.src, IPV67_ADDR_STR_MAX);
        }
        memcpy(reply_hdr.dst, hdr.src, IPV67_ADDR_STR_MAX);
        ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, NULL, 0);
        ipv67_current = saved;
        return;
    }

    if (ipv67_self_set && !ipv67_addr_eq(&dst_addr, &ipv67_self)) {
        hdr.hop_limit--;
        if (hdr.hop_limit == 0) {
            ipv67_current = saved;
            return;
        }
        fwd = ipv67_find_route(&dst_addr);
        if (fwd) {
            ipv67_send_to_route(fwd, &hdr, packet + sizeof(ipv67_header_t), hdr.payload_len);
        }
    }
    ipv67_current = saved;
}

int ipv67_ping(const ipv67_addr_t *dst, uint32_t timeout_ms) {
    ipv67_header_t hdr;
    ipv67_route_t *route;
    char dst_str[IPV67_ADDR_STR_MAX];
    char src_str[IPV67_ADDR_STR_MAX];
    int i;
    int sent;
    int ret;
    uint32_t token;
    uint64_t timeout_ticks;
    uint64_t start;

    if (!dst || !ipv67_self_set) return IPV67_ERR_INVAL;
    if (ipv67_addr_eq(dst, &ipv67_self)) return IPV67_ERR_OK;

    ipv67_addr_format(dst, dst_str, sizeof(dst_str));
    ipv67_addr_format(&ipv67_self, src_str, sizeof(src_str));

    memset(&hdr, 0, sizeof(ipv67_header_t));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = IPV67_TYPE_PING;
    hdr.payload_len = sizeof(uint32_t);
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, src_str, strlen(src_str) + 1);
    memcpy(hdr.dst, dst_str, strlen(dst_str) + 1);

    token = (uint32_t)(net_get_ticks() ^ pit_get_ticks() ^ ((uint64_t)dst_str[0] << 16));
    memcpy(&ipv67_ping_state.target, dst, sizeof(ipv67_addr_t));
    ipv67_ping_state.token = token;
    ipv67_ping_state.send_time = net_get_ticks();
    ipv67_ping_state.rtt = 0;
    ipv67_ping_state.active = 1;
    ipv67_ping_state.received = 0;

    route = ipv67_find_route(dst);
    if (route) {
        ret = ipv67_send_to_route(route, &hdr, (const uint8_t *)&token, sizeof(token));
    } else {
        sent = 0;
        ret = IPV67_ERR_NOROUTE;
        for (i = 0; i < ipv67_current->peer_cap; i++) {
            if (ipv67_peers[i].active) {
                ret = ipv67_send_to_peer(&ipv67_peers[i], &hdr, (const uint8_t *)&token, sizeof(token));
                if (ret >= 0) sent++;
            }
        }
        if (sent > 0) ret = IPV67_ERR_OK;
    }

    if (ret < 0) {
        ipv67_ping_state.active = 0;
        return ret;
    }

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();
    while (!ipv67_ping_state.received) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (pit_get_ticks() - start > timeout_ticks) {
            ipv67_ping_state.active = 0;
            return IPV67_ERR_TIMEOUT;
        }
        schedule();
    }

    ipv67_ping_state.active = 0;
    return (int)ipv67_ping_state.rtt;
}

void ipv67_init(void) {
    ipv67_context_t *ctx;
    uint16_t port;

    port = ipv67_current && ipv67_current->port ? ipv67_current->port : IPV67_PORT_DEFAULT;
    ctx = ipv67_context_get(port, 1);
    if (!ctx) return;
    ipv67_current = ctx;
    ipv67_context_reset(ctx, port);
}

int ipv67_set_port(uint16_t port) {
    return ipv67_use_port(port);
}

uint16_t ipv67_get_port(void) {
    return ipv67_port;
}

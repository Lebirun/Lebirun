#include "internal.h"

ipv67_peer_t *find_peer(uint32_t ipv4, uint16_t port) {
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

int ipv6_addr_eq_raw(const ipv6_addr_t *a, const ipv6_addr_t *b) {
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

ipv67_peer_t *find_peer6(const ipv6_addr_t *ipv6, uint16_t port) {
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

ipv67_peer_t *find_peer_addr(const ipv67_addr_t *addr) {
    int i;

    if (!addr || !addr->zone1[0]) return NULL;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active && ipv67_addr_eq(&ipv67_peers[i].addr, addr)) {
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

    if (ipv67_ensure_peer_cap(ipv67_current->peer_count_val + 1) < 0) return -1;

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
            ipv67_make_alias(learned_addr, peer->alias);
            ipv67_update_route(learned_addr, IPV67_PEER_IPV4, ipv4, NULL, port, 1);
        }
        peer->last_seen_ticks = net_get_ticks();
        return IPV67_ERR_OK;
    }
    if (learned_addr) {
        peer = find_peer_addr(learned_addr);
        if (peer) {
            peer->ipv4 = ipv4;
            memset(&peer->ipv6, 0, sizeof(ipv6_addr_t));
            peer->port = port;
            peer->family = IPV67_PEER_IPV4;
            ipv67_make_alias(learned_addr, peer->alias);
            peer->last_seen_ticks = net_get_ticks();
            ipv67_update_route(learned_addr, IPV67_PEER_IPV4, ipv4, NULL, port, 1);
            return IPV67_ERR_OK;
        }
    }

    free_idx = ipv67_find_peer_slot();

    if (free_idx < 0) return IPV67_ERR_NOMEM;

    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    ipv67_peers[free_idx].ipv4 = ipv4;
    ipv67_peers[free_idx].port = port;
    ipv67_peers[free_idx].family = IPV67_PEER_IPV4;
    if (learned_addr) {
        memcpy(&ipv67_peers[free_idx].addr, learned_addr, sizeof(ipv67_addr_t));
        ipv67_make_alias(learned_addr, ipv67_peers[free_idx].alias);
    }
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
            ipv67_make_alias(learned_addr, peer->alias);
            ipv67_update_route(learned_addr, IPV67_PEER_IPV6, 0, ipv6, port, 1);
        }
        peer->last_seen_ticks = net_get_ticks();
        return IPV67_ERR_OK;
    }
    if (learned_addr) {
        peer = find_peer_addr(learned_addr);
        if (peer) {
            memcpy(&peer->ipv6, ipv6, sizeof(ipv6_addr_t));
            peer->ipv4 = 0;
            peer->port = port;
            peer->family = IPV67_PEER_IPV6;
            ipv67_make_alias(learned_addr, peer->alias);
            peer->last_seen_ticks = net_get_ticks();
            ipv67_update_route(learned_addr, IPV67_PEER_IPV6, 0, ipv6, port, 1);
            return IPV67_ERR_OK;
        }
    }

    free_idx = ipv67_find_peer_slot();

    if (free_idx < 0) return IPV67_ERR_NOMEM;

    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    memcpy(&ipv67_peers[free_idx].ipv6, ipv6, sizeof(ipv6_addr_t));
    ipv67_peers[free_idx].port = port;
    ipv67_peers[free_idx].family = IPV67_PEER_IPV6;
    if (learned_addr) {
        memcpy(&ipv67_peers[free_idx].addr, learned_addr, sizeof(ipv67_addr_t));
        ipv67_make_alias(learned_addr, ipv67_peers[free_idx].alias);
    }
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
    int i;
    int count;
    int cap;

    if (!ipv67_current || !ipv67_peers || ipv67_current->peer_cap <= 0) return 0;
    cap = ipv67_current->peer_cap;
    count = 0;
    for (i = 0; i < cap; i++) {
        if (ipv67_peers[i].active) count++;
    }
    ipv67_peer_count_val = count;
    return count;
}

int ipv67_get_peers(ipv67_peer_t *out, int max) {
    int i;
    int count;
    int cap;

    if (!out || max <= 0) return 0;
    if (!ipv67_current || !ipv67_peers || ipv67_current->peer_cap <= 0) return 0;
    cap = ipv67_current->peer_cap;

    count = 0;
    for (i = 0; i < cap && count < max; i++) {
        if (ipv67_peers[i].active) {
            memcpy(&out[count], &ipv67_peers[i], sizeof(ipv67_peer_t));
            count++;
        }
    }
    return count;
}

int ipv67_route_count_get(void) {
    int i;
    int count;
    int cap;

    if (!ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0) return 0;
    cap = ipv67_current->route_cap;
    count = 0;
    for (i = 0; i < cap; i++) {
        if (ipv67_routes[i].valid) count++;
    }
    ipv67_route_count = count;
    return count;
}

int ipv67_get_routes(ipv67_route_t *out, int max) {
    int i;
    int count;
    int cap;

    if (!out || max <= 0) return 0;
    if (!ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0) return 0;
    cap = ipv67_current->route_cap;

    count = 0;
    for (i = 0; i < cap && count < max; i++) {
        if (ipv67_routes[i].valid) {
            memcpy(&out[count], &ipv67_routes[i], sizeof(ipv67_route_t));
            count++;
        }
    }
    return count;
}


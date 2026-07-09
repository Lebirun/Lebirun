#include "internal.h"

static int ipv67_find_peer_slot(void);
static ipv67_peer_t *ipv67_find_peer_endpoint(uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port);
static void ipv67_set_peer_endpoint(ipv67_peer_t *peer, uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port);
static int ipv67_ipv4_is_loopback(uint32_t ipv4);

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

static int ipv67_ipv4_is_loopback(uint32_t ipv4) {
    return ((ipv4 >> 24) & 0xff) == 127;
}

static int ipv6_addr_is_loopback(const ipv6_addr_t *addr) {
    int i;

    if (!addr) return 0;
    for (i = 0; i < 15; i++) {
        if (addr->octets[i] != 0) return 0;
    }
    return addr->octets[15] == 1;
}

static ipv67_peer_t *ipv67_find_peer_endpoint(uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port) {
    if (family == IPV67_PEER_IPV6) return find_peer6(ipv6, port);
    return find_peer(ipv4, port);
}

static void ipv67_set_peer_endpoint(ipv67_peer_t *peer, uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port) {
    if (!peer) return;
    peer->family = family;
    peer->port = port;
    if (family == IPV67_PEER_IPV6) {
        peer->ipv4 = 0;
        if (ipv6) memcpy(&peer->ipv6, ipv6, sizeof(ipv6_addr_t));
        else memset(&peer->ipv6, 0, sizeof(ipv6_addr_t));
    } else {
        peer->ipv4 = ipv4;
        memset(&peer->ipv6, 0, sizeof(ipv6_addr_t));
    }
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

static void ipv67_update_peer_alias(ipv67_peer_t *peer) {
    if (!peer) return;
    if (ipv67_identity_key_present(peer->public_key)) {
        ipv67_make_alias_from_key(peer->public_key, peer->alias);
    } else if (peer->addr.zone1[0]) {
        ipv67_make_alias(&peer->addr, peer->alias);
    }
}

int ipv67_peer_public_key_allowed(const ipv67_peer_t *peer, const uint8_t *public_key) {
    if (!peer || !ipv67_identity_key_present(public_key)) return 0;
    if (!ipv67_identity_key_present(peer->public_key)) return 1;
    if (!peer->authenticated && !peer->session_established) return 1;
    return memcmp(peer->public_key, public_key, IPV67_IDENTITY_SIZE) == 0;
}

int ipv67_peer_public_key_addr_allowed(const ipv67_peer_t *peer, const uint8_t *public_key, const ipv67_addr_t *addr) {
    if (!peer || !addr || !ipv67_identity_key_present(public_key)) return 0;
    if (!ipv67_peer_public_key_allowed(peer, public_key)) return 0;
    if (ipv67_asn_claim_allows_key_addr(public_key, addr)) return 1;
    return 0;
}

int ipv67_peer_addr_verified(const ipv67_peer_t *peer) {
    if (!peer || !peer->active || !peer->addr.zone1[0]) return 0;
    if (!peer->authenticated || !peer->session_established) return 0;
    if (!ipv67_identity_key_present(peer->public_key)) return 0;
    if (ipv67_asn_claim_allows_key_addr(peer->public_key, &peer->addr)) return 1;
    return 0;
}

int ipv67_peer_auth_key_addr_allowed(const ipv67_peer_t *peer, const uint8_t *public_key, const ipv67_addr_t *addr) {
    if (!peer || !addr || !ipv67_identity_key_present(public_key)) return 0;
    if (!ipv67_peer_public_key_allowed(peer, public_key)) return 0;
    if (ipv67_asn_claim_allows_key_addr(public_key, addr)) return 1;
    return 0;
}

static ipv67_peer_t *ipv67_learn_peer_endpoint(uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr) {
    ipv67_peer_t *peer;
    int free_idx;
    int known_identity;
    int same_addr;

    if (!addr) return NULL;
    if (family == IPV67_PEER_IPV6 && !ipv6) return NULL;
    peer = ipv67_find_peer_endpoint(family, ipv4, ipv6, port);
    if (!peer) peer = find_peer_addr(addr);
    if (peer) {
        known_identity = ipv67_identity_key_present(peer->public_key);
        same_addr = peer->addr.zone1[0] && ipv67_addr_eq(&peer->addr, addr);
        if (known_identity && peer->addr.zone1[0] && !same_addr && !ipv67_peer_public_key_addr_allowed(peer, peer->public_key, addr)) return NULL;
        ipv67_set_peer_endpoint(peer, family, ipv4, ipv6, port);
        memcpy(&peer->addr, addr, sizeof(ipv67_addr_t));
        peer->active = 1;
        peer->missed_probes = 0;
        peer->last_seen_ticks = net_get_ticks();
        ipv67_update_peer_alias(peer);
        return peer;
    }
    free_idx = ipv67_find_peer_slot();
    if (free_idx < 0) return NULL;
    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    ipv67_set_peer_endpoint(&ipv67_peers[free_idx], family, ipv4, ipv6, port);
    memcpy(&ipv67_peers[free_idx].addr, addr, sizeof(ipv67_addr_t));
    ipv67_peers[free_idx].active = 1;
    ipv67_peers[free_idx].missed_probes = 0;
    ipv67_peers[free_idx].last_seen_ticks = net_get_ticks();
    ipv67_update_peer_alias(&ipv67_peers[free_idx]);
    ipv67_peer_count_val++;
    return &ipv67_peers[free_idx];
}

ipv67_peer_t *ipv67_learn_peer4(uint32_t ipv4, uint16_t port, const ipv67_addr_t *addr) {
    return ipv67_learn_peer_endpoint(IPV67_PEER_IPV4, ipv4, NULL, port, addr);
}

ipv67_peer_t *ipv67_learn_peer6(const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr) {
    return ipv67_learn_peer_endpoint(IPV67_PEER_IPV6, 0, ipv6, port, addr);
}

static void ipv67_apply_peer_public_key(ipv67_peer_t *peer, const uint8_t *public_key) {
    if (!ipv67_peer_public_key_allowed(peer, public_key)) return;
    memcpy(peer->public_key, public_key, IPV67_IDENTITY_SIZE);
    ipv67_update_peer_alias(peer);
}

static ipv67_peer_t *ipv67_remember_peer_candidate(uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr, const uint8_t *public_key) {
    ipv67_peer_t *peer;
    int free_idx;

    if (!addr || !addr->zone1[0] || port == 0) return NULL;
    if (family == IPV67_PEER_IPV4 && ipv4 == 0) return NULL;
    if (family == IPV67_PEER_IPV6 && !ipv6) return NULL;
    if (family == IPV67_PEER_IPV4 && ipv67_ipv4_is_loopback(ipv4)) return NULL;
    if (family == IPV67_PEER_IPV6 && ipv6_addr_is_loopback(ipv6)) return NULL;
    if (ipv67_self_set && ipv67_addr_eq(addr, &ipv67_self)) return NULL;
    peer = ipv67_find_peer_endpoint(family, ipv4, ipv6, port);
    if (!peer) peer = find_peer_addr(addr);
    if (peer) {
        if (ipv67_identity_key_present(public_key) && !ipv67_peer_public_key_addr_allowed(peer, public_key, addr)) return NULL;
        ipv67_set_peer_endpoint(peer, family, ipv4, ipv6, port);
        memcpy(&peer->addr, addr, sizeof(ipv67_addr_t));
        peer->active = 1;
        peer->last_seen_ticks = net_get_ticks();
        ipv67_apply_peer_public_key(peer, public_key);
        ipv67_update_peer_alias(peer);
        return peer;
    }
    free_idx = ipv67_find_peer_slot();
    if (free_idx < 0) return NULL;
    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    ipv67_set_peer_endpoint(&ipv67_peers[free_idx], family, ipv4, ipv6, port);
    memcpy(&ipv67_peers[free_idx].addr, addr, sizeof(ipv67_addr_t));
    ipv67_peers[free_idx].active = 1;
    ipv67_peers[free_idx].last_seen_ticks = net_get_ticks();
    if (ipv67_identity_key_present(public_key) && !ipv67_peer_public_key_addr_allowed(&ipv67_peers[free_idx], public_key, addr)) {
        memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
        return NULL;
    }
    ipv67_apply_peer_public_key(&ipv67_peers[free_idx], public_key);
    ipv67_update_peer_alias(&ipv67_peers[free_idx]);
    ipv67_peer_count_val++;
    return &ipv67_peers[free_idx];
}

ipv67_peer_t *ipv67_remember_peer_candidate4(uint32_t ipv4, uint16_t port, const ipv67_addr_t *addr, const uint8_t *public_key) {
    return ipv67_remember_peer_candidate(IPV67_PEER_IPV4, ipv4, NULL, port, addr, public_key);
}

ipv67_peer_t *ipv67_remember_peer_candidate6(const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr, const uint8_t *public_key) {
    return ipv67_remember_peer_candidate(IPV67_PEER_IPV6, 0, ipv6, port, addr, public_key);
}

static int ipv67_find_peer_slot(void) {
    int i;
    int oldest_idx;
    uint32_t oldest_age;
    uint32_t age;
    int best_score;
    int score;

    if (ipv67_ensure_peer_cap(ipv67_current->peer_count_val + 1) < 0) return -1;

    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (!ipv67_peers[i].active) return i;
    }

    oldest_idx = 0;
    oldest_age = 0;
    best_score = -1;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        score = 0;
        if (!ipv67_peers[i].authenticated || !ipv67_peers[i].session_established) score = 3;
        else if (!ipv67_peer_addr_verified(&ipv67_peers[i])) score = 2;
        else if (ipv67_peers[i].missed_probes > 0) score = 1;
        age = net_get_ticks() - ipv67_peers[i].last_seen_ticks;
        if (score > best_score || (score == best_score && age > oldest_age)) {
            oldest_age = age;
            oldest_idx = i;
            best_score = score;
        }
    }
    ipv67_remove_routes_for_peer(&ipv67_peers[oldest_idx]);
    ipv67_remove_asn_claims_for_peer(&ipv67_peers[oldest_idx]);
    memset(&ipv67_peers[oldest_idx], 0, sizeof(ipv67_peer_t));
    if (ipv67_peer_count_val > 0) ipv67_peer_count_val--;
    return oldest_idx;
}

int ipv67_add_peer(uint32_t ipv4, uint16_t port) {
    return ipv67_add_peer_with_addr(ipv4, port, NULL);
}

static int ipv67_add_peer_endpoint(uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr) {
    int free_idx;
    ipv67_peer_t *peer;
    const ipv67_addr_t *learned_addr;

    learned_addr = addr;
    if (family == IPV67_PEER_IPV6 && (!ipv6 || port == 0)) return IPV67_ERR_INVAL;
    if (!learned_addr && ipv67_self_set && port == ipv67_port) {
        if (family == IPV67_PEER_IPV4 && ipv4 == 0x7f000001u) learned_addr = &ipv67_self;
        if (family == IPV67_PEER_IPV6 && ipv6_addr_is_loopback(ipv6)) learned_addr = &ipv67_self;
    }
    peer = ipv67_find_peer_endpoint(family, ipv4, ipv6, port);
    if (peer) {
        if (learned_addr) {
            memcpy(&peer->addr, learned_addr, sizeof(ipv67_addr_t));
            ipv67_make_alias(learned_addr, peer->alias);
            ipv67_update_route(learned_addr, family, ipv4, ipv6, port, 1);
        }
        peer->last_seen_ticks = net_get_ticks();
        return IPV67_ERR_OK;
    }
    if (learned_addr) {
        peer = find_peer_addr(learned_addr);
        if (peer) {
            ipv67_set_peer_endpoint(peer, family, ipv4, ipv6, port);
            ipv67_make_alias(learned_addr, peer->alias);
            peer->last_seen_ticks = net_get_ticks();
            ipv67_update_route(learned_addr, family, ipv4, ipv6, port, 1);
            return IPV67_ERR_OK;
        }
    }

    free_idx = ipv67_find_peer_slot();

    if (free_idx < 0) return IPV67_ERR_NOMEM;

    memset(&ipv67_peers[free_idx], 0, sizeof(ipv67_peer_t));
    ipv67_set_peer_endpoint(&ipv67_peers[free_idx], family, ipv4, ipv6, port);
    if (learned_addr) {
        memcpy(&ipv67_peers[free_idx].addr, learned_addr, sizeof(ipv67_addr_t));
        ipv67_make_alias(learned_addr, ipv67_peers[free_idx].alias);
    }
    ipv67_peers[free_idx].active = 1;
    ipv67_peers[free_idx].last_seen_ticks = net_get_ticks();
    if (learned_addr) ipv67_update_route(learned_addr, family, ipv4, ipv6, port, 1);
    ipv67_peer_count_val++;
    return IPV67_ERR_OK;
}

int ipv67_add_peer_with_addr(uint32_t ipv4, uint16_t port, const ipv67_addr_t *addr) {
    return ipv67_add_peer_endpoint(IPV67_PEER_IPV4, ipv4, NULL, port, addr);
}

int ipv67_add_peer6(const ipv6_addr_t *ipv6, uint16_t port, const ipv67_addr_t *addr) {
    return ipv67_add_peer_endpoint(IPV67_PEER_IPV6, 0, ipv6, port, addr);
}

int ipv67_remove_peer(uint32_t ipv4, uint16_t port) {
    ipv67_peer_t *p;
    p = find_peer(ipv4, port);
    if (!p) return IPV67_ERR_NOPEER;
    ipv67_remove_routes_for_peer(p);
    ipv67_remove_asn_claims_for_peer(p);
    memset(p, 0, sizeof(ipv67_peer_t));
    ipv67_peer_count_val--;
    if (ipv67_peer_count_val < 0) ipv67_peer_count_val = 0;
    ipv67_release_empty_tables();
    return IPV67_ERR_OK;
}

int ipv67_remove_peer_by_addr(const ipv67_addr_t *addr) {
    int i;

    if (!addr) return IPV67_ERR_INVAL;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active && ipv67_addr_eq(&ipv67_peers[i].addr, addr)) {
            ipv67_remove_routes_for_peer(&ipv67_peers[i]);
            ipv67_remove_asn_claims_for_peer(&ipv67_peers[i]);
            memset(&ipv67_peers[i], 0, sizeof(ipv67_peer_t));
            ipv67_peer_count_val--;
            if (ipv67_peer_count_val < 0) ipv67_peer_count_val = 0;
            ipv67_release_empty_tables();
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

int ipv67_get_peer_at(int index, ipv67_peer_t *out) {
    int i;
    int count;
    int cap;

    if (!out || index < 0) return IPV67_ERR_INVAL;
    if (!ipv67_current || !ipv67_peers || ipv67_current->peer_cap <= 0) return IPV67_ERR_NOPEER;
    cap = ipv67_current->peer_cap;
    count = 0;
    for (i = 0; i < cap; i++) {
        if (!ipv67_peers[i].active) continue;
        if (count == index) {
            memcpy(out, &ipv67_peers[i], sizeof(ipv67_peer_t));
            return IPV67_ERR_OK;
        }
        count++;
    }
    return IPV67_ERR_NOPEER;
}

int ipv67_route_count_get(void) {
    ipv67_peer_t *peer;
    uint32_t now;
    int i;
    int count;
    int cap;

    if (!ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0) return 0;
    cap = ipv67_current->route_cap;
    count = 0;
    now = net_get_ticks();
    for (i = 0; i < cap; i++) {
        if (!ipv67_routes[i].valid) continue;
        if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
        peer = ipv67_peer_for_route(&ipv67_routes[i]);
        if (!peer || !peer->authenticated || !peer->session_established) continue;
        if (!ipv67_peer_addr_verified(peer)) continue;
        if (!ipv67_identity_key_present(peer->public_key)) continue;
        if (!ipv67_identity_key_present(ipv67_routes[i].public_key)) continue;
        count++;
    }
    ipv67_route_count = count;
    return count;
}

int ipv67_get_routes(ipv67_route_t *out, int max) {
    ipv67_peer_t *peer;
    uint32_t now;
    int i;
    int count;
    int cap;

    if (!out || max <= 0) return 0;
    if (!ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0) return 0;
    cap = ipv67_current->route_cap;

    count = 0;
    now = net_get_ticks();
    for (i = 0; i < cap && count < max; i++) {
        if (!ipv67_routes[i].valid) continue;
        if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
        peer = ipv67_peer_for_route(&ipv67_routes[i]);
        if (!peer || !peer->authenticated || !peer->session_established) continue;
        if (!ipv67_peer_addr_verified(peer)) continue;
        if (!ipv67_identity_key_present(peer->public_key)) continue;
        if (!ipv67_identity_key_present(ipv67_routes[i].public_key)) continue;
        memcpy(&out[count], &ipv67_routes[i], sizeof(ipv67_route_t));
        count++;
    }
    return count;
}

int ipv67_get_route_at(int index, ipv67_route_t *out) {
    ipv67_peer_t *peer;
    uint32_t now;
    int i;
    int count;
    int cap;

    if (!out || index < 0) return IPV67_ERR_INVAL;
    if (!ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0) return IPV67_ERR_NOPEER;
    cap = ipv67_current->route_cap;
    count = 0;
    now = net_get_ticks();
    for (i = 0; i < cap; i++) {
        if (!ipv67_routes[i].valid) continue;
        if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
        peer = ipv67_peer_for_route(&ipv67_routes[i]);
        if (!peer || !peer->authenticated || !peer->session_established) continue;
        if (!ipv67_peer_addr_verified(peer)) continue;
        if (!ipv67_identity_key_present(peer->public_key)) continue;
        if (!ipv67_identity_key_present(ipv67_routes[i].public_key)) continue;
        if (count == index) {
            memcpy(out, &ipv67_routes[i], sizeof(ipv67_route_t));
            return IPV67_ERR_OK;
        }
        count++;
    }
    return IPV67_ERR_NOPEER;
}

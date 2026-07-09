#include "internal.h"

static ipv67_route_t *ipv67_find_valid_route(const ipv67_addr_t *dst);

int ipv67_route_matches_peer(const ipv67_route_t *route, const ipv67_peer_t *peer) {
    if (!route || !peer) return 0;
    if (route->next_hop_family != peer->family) return 0;
    if (route->next_hop_port != peer->port) return 0;
    if (peer->family == IPV67_PEER_IPV6) return ipv6_addr_eq_raw(&route->next_hop_ipv6, &peer->ipv6);
    return route->next_hop_ipv4 == peer->ipv4;
}

int ipv67_route_matches_endpoint4(const ipv67_route_t *route, uint32_t ipv4, uint16_t port) {
    if (!route) return 0;
    if (route->next_hop_family != IPV67_PEER_IPV4) return 0;
    if (route->next_hop_port != port) return 0;
    return route->next_hop_ipv4 == ipv4;
}

int ipv67_route_matches_endpoint6(const ipv67_route_t *route, const ipv6_addr_t *ipv6, uint16_t port) {
    if (!route || !ipv6) return 0;
    if (route->next_hop_family != IPV67_PEER_IPV6) return 0;
    if (route->next_hop_port != port) return 0;
    return ipv6_addr_eq_raw(&route->next_hop_ipv6, ipv6);
}

void ipv67_remove_routes_for_peer(const ipv67_peer_t *peer) {
    int i;

    if (!peer || !ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0) return;
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_route_matches_peer(&ipv67_routes[i], peer)) {
            memset(&ipv67_routes[i], 0, sizeof(ipv67_route_t));
            if (ipv67_route_count > 0) ipv67_route_count--;
        }
    }
    ipv67_release_empty_tables();
}

void ipv67_cleanup_stale(void) {
    ipv67_peer_t *peer;
    uint32_t now;
    int i;

    if (!ipv67_current) return;
    now = net_get_ticks();
    if (ipv67_routes && ipv67_current->route_cap > 0) {
        for (i = 0; i < ipv67_current->route_cap; i++) {
            if (!ipv67_routes[i].valid) continue;
            peer = ipv67_peer_for_route(&ipv67_routes[i]);
            if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS || !peer || !peer->authenticated || !peer->session_established || !ipv67_identity_key_present(peer->public_key) || !ipv67_peer_addr_verified(peer) || !ipv67_identity_key_present(ipv67_routes[i].public_key)) {
                memset(&ipv67_routes[i], 0, sizeof(ipv67_route_t));
                if (ipv67_route_count > 0) ipv67_route_count--;
            }
        }
    }
    if (!ipv67_peers || ipv67_current->peer_cap <= 0) return;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (!ipv67_peers[i].active) continue;
        if (now - ipv67_peers[i].last_seen_ticks <= IPV67_PEER_TTL_TICKS) continue;
        ipv67_remove_routes_for_peer(&ipv67_peers[i]);
        ipv67_remove_asn_claims_for_peer(&ipv67_peers[i]);
        memset(&ipv67_peers[i], 0, sizeof(ipv67_peer_t));
        if (ipv67_peer_count_val > 0) ipv67_peer_count_val--;
    }
    ipv67_release_empty_tables();
}

static const uint8_t *ipv67_route_public_key_for(uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port) {
    ipv67_peer_t *peer;

    if (family == IPV67_PEER_IPV6) peer = find_peer6(via_ipv6, via_port);
    else peer = find_peer(via_ipv4, via_port);
    if (!peer || !ipv67_identity_key_present(peer->public_key)) return NULL;
    return peer->public_key;
}

static int ipv67_route_public_key_addr_allowed(const uint8_t *public_key, const ipv67_addr_t *addr) {
    if (!ipv67_identity_key_present(public_key) || !addr) return 0;
    if (ipv67_asn_claim_routes_key_addr(public_key, addr)) return 1;
    return 0;
}

void ipv67_update_route_ex(const ipv67_addr_t *dest, uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, uint8_t hops, uint8_t metric, uint32_t sequence, const uint8_t *public_key) {
    int i;
    int free_idx;
    int oldest_idx;
    uint32_t oldest;
    uint32_t now;
    int same_next;
    const uint8_t *route_key;
    int route_key_present;
    int older_sequence;
    int same_sequence;

    if (!dest) return;
    now = net_get_ticks();
    route_key = public_key;
    if (hops <= 1 && !ipv67_identity_key_present(route_key)) route_key = ipv67_route_public_key_for(family, via_ipv4, via_ipv6, via_port);
    route_key_present = ipv67_identity_key_present(route_key);
    if (hops > 1 && !route_key_present) return;
    if (route_key_present && !ipv67_route_public_key_addr_allowed(route_key, dest)) return;
    if (metric == 0) metric = 1;
    if (hops == 0) hops = 1;
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_addr_eq(&ipv67_routes[i].dest, dest)) {
            if (route_key_present &&
                ipv67_identity_key_present(ipv67_routes[i].public_key) &&
                crypto_constant_compare(ipv67_routes[i].public_key, route_key, IPV67_IDENTITY_SIZE) != 0) return;
            if (route_key_present) memcpy(ipv67_routes[i].public_key, route_key, IPV67_IDENTITY_SIZE);
            same_next = 0;
            if (family == IPV67_PEER_IPV6) {
                if (via_ipv6 && ipv67_route_matches_endpoint6(&ipv67_routes[i], via_ipv6, via_port)) same_next = 1;
            } else {
                if (ipv67_route_matches_endpoint4(&ipv67_routes[i], via_ipv4, via_port)) same_next = 1;
            }
            older_sequence = sequence != 0 && ipv67_routes[i].sequence != 0 && sequence < ipv67_routes[i].sequence;
            same_sequence = sequence != 0 && ipv67_routes[i].sequence != 0 && sequence == ipv67_routes[i].sequence;
            if (older_sequence) {
                if (same_next) ipv67_routes[i].age_ticks = now;
                return;
            }
            if (same_sequence && !same_next) return;
            if (metric < ipv67_routes[i].metric || (!same_next && metric <= ipv67_routes[i].metric) || sequence > ipv67_routes[i].sequence || now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS / 2) {
                ipv67_routes[i].next_hop_family = family;
                ipv67_routes[i].next_hop_ipv4 = via_ipv4;
                if (via_ipv6) memcpy(&ipv67_routes[i].next_hop_ipv6, via_ipv6, sizeof(ipv6_addr_t));
                else memset(&ipv67_routes[i].next_hop_ipv6, 0, sizeof(ipv6_addr_t));
                ipv67_routes[i].next_hop_port = via_port;
                ipv67_routes[i].hops = hops;
                ipv67_routes[i].metric = metric;
                ipv67_routes[i].sequence = sequence ? sequence : ++ipv67_route_sequence;
                ipv67_routes[i].age_ticks = now;
                if (route_key_present) memcpy(ipv67_routes[i].public_key, route_key, IPV67_IDENTITY_SIZE);
                IPV67_STATS_INC(route_updates);
            } else {
                ipv67_routes[i].age_ticks = now;
            }
            return;
        }
    }

    free_idx = -1;
    if (ipv67_ensure_route_cap(ipv67_route_count + 1) < 0) return;

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
            if (now - ipv67_routes[i].age_ticks > oldest) {
                oldest = now - ipv67_routes[i].age_ticks;
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
    ipv67_routes[free_idx].metric = metric;
    ipv67_routes[free_idx].valid = 1;
    ipv67_routes[free_idx].sequence = sequence ? sequence : ++ipv67_route_sequence;
    ipv67_routes[free_idx].age_ticks = now;
    if (route_key_present) memcpy(ipv67_routes[free_idx].public_key, route_key, IPV67_IDENTITY_SIZE);
    IPV67_STATS_INC(route_updates);
}

void ipv67_update_route(const ipv67_addr_t *dest, uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, uint8_t hops) {
    uint8_t metric;

    metric = hops;
    if (metric == 0) metric = 1;
    ipv67_update_route_ex(dest, family, via_ipv4, via_ipv6, via_port, hops, metric, 0, NULL);
}

static int ipv67_ipv6_is_loopback(const ipv6_addr_t *addr) {
    int i;

    if (!addr) return 0;
    for (i = 0; i < 15; i++) {
        if (addr->octets[i] != 0) return 0;
    }
    return addr->octets[15] == 1;
}

static ipv67_route_t *ipv67_find_valid_route(const ipv67_addr_t *dst) {
    int i;
    ipv67_route_t *best;
    ipv67_peer_t *peer;
    int best_hops;
    uint32_t now;

    best = NULL;
    best_hops = IPV67_MAX_HOPS + 1;
    now = net_get_ticks();
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_addr_eq(&ipv67_routes[i].dest, dst)) {
            if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
            peer = ipv67_peer_for_route(&ipv67_routes[i]);
            if (!peer || !peer->authenticated || !peer->session_established) continue;
            if (!ipv67_peer_addr_verified(peer)) continue;
            if (!ipv67_identity_key_present(peer->public_key)) continue;
            if (!ipv67_identity_key_present(ipv67_routes[i].public_key)) continue;
            if (ipv67_routes[i].metric < best_hops) {
                best_hops = ipv67_routes[i].metric;
                best = &ipv67_routes[i];
            }
        }
    }
    return best;
}

ipv67_route_t *ipv67_find_route(const ipv67_addr_t *dst) {
    ipv67_route_t *best;

    ipv67_cleanup_stale();
    best = ipv67_find_valid_route(dst);
    if (!best && ipv67_route_from_asn(dst)) best = ipv67_find_valid_route(dst);
    return best;
}

int ipv67_send_raw(uint32_t dst_ipv4, uint16_t dst_port,
                           ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t buf[sizeof(ipv67_header_t) + 512];
    uint64_t total;
    netif_t *netif;
    ipv67_context_t *saved;
    ipv4_addr_t dst;
    ipv4_addr_t next_hop;
    int ret;
    uint16_t src_port;

    total = sizeof(ipv67_header_t) + plen;
    if (total > sizeof(buf)) return IPV67_ERR_TOOLONG;

    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);

    if (dst_ipv4 == 0x7f000001u && ipv67_context_get(dst_port, 0)) {
        src_port = ipv67_port;
        ret = ipv67_rx_enqueue(IPV67_PEER_IPV4, dst_port, dst_ipv4, NULL, src_port, buf, total);
        if (!ret) return IPV67_ERR_NOMEM;
        IPV67_STATS_INC(tx_packets);
        return IPV67_ERR_OK;
    }

    netif = netif_get_default();
    if (!netif) {
        return IPV67_ERR_NOROUTE;
    }

    saved = ipv67_current;
    src_port = ipv67_port;
    ipv67_current->io_depth++;

    dst.octets[0] = (uint8_t)((dst_ipv4 >> 24) & 0xFF);
    dst.octets[1] = (uint8_t)((dst_ipv4 >> 16) & 0xFF);
    dst.octets[2] = (uint8_t)((dst_ipv4 >>  8) & 0xFF);
    dst.octets[3] = (uint8_t)( dst_ipv4        & 0xFF);

    next_hop = dst;
    if (!ipv4_is_local(netif, dst)) next_hop = netif->gateway;
    if (ipv4_eq(next_hop, IPV4_ZERO)) {
        if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
        return IPV67_ERR_NOROUTE;
    }

    ipv67_stack_unlock();
    ret = udp_send(netif, dst, src_port, dst_port, buf, (uint64_t)total);
    ipv67_stack_lock();
    ipv67_current = saved;
    if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
    if (ret < 0) return IPV67_ERR_NOROUTE;
    IPV67_STATS_INC(tx_packets);
    return ret;
}

int ipv67_send_raw6(const ipv6_addr_t *dst_ipv6, uint16_t dst_port,
                           ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t buf[sizeof(ipv67_header_t) + 512];
    uint64_t total;
    netif_t *netif;
    ipv67_context_t *saved;
    ipv6_addr_t dst_copy;
    int ret;
    uint16_t src_port;

    if (!dst_ipv6) return IPV67_ERR_INVAL;
    memcpy(&dst_copy, dst_ipv6, sizeof(dst_copy));

    total = sizeof(ipv67_header_t) + plen;
    if (total > sizeof(buf)) return IPV67_ERR_TOOLONG;

    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);

    if (ipv67_ipv6_is_loopback(&dst_copy) && ipv67_context_get(dst_port, 0)) {
        src_port = ipv67_port;
        ret = ipv67_rx_enqueue(IPV67_PEER_IPV6, dst_port, 0, &dst_copy, src_port, buf, total);
        if (!ret) return IPV67_ERR_NOMEM;
        IPV67_STATS_INC(tx_packets);
        return IPV67_ERR_OK;
    }

    netif = netif_get_default();
    if (!netif) {
        return IPV67_ERR_NOROUTE;
    }

    saved = ipv67_current;
    src_port = ipv67_port;
    ipv67_current->io_depth++;
    ipv67_stack_unlock();
    ret = udp_send6(netif, dst_copy, src_port, dst_port, buf, (uint64_t)total);
    ipv67_stack_lock();
    ipv67_current = saved;
    if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
    if (ret < 0) return IPV67_ERR_NOROUTE;
    IPV67_STATS_INC(tx_packets);
    return ret;
}

int ipv67_send_to_peer(const ipv67_peer_t *peer, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    ipv67_peer_t peer_copy;

    if (!peer) return IPV67_ERR_INVAL;
    memcpy(&peer_copy, peer, sizeof(peer_copy));
    if (peer_copy.family == IPV67_PEER_IPV6) {
        return ipv67_send_raw6(&peer_copy.ipv6, peer_copy.port, hdr, payload, plen);
    }
    return ipv67_send_raw(peer_copy.ipv4, peer_copy.port, hdr, payload, plen);
}

int ipv67_send_to_route(const ipv67_route_t *route, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    ipv67_route_t route_copy;

    if (!route) return IPV67_ERR_INVAL;
    memcpy(&route_copy, route, sizeof(route_copy));
    if (route_copy.next_hop_family == IPV67_PEER_IPV6) {
        return ipv67_send_raw6(&route_copy.next_hop_ipv6, route_copy.next_hop_port, hdr, payload, plen);
    }
    return ipv67_send_raw(route_copy.next_hop_ipv4, route_copy.next_hop_port, hdr, payload, plen);
}

ipv67_peer_t *ipv67_peer_for_route(const ipv67_route_t *route) {
    if (!route) return NULL;
    if (route->next_hop_family == IPV67_PEER_IPV6) return find_peer6(&route->next_hop_ipv6, route->next_hop_port);
    return find_peer(route->next_hop_ipv4, route->next_hop_port);
}

int ipv67_send_route_unlocked(const ipv67_route_t *route, uint16_t src_port, ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t buf[sizeof(ipv67_header_t) + 512];
    uint64_t total;
    netif_t *netif;
    ipv4_addr_t dst;
    ipv4_addr_t next_hop;
    int ret;

    if (!route || !hdr) return IPV67_ERR_INVAL;
    total = sizeof(ipv67_header_t) + plen;
    if (total > sizeof(buf)) return IPV67_ERR_TOOLONG;
    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);
    netif = netif_get_default();
    if (!netif) {
        return IPV67_ERR_NOROUTE;
    }
    if (route->next_hop_family == IPV67_PEER_IPV6) {
        ret = udp_send6(netif, route->next_hop_ipv6, src_port, route->next_hop_port, buf, total);
        if (ret < 0) return IPV67_ERR_NOROUTE;
        if (ipv67_current) IPV67_STATS_INC(tx_packets);
        return ret;
    }
    dst.octets[0] = (uint8_t)((route->next_hop_ipv4 >> 24) & 0xFF);
    dst.octets[1] = (uint8_t)((route->next_hop_ipv4 >> 16) & 0xFF);
    dst.octets[2] = (uint8_t)((route->next_hop_ipv4 >>  8) & 0xFF);
    dst.octets[3] = (uint8_t)( route->next_hop_ipv4        & 0xFF);
    next_hop = dst;
    if (!ipv4_is_local(netif, dst)) next_hop = netif->gateway;
    if (ipv4_eq(next_hop, IPV4_ZERO)) {
        return IPV67_ERR_NOROUTE;
    }
    ret = udp_send(netif, dst, src_port, route->next_hop_port, buf, total);
    if (ret < 0) return IPV67_ERR_NOROUTE;
    if (ipv67_current) IPV67_STATS_INC(tx_packets);
    return ret;
}

uint16_t ipv67_build_route_adv(uint8_t *buf, uint16_t max_len, const ipv67_peer_t *skip_peer) {
    ipv67_route_adv_entry_t entry;
    ipv67_peer_t *route_peer;
    uint16_t off;
    uint32_t now;
    int i;
    int same_peer;

    if (!buf || max_len < sizeof(ipv67_route_adv_entry_t)) return 0;
    ipv67_cleanup_stale();
    off = 0;
    now = net_get_ticks();
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (off + sizeof(entry) > max_len) break;
        if (!ipv67_peers[i].active || !ipv67_peers[i].addr.zone1[0]) continue;
        if (!ipv67_peers[i].authenticated || !ipv67_peers[i].session_established) continue;
        if (!ipv67_peer_addr_verified(&ipv67_peers[i])) continue;
        if (!ipv67_identity_key_present(ipv67_peers[i].public_key)) continue;
        same_peer = 0;
        if (skip_peer && ipv67_peers[i].family == skip_peer->family && ipv67_peers[i].port == skip_peer->port) {
            if (ipv67_peers[i].family == IPV67_PEER_IPV6 && ipv6_addr_eq_raw(&ipv67_peers[i].ipv6, &skip_peer->ipv6)) same_peer = 1;
            if (ipv67_peers[i].family == IPV67_PEER_IPV4 && ipv67_peers[i].ipv4 == skip_peer->ipv4) same_peer = 1;
        }
        if (same_peer) continue;
        memset(&entry, 0, sizeof(entry));
        entry.family = ipv67_peers[i].family;
        entry.hops = 1;
        entry.metric = 1;
        entry.sequence = (uint32_t)ipv67_peers[i].highest_sequence;
        entry.port = ipv67_peers[i].port;
        entry.ipv4 = ipv67_peers[i].ipv4;
        memcpy(&entry.ipv6, &ipv67_peers[i].ipv6, sizeof(ipv6_addr_t));
        ipv67_addr_format(&ipv67_peers[i].addr, entry.addr, sizeof(entry.addr));
        memcpy(entry.public_key, ipv67_peers[i].public_key, IPV67_IDENTITY_SIZE);
        memcpy(buf + off, &entry, sizeof(entry));
        off += sizeof(entry);
    }
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (off + sizeof(entry) > max_len) break;
        if (!ipv67_routes[i].valid) continue;
        if (skip_peer && ipv67_route_matches_peer(&ipv67_routes[i], skip_peer)) continue;
        if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
        route_peer = ipv67_peer_for_route(&ipv67_routes[i]);
        if (!route_peer || !route_peer->authenticated || !route_peer->session_established) continue;
        if (!ipv67_peer_addr_verified(route_peer)) continue;
        if (!ipv67_identity_key_present(route_peer->public_key)) continue;
        if (!ipv67_identity_key_present(ipv67_routes[i].public_key)) continue;
        memset(&entry, 0, sizeof(entry));
        entry.family = ipv67_routes[i].next_hop_family;
        entry.hops = ipv67_routes[i].hops;
        if (entry.hops == 0 || entry.hops >= IPV67_MAX_HOPS) continue;
        if (ipv67_routes[i].metric >= IPV67_ROUTE_METRIC_MAX) continue;
        entry.metric = ipv67_routes[i].metric;
        if (entry.metric == 0) entry.metric = entry.hops;
        entry.sequence = ipv67_routes[i].sequence;
        entry.port = ipv67_routes[i].next_hop_port;
        entry.ipv4 = ipv67_routes[i].next_hop_ipv4;
        memcpy(&entry.ipv6, &ipv67_routes[i].next_hop_ipv6, sizeof(ipv6_addr_t));
        ipv67_addr_format(&ipv67_routes[i].dest, entry.addr, sizeof(entry.addr));
        if (ipv67_identity_key_present(ipv67_routes[i].public_key)) memcpy(entry.public_key, ipv67_routes[i].public_key, IPV67_IDENTITY_SIZE);
        memcpy(buf + off, &entry, sizeof(entry));
        off += sizeof(entry);
    }
    return off;
}

void ipv67_apply_route_adv(uint8_t via_family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, const uint8_t *payload, uint16_t plen, int authenticated, const uint8_t *source_key) {
    ipv67_route_adv_entry_t entry;
    ipv67_addr_t dest;
    ipv67_peer_t *peer;
    uint16_t off;
    uint8_t hops;
    uint8_t metric;
    uint32_t sequence;
    int same_endpoint;
    int punch_request;
    int key_owns_dest;
    int source_key_present;
    int entry_is_source_key;

    if (!payload) return;
    if (!authenticated) return;
    source_key_present = ipv67_identity_key_present(source_key);
    if (!source_key_present) return;
    if (via_port == 0) return;
    if (via_family == IPV67_PEER_IPV4 && via_ipv4 == 0) return;
    if (via_family == IPV67_PEER_IPV6 && !via_ipv6) return;
    off = 0;
    while (off + sizeof(entry) <= plen) {
        memcpy(&entry, payload + off, sizeof(entry));
        entry.addr[IPV67_ADDR_STR_MAX - 1] = '\0';
        if (ipv67_addr_parse(entry.addr, &dest) == IPV67_ERR_OK) {
            key_owns_dest = 0;
            if (ipv67_identity_key_present(entry.public_key)) {
                if (ipv67_asn_claim_routes_key_addr(entry.public_key, &dest)) key_owns_dest = 1;
            }
            if (!key_owns_dest) {
                off += sizeof(entry);
                continue;
            }
            if (entry.hops > 0 && entry.hops < IPV67_MAX_HOPS && entry.metric > 0 && (!ipv67_self_set || !ipv67_addr_eq(&dest, &ipv67_self))) {
                punch_request = 0;
                same_endpoint = 0;
                entry_is_source_key = crypto_constant_compare(entry.public_key, source_key, IPV67_IDENTITY_SIZE) == 0;
                if (entry.family == IPV67_PEER_IPV4 && via_family == IPV67_PEER_IPV4 && entry.ipv4 == via_ipv4 && entry.port == via_port) same_endpoint = 1;
                if (entry.family == IPV67_PEER_IPV6 && via_family == IPV67_PEER_IPV6 && via_ipv6 && ipv6_addr_eq_raw(&entry.ipv6, via_ipv6) && entry.port == via_port) same_endpoint = 1;
                if (same_endpoint && entry.hops == 1 && !entry_is_source_key) {
                    off += sizeof(entry);
                    continue;
                }
                if (!same_endpoint && entry.hops == 1) {
                    if (entry.family == IPV67_PEER_IPV4 && entry.ipv4 != 0 && entry.port != 0) peer = ipv67_remember_peer_candidate4(entry.ipv4, entry.port, &dest, entry.public_key);
                    else if (entry.family == IPV67_PEER_IPV6 && entry.port != 0) peer = ipv67_remember_peer_candidate6(&entry.ipv6, entry.port, &dest, entry.public_key);
                    else peer = NULL;
                    if (peer && ipv67_identity_key_set && !peer->session_established) ipv67_send_auth_to_peer(peer, IPV67_TYPE_AUTH_HELLO);
                    if (peer) punch_request = 1;
                }
                hops = entry.hops + 1;
                if (hops > IPV67_MAX_HOPS) hops = IPV67_MAX_HOPS;
                metric = entry.metric;
                if (metric < hops) metric = hops;
                if (metric < IPV67_ROUTE_METRIC_MAX) metric++;
                sequence = entry.sequence;
                ipv67_update_route_ex(&dest, via_family, via_ipv4, via_ipv6, via_port, hops, metric, sequence, entry.public_key);
                if (punch_request) ipv67_send_punch_request(&dest);
                peer = find_peer_addr(&dest);
                if (peer && ipv67_identity_key_present(entry.public_key) && ipv67_peer_public_key_addr_allowed(peer, entry.public_key, &dest)) {
                    memcpy(peer->public_key, entry.public_key, IPV67_IDENTITY_SIZE);
                    ipv67_make_alias_from_key(peer->public_key, peer->alias);
                }
            }
        }
        off += sizeof(entry);
    }
}

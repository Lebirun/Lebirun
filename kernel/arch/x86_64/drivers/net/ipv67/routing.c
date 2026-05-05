#include "internal.h"

static int ipv67_route_uses_peer(const ipv67_route_t *route, const ipv67_peer_t *peer) {
    if (!route || !peer) return 0;
    if (route->next_hop_family != peer->family) return 0;
    if (route->next_hop_port != peer->port) return 0;
    if (peer->family == IPV67_PEER_IPV6) return ipv6_addr_eq_raw(&route->next_hop_ipv6, &peer->ipv6);
    return route->next_hop_ipv4 == peer->ipv4;
}

void ipv67_cleanup_stale(void) {
    uint32_t now;
    int i;
    int j;

    if (!ipv67_current) return;
    now = net_get_ticks();
    if (ipv67_routes && ipv67_current->route_cap > 0) {
        for (i = 0; i < ipv67_current->route_cap; i++) {
            if (ipv67_routes[i].valid && now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) {
                memset(&ipv67_routes[i], 0, sizeof(ipv67_route_t));
                if (ipv67_route_count > 0) ipv67_route_count--;
            }
        }
    }
    if (!ipv67_peers || ipv67_current->peer_cap <= 0) return;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (!ipv67_peers[i].active) continue;
        if (now - ipv67_peers[i].last_seen_ticks <= IPV67_PEER_TTL_TICKS) continue;
        if (ipv67_routes && ipv67_current->route_cap > 0) {
            for (j = 0; j < ipv67_current->route_cap; j++) {
                if (ipv67_routes[j].valid && ipv67_route_uses_peer(&ipv67_routes[j], &ipv67_peers[i])) {
                    memset(&ipv67_routes[j], 0, sizeof(ipv67_route_t));
                    if (ipv67_route_count > 0) ipv67_route_count--;
                }
            }
        }
        memset(&ipv67_peers[i], 0, sizeof(ipv67_peer_t));
        if (ipv67_peer_count_val > 0) ipv67_peer_count_val--;
    }
}

void ipv67_update_route(const ipv67_addr_t *dest, uint8_t family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, uint8_t hops) {
    int i;
    int free_idx;
    int oldest_idx;
    uint32_t oldest;
    uint32_t now;
    uint8_t metric;

    if (!dest) return;
    now = net_get_ticks();
    metric = hops;
    if (metric == 0) metric = 1;
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_addr_eq(&ipv67_routes[i].dest, dest)) {
            if (metric <= ipv67_routes[i].metric || now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS / 2) {
                ipv67_routes[i].next_hop_family = family;
                ipv67_routes[i].next_hop_ipv4 = via_ipv4;
                if (via_ipv6) memcpy(&ipv67_routes[i].next_hop_ipv6, via_ipv6, sizeof(ipv6_addr_t));
                ipv67_routes[i].next_hop_port = via_port;
                ipv67_routes[i].hops = hops;
                ipv67_routes[i].metric = metric;
                ipv67_routes[i].sequence = ++ipv67_route_sequence;
                ipv67_routes[i].age_ticks = now;
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
    ipv67_routes[free_idx].sequence = ++ipv67_route_sequence;
    ipv67_routes[free_idx].age_ticks = now;
}

ipv67_route_t *ipv67_find_route(const ipv67_addr_t *dst) {
    int i;
    ipv67_route_t *best;
    int best_hops;
    uint32_t now;

    ipv67_cleanup_stale();
    best = NULL;
    best_hops = IPV67_MAX_HOPS + 1;
    now = net_get_ticks();
    for (i = 0; i < ipv67_current->route_cap; i++) {
        if (ipv67_routes[i].valid && ipv67_addr_eq(&ipv67_routes[i].dest, dst)) {
            if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
            if (ipv67_routes[i].metric < best_hops) {
                best_hops = ipv67_routes[i].metric;
                best = &ipv67_routes[i];
            }
        }
    }
    return best;
}

int ipv67_send_raw(uint32_t dst_ipv4, uint16_t dst_port,
                           ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t stack_buf[sizeof(ipv67_header_t) + 512];
    uint8_t *buf;
    uint64_t total;
    netif_t *netif;
    ipv67_context_t *saved;
    ipv4_addr_t dst;
    ipv4_addr_t next_hop;
    int ret;
    int heap;
    uint16_t src_port;

    total = sizeof(ipv67_header_t) + plen;
    heap = 0;
    if (total <= sizeof(stack_buf)) {
        buf = stack_buf;
    } else {
        buf = (uint8_t *)kmalloc(total);
        if (!buf) return IPV67_ERR_NOMEM;
        heap = 1;
    }

    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);

    netif = netif_get_default();
    if (!netif) {
        if (heap) kfree(buf);
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
        if (heap) kfree(buf);
        return IPV67_ERR_NOROUTE;
    }

    ipv67_stack_unlock();
    ret = udp_send(netif, dst, src_port, dst_port, buf, (uint64_t)total);
    ipv67_stack_lock();
    ipv67_current = saved;
    if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
    if (heap) kfree(buf);
    if (ret < 0) return IPV67_ERR_NOROUTE;
    return ret;
}

int ipv67_send_raw6(const ipv6_addr_t *dst_ipv6, uint16_t dst_port,
                           ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    uint8_t stack_buf[sizeof(ipv67_header_t) + 512];
    uint8_t *buf;
    uint64_t total;
    netif_t *netif;
    ipv67_context_t *saved;
    ipv6_addr_t dst_copy;
    int ret;
    int heap;
    uint16_t src_port;

    if (!dst_ipv6) return IPV67_ERR_INVAL;
    memcpy(&dst_copy, dst_ipv6, sizeof(dst_copy));

    total = sizeof(ipv67_header_t) + plen;
    heap = 0;
    if (total <= sizeof(stack_buf)) {
        buf = stack_buf;
    } else {
        buf = (uint8_t *)kmalloc(total);
        if (!buf) return IPV67_ERR_NOMEM;
        heap = 1;
    }

    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);

    netif = netif_get_default();
    if (!netif) {
        if (heap) kfree(buf);
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
    if (heap) kfree(buf);
    if (ret < 0) return IPV67_ERR_NOROUTE;
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
    uint8_t stack_buf[sizeof(ipv67_header_t) + 512];
    uint8_t *buf;
    uint64_t total;
    netif_t *netif;
    ipv4_addr_t dst;
    ipv4_addr_t next_hop;
    int ret;
    int heap;

    if (!route || !hdr) return IPV67_ERR_INVAL;
    total = sizeof(ipv67_header_t) + plen;
    heap = 0;
    if (total <= sizeof(stack_buf)) {
        buf = stack_buf;
    } else {
        buf = (uint8_t *)kmalloc(total);
        if (!buf) return IPV67_ERR_NOMEM;
        heap = 1;
    }
    memcpy(buf, hdr, sizeof(ipv67_header_t));
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);
    netif = netif_get_default();
    if (!netif) {
        if (heap) kfree(buf);
        return IPV67_ERR_NOROUTE;
    }
    if (route->next_hop_family == IPV67_PEER_IPV6) {
        ret = udp_send6(netif, route->next_hop_ipv6, src_port, route->next_hop_port, buf, total);
        if (heap) kfree(buf);
        if (ret < 0) return IPV67_ERR_NOROUTE;
        return ret;
    }
    dst.octets[0] = (uint8_t)((route->next_hop_ipv4 >> 24) & 0xFF);
    dst.octets[1] = (uint8_t)((route->next_hop_ipv4 >> 16) & 0xFF);
    dst.octets[2] = (uint8_t)((route->next_hop_ipv4 >>  8) & 0xFF);
    dst.octets[3] = (uint8_t)( route->next_hop_ipv4        & 0xFF);
    next_hop = dst;
    if (!ipv4_is_local(netif, dst)) next_hop = netif->gateway;
    if (ipv4_eq(next_hop, IPV4_ZERO)) {
        if (heap) kfree(buf);
        return IPV67_ERR_NOROUTE;
    }
    ret = udp_send(netif, dst, src_port, route->next_hop_port, buf, total);
    if (heap) kfree(buf);
    if (ret < 0) return IPV67_ERR_NOROUTE;
    return ret;
}

uint16_t ipv67_build_route_adv(uint8_t *buf, uint16_t max_len) {
    ipv67_route_adv_entry_t entry;
    uint16_t off;
    uint32_t now;
    int i;

    if (!buf || max_len < sizeof(ipv67_route_adv_entry_t)) return 0;
    ipv67_cleanup_stale();
    off = 0;
    now = net_get_ticks();
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (off + sizeof(entry) > max_len) break;
        if (!ipv67_peers[i].active || !ipv67_peers[i].addr.zone1[0]) continue;
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
        if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
        memset(&entry, 0, sizeof(entry));
        entry.family = ipv67_routes[i].next_hop_family;
        entry.hops = ipv67_routes[i].hops + 1;
        if (entry.hops > IPV67_MAX_HOPS) continue;
        entry.metric = ipv67_routes[i].metric + 1;
        entry.sequence = ipv67_routes[i].sequence;
        entry.port = ipv67_routes[i].next_hop_port;
        entry.ipv4 = ipv67_routes[i].next_hop_ipv4;
        memcpy(&entry.ipv6, &ipv67_routes[i].next_hop_ipv6, sizeof(ipv6_addr_t));
        ipv67_addr_format(&ipv67_routes[i].dest, entry.addr, sizeof(entry.addr));
        memcpy(buf + off, &entry, sizeof(entry));
        off += sizeof(entry);
    }
    return off;
}

void ipv67_apply_route_adv(uint8_t via_family, uint32_t via_ipv4, const ipv6_addr_t *via_ipv6, uint16_t via_port, const uint8_t *payload, uint16_t plen) {
    ipv67_route_adv_entry_t entry;
    ipv67_addr_t dest;
    ipv67_peer_t *peer;
    uint16_t off;
    uint8_t hops;

    if (!payload) return;
    off = 0;
    while (off + sizeof(entry) <= plen) {
        memcpy(&entry, payload + off, sizeof(entry));
        entry.addr[IPV67_ADDR_STR_MAX - 1] = '\0';
        if (ipv67_addr_parse(entry.addr, &dest) == IPV67_ERR_OK) {
            if (!ipv67_self_set || !ipv67_addr_eq(&dest, &ipv67_self)) {
                hops = entry.hops + 1;
                if (hops > IPV67_MAX_HOPS) hops = IPV67_MAX_HOPS;
                ipv67_update_route(&dest, via_family, via_ipv4, via_ipv6, via_port, hops);
                peer = find_peer_addr(&dest);
                if (peer && entry.public_key[0]) {
                    memcpy(peer->public_key, entry.public_key, IPV67_IDENTITY_SIZE);
                    ipv67_make_alias_from_key(peer->public_key, peer->alias);
                }
            }
        }
        off += sizeof(entry);
    }
}

#include "internal.h"

void ipv67_receive6(const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    uint16_t port;

    if (ipv67_stack_trylock()) {
        port = ipv67_current && ipv67_current->port ? ipv67_current->port : IPV67_PORT_DEFAULT;
        ipv67_stack_unlock();
    } else {
        port = IPV67_PORT_DEFAULT;
    }
    ipv67_receive6_on_port(port, src_ipv6, src_port, packet, len);
}

void ipv67_receive6_on_port(uint16_t local_port, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    if (!ipv67_stack_trylock()) {
        ipv67_rx_enqueue(IPV67_PEER_IPV6, local_port, 0, src_ipv6, src_port, packet, len);
        return;
    }
    ipv67_drain_pending_locked();
    ipv67_receive6_on_port_locked(local_port, src_ipv6, src_port, packet, len);
    ipv67_stack_unlock();
}

void ipv67_receive6_on_port_locked(uint16_t local_port, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
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
    ipv67_auth_payload_t auth;
    ipv67_peer_t peer_copy;
    int i;

    saved = ipv67_current;
    ctx = ipv67_context_get(local_port, 0);
    if (!ctx) return;
    ipv67_current = ctx;

    if (!src_ipv6 || !packet || len < sizeof(ipv67_header_t) || len > 65535) {
        IPV67_RESTORE_RETURN(saved);
    }

    memcpy(&hdr, packet, sizeof(ipv67_header_t));

    if (hdr.magic != IPV67_MAGIC) {
        IPV67_RESTORE_RETURN(saved);
    }
    if (hdr.version != IPV67_PROTO_VERSION) {
        IPV67_RESTORE_RETURN(saved);
    }

    hdr.src[IPV67_ADDR_STR_MAX - 1] = '\0';
    hdr.dst[IPV67_ADDR_STR_MAX - 1] = '\0';

    if (ipv67_addr_parse(hdr.src, &src_addr) != IPV67_ERR_OK) {
        IPV67_RESTORE_RETURN(saved);
    }
    if (ipv67_addr_parse(hdr.dst, &dst_addr) != IPV67_ERR_OK) {
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.hop_limit == 0) {
        IPV67_RESTORE_RETURN(saved);
    }

    payload_len = hdr.payload_len;
    if ((uint64_t)payload_len > len - sizeof(ipv67_header_t)) {
        IPV67_RESTORE_RETURN(saved);
    }
    payload = packet + sizeof(ipv67_header_t);
    p = find_peer6(src_ipv6, src_port);
    if (!ipv67_verify_header(&hdr, payload, payload_len, p)) {
        IPV67_RESTORE_RETURN(saved);
    }
    if (ipv67_packet_replay_seen(p, hdr.type, hdr.sequence, hdr.packet_id)) {
        IPV67_RESTORE_RETURN(saved);
    }

    ipv67_add_peer6(src_ipv6, src_port, &src_addr);
    p = find_peer6(src_ipv6, src_port);
    if (p) {
        memcpy(p->alias, hdr.alias, IPV67_ALIAS_SIZE);
        if (ipv67_auth_key_set) p->authenticated = 1;
        p->missed_probes = 0;
        p->last_seen_ticks = net_get_ticks();
    }
    ipv67_update_route(&src_addr, IPV67_PEER_IPV6, 0, src_ipv6, src_port, 1);
    if (hdr.type == IPV67_TYPE_PEER_REQ || hdr.type == IPV67_TYPE_PEER_ACK || hdr.type == IPV67_TYPE_ROUTE_ADV) {
        ipv67_apply_route_adv(IPV67_PEER_IPV6, 0, src_ipv6, src_port, payload, payload_len);
    }

    if ((hdr.type == IPV67_TYPE_AUTH_HELLO || hdr.type == IPV67_TYPE_AUTH_REPLY || hdr.type == IPV67_TYPE_AUTH_DONE) && payload_len >= sizeof(ipv67_auth_payload_t)) {
        memcpy(&auth, payload, sizeof(auth));
        if (p && auth.public_key[0]) {
            memcpy(p->public_key, auth.public_key, IPV67_IDENTITY_SIZE);
            ipv67_make_alias_from_key(p->public_key, p->alias);
            p->remote_challenge = auth.challenge;
        }
        if (hdr.type == IPV67_TYPE_AUTH_HELLO) {
            if (p) ipv67_send_auth_to_peer(p, IPV67_TYPE_AUTH_REPLY);
            IPV67_RESTORE_RETURN(saved);
        }
        if (hdr.type == IPV67_TYPE_AUTH_REPLY) {
            if (p && ipv67_verify_auth_payload(p, &auth, IPV67_TYPE_AUTH_REPLY)) {
                ipv67_derive_session_key(p);
                ipv67_send_auth_to_peer(p, IPV67_TYPE_AUTH_DONE);
            }
            IPV67_RESTORE_RETURN(saved);
        }
        if (hdr.type == IPV67_TYPE_AUTH_DONE) {
            if (p && ipv67_verify_auth_payload(p, &auth, IPV67_TYPE_AUTH_DONE)) ipv67_derive_session_key(p);
            IPV67_RESTORE_RETURN(saved);
        }
    }

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
        ipv67_prepare_header(&reply_hdr);
        ipv67_sign_header_key(&reply_hdr, NULL, 0, ipv67_signing_key_for_peer(p));
        ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, NULL, 0);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PING && (!ipv67_self_set || ipv67_addr_eq(&dst_addr, &ipv67_self))) {
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
        ipv67_prepare_header(&reply_hdr);
        ipv67_sign_header_key(&reply_hdr, payload, payload_len, ipv67_bootstrap_key);
        ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, payload, payload_len);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PONG && (!ipv67_self_set || ipv67_addr_eq(&dst_addr, &ipv67_self))) {
        if (ipv67_ping_state.active && !ipv67_ping_state.received &&
            ipv67_addr_eq(&src_addr, &ipv67_ping_state.target) &&
            payload_len >= sizeof(uint32_t)) {
            memcpy(&token, payload, sizeof(uint32_t));
            if (token == ipv67_ping_state.token) {
                ipv67_ping_state.received = 1;
                ipv67_ping_state.rtt = net_get_ticks() - ipv67_ping_state.send_time;
            }
        }
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PEER_REQ) {
        if (p) ipv67_advertise_to_peer(p, IPV67_TYPE_PEER_ACK);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PEER_ACK || hdr.type == IPV67_TYPE_ROUTE_ADV) {
        IPV67_RESTORE_RETURN(saved);
    }

    if (ipv67_self_set && !ipv67_addr_eq(&dst_addr, &ipv67_self)) {
        hdr.hop_limit--;
        if (hdr.hop_limit == 0) {
            IPV67_RESTORE_RETURN(saved);
        }
        fwd = ipv67_find_route(&dst_addr);
        if (fwd) {
            ipv67_sign_header_key(&hdr, packet + sizeof(ipv67_header_t), hdr.payload_len, ipv67_bootstrap_key);
            ipv67_send_to_route(fwd, &hdr, packet + sizeof(ipv67_header_t), hdr.payload_len);
        } else {
            for (i = 0; i < ipv67_current->peer_cap; i++) {
                if (!ipv67_peers[i].active) continue;
                if (ipv67_peers[i].family == IPV67_PEER_IPV6 && ipv6_addr_eq_raw(&ipv67_peers[i].ipv6, src_ipv6) && ipv67_peers[i].port == src_port) continue;
                memcpy(&peer_copy, &ipv67_peers[i], sizeof(peer_copy));
                ipv67_sign_header_key(&hdr, packet + sizeof(ipv67_header_t), hdr.payload_len, ipv67_bootstrap_key);
                ipv67_send_to_peer(&peer_copy, &hdr, packet + sizeof(ipv67_header_t), hdr.payload_len);
            }
        }
    }
    ipv67_current = saved;
}


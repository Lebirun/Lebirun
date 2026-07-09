#include "internal.h"

void ipv67_receive(uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    uint16_t port;

    if (ipv67_stack_trylock()) {
        port = ipv67_current && ipv67_current->port ? ipv67_current->port : IPV67_PORT_DEFAULT;
        ipv67_stack_unlock();
    } else {
        port = IPV67_PORT_DEFAULT;
    }
    ipv67_receive_on_port(port, src_ipv4, src_port, packet, len);
}

void ipv67_receive_on_port(uint16_t local_port, uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    if (!ipv67_stack_trylock()) {
        ipv67_rx_enqueue(IPV67_PEER_IPV4, local_port, src_ipv4, NULL, src_port, packet, len);
        return;
    }
    ipv67_drain_pending_locked();
    ipv67_receive_on_port_locked(local_port, src_ipv4, src_port, packet, len);
    ipv67_stack_unlock();
}

void ipv67_receive_on_port_locked(uint16_t local_port, uint32_t src_ipv4, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    ipv67_receive_common_on_port_locked(IPV67_PEER_IPV4, local_port, src_ipv4, NULL, src_port, packet, len);
}

void ipv67_receive_common_on_port_locked(uint8_t family, uint16_t local_port, uint32_t src_ipv4, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
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
    ipv67_peer_t *route_peer;
    ipv67_auth_payload_t auth;
    ipv67_peer_t peer_copy;
    ipv67_peer_t original_peer;
    ipv67_peer_t *existing_peer;
    int i;
    int forwarded;
    int send_ret;
    int peer_auth_ok;
    int route_payload_allowed;
    int auth_ok;
    int auth_type;
    int peer_existed;
    int peer_addr_ok;
    int auth_addr_allowed;
    int direct_reply;
    int routed_source;
    ipv67_ping_state_t *ping_state;

    saved = ipv67_current;
    ctx = ipv67_context_get(local_port, 0);
    if (!ctx) return;
    ipv67_current = ctx;

    if (!packet || len < sizeof(ipv67_header_t) || len > 65535 || (family == IPV67_PEER_IPV6 && !src_ipv6)) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    memcpy(&hdr, packet, sizeof(ipv67_header_t));

    if (hdr.magic != IPV67_MAGIC) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    if (hdr.version != IPV67_PROTO_VERSION) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    if (!ipv67_type_known(hdr.type)) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    hdr.src[IPV67_ADDR_STR_MAX - 1] = '\0';
    hdr.dst[IPV67_ADDR_STR_MAX - 1] = '\0';

    if (ipv67_addr_parse(hdr.src, &src_addr) != IPV67_ERR_OK) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    if (ipv67_addr_parse(hdr.dst, &dst_addr) != IPV67_ERR_OK) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.hop_limit == 0) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    payload_len = hdr.payload_len;
    if ((uint64_t)payload_len > len - sizeof(ipv67_header_t)) {
        IPV67_STATS_INC(malformed_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    payload = packet + sizeof(ipv67_header_t);
    IPV67_STATS_INC(rx_packets);
    if (family == IPV67_PEER_IPV6) p = find_peer6(src_ipv6, src_port);
    else p = find_peer(src_ipv4, src_port);
    auth_type = hdr.type == IPV67_TYPE_AUTH_HELLO || hdr.type == IPV67_TYPE_AUTH_REPLY || hdr.type == IPV67_TYPE_AUTH_DONE;
    if (!ipv67_verify_header(&hdr, payload, payload_len, p)) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    if (!auth_type && ipv67_packet_replay_seen(p, hdr.type, hdr.sequence, hdr.packet_id)) {
        IPV67_STATS_INC(replay_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    peer_auth_ok = 0;
    route_payload_allowed = 0;
    peer_auth_ok = ipv67_header_has_peer_auth(&hdr, payload, payload_len, p);

    if (ipv67_auth_required && !peer_auth_ok && !ipv67_type_allows_unauthenticated(hdr.type)) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    if (!auth_type && !peer_auth_ok && p && p->authenticated && p->session_established) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_HELLO && ipv67_auth_required && !p) {
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
        if (ipv67_sign_header_key(&reply_hdr, NULL, 0, ipv67_signing_key_for_peer(NULL)) < 0) IPV67_RESTORE_RETURN(saved);
        if (family == IPV67_PEER_IPV6) ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, NULL, 0);
        else ipv67_send_raw(src_ipv4, src_port, &reply_hdr, NULL, 0);
        IPV67_RESTORE_RETURN(saved);
    }

    if ((hdr.type == IPV67_TYPE_PEER_REQ || hdr.type == IPV67_TYPE_PEER_ACK) && ipv67_auth_required && !p) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    existing_peer = p ? p : find_peer_addr(&src_addr);
    peer_existed = existing_peer != NULL;
    if (peer_existed) memcpy(&original_peer, existing_peer, sizeof(original_peer));
    if (!auth_type && !peer_auth_ok && existing_peer && existing_peer != p && existing_peer->authenticated && existing_peer->session_established) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }

    routed_source = p && peer_auth_ok && p->addr.zone1[0] && !ipv67_addr_eq(&p->addr, &src_addr);
    if (!routed_source) {
        if (family == IPV67_PEER_IPV6) p = ipv67_learn_peer6(src_ipv6, src_port, &src_addr);
        else p = ipv67_learn_peer4(src_ipv4, src_port, &src_addr);
    }
    if (!p) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    if (!peer_auth_ok) peer_auth_ok = ipv67_header_has_peer_auth(&hdr, payload, payload_len, p);
    p->missed_probes = 0;
    p->last_seen_ticks = net_get_ticks();

    if (auth_type) {
        if (hdr.type == IPV67_TYPE_AUTH_HELLO) IPV67_STATS_INC(auth_hello_rx);
        if (hdr.type == IPV67_TYPE_AUTH_REPLY) IPV67_STATS_INC(auth_reply_rx);
        if (hdr.type == IPV67_TYPE_AUTH_DONE) IPV67_STATS_INC(auth_done_rx);
        if (payload_len < sizeof(ipv67_auth_payload_t)) {
            if (peer_existed) memcpy(p, &original_peer, sizeof(original_peer));
            else ipv67_remove_peer_by_addr(&src_addr);
            IPV67_STATS_INC(auth_drops);
            IPV67_STATS_INC(auth_payload_fail);
            IPV67_RESTORE_RETURN(saved);
        }
        memcpy(&auth, payload, sizeof(auth));
        auth_ok = 0;
        auth_addr_allowed = 0;
        if (p && ipv67_identity_key_present(auth.public_key)) {
            if (!p->session_established) auth_addr_allowed = 1;
            else if (ipv67_peer_auth_key_addr_allowed(p, auth.public_key, &src_addr)) auth_addr_allowed = 1;
        }
        if (auth_addr_allowed) auth_ok = ipv67_verify_auth_payload(p, &auth, hdr.type);
        if (!auth_ok) {
            if (peer_existed) memcpy(p, &original_peer, sizeof(original_peer));
            else ipv67_remove_peer_by_addr(&src_addr);
            IPV67_STATS_INC(auth_drops);
            IPV67_STATS_INC(auth_payload_fail);
            IPV67_RESTORE_RETURN(saved);
        }
        IPV67_STATS_INC(auth_payload_ok);
        if (ipv67_packet_replay_seen(p, hdr.type, hdr.sequence, hdr.packet_id)) {
            if (peer_existed) memcpy(p, &original_peer, sizeof(original_peer));
            else ipv67_remove_peer_by_addr(&src_addr);
            IPV67_STATS_INC(replay_drops);
            IPV67_RESTORE_RETURN(saved);
        }
        if (p) {
            memcpy(p->public_key, auth.public_key, IPV67_IDENTITY_SIZE);
            ipv67_make_alias_from_key(p->public_key, p->alias);
            p->remote_challenge = auth.challenge;
        }
        if (hdr.type == IPV67_TYPE_AUTH_HELLO) {
            if (p) ipv67_send_auth_to_peer(p, IPV67_TYPE_AUTH_REPLY);
            IPV67_RESTORE_RETURN(saved);
        }
        if (hdr.type == IPV67_TYPE_AUTH_REPLY) {
            if (p) {
                ipv67_derive_session_key(p);
                if (p->authenticated && p->session_established) IPV67_STATS_INC(auth_sessions);
                else IPV67_STATS_INC(auth_session_fail);
                if (p->authenticated && p->session_established && ipv67_peer_addr_verified(p)) ipv67_update_route(&src_addr, family, src_ipv4, src_ipv6, src_port, 1);
                ipv67_send_auth_to_peer(p, IPV67_TYPE_AUTH_DONE);
                if (p->authenticated && p->session_established) {
                    ipv67_advertise_to_peer(p, IPV67_TYPE_PEER_REQ);
                    ipv67_advertise_asns_to_peer(p);
                }
            }
            IPV67_RESTORE_RETURN(saved);
        }
        if (hdr.type == IPV67_TYPE_AUTH_DONE) {
            if (p) {
                ipv67_derive_session_key(p);
                if (p->authenticated && p->session_established) IPV67_STATS_INC(auth_sessions);
                else IPV67_STATS_INC(auth_session_fail);
                if (p->authenticated && p->session_established) {
                    if (ipv67_peer_addr_verified(p)) ipv67_update_route(&src_addr, family, src_ipv4, src_ipv6, src_port, 1);
                    ipv67_advertise_to_peer(p, IPV67_TYPE_PEER_REQ);
                    ipv67_advertise_asns_to_peer(p);
                }
            }
            IPV67_RESTORE_RETURN(saved);
        }
    }

    peer_addr_ok = p && ipv67_peer_addr_verified(p);
    if (peer_auth_ok && p && p->authenticated && p->session_established && !peer_addr_ok && hdr.type != IPV67_TYPE_ASN_ADV && hdr.type != IPV67_TYPE_PEER_REQ) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    route_payload_allowed = peer_auth_ok && p && p->authenticated && p->session_established && peer_addr_ok;
    if (route_payload_allowed) ipv67_update_route(&src_addr, family, src_ipv4, src_ipv6, src_port, 1);
    if ((hdr.type == IPV67_TYPE_PEER_REQ || hdr.type == IPV67_TYPE_PEER_ACK || hdr.type == IPV67_TYPE_ROUTE_ADV) && route_payload_allowed) {
        ipv67_apply_route_adv(family, src_ipv4, src_ipv6, src_port, payload, payload_len, peer_auth_ok, p->public_key);
    }
    if (hdr.type == IPV67_TYPE_ASN_ADV) {
        if (peer_auth_ok && p && p->authenticated && p->session_established) {
            ipv67_apply_asn_adv(payload, payload_len, p->public_key, p->alias, 1);
            if (ipv67_peer_addr_verified(p)) {
                ipv67_update_route(&src_addr, family, src_ipv4, src_ipv6, src_port, 1);
                ipv67_advertise_to_peer(p, IPV67_TYPE_PEER_REQ);
            }
        }
        else IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
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
        if (ipv67_sign_header_key(&reply_hdr, NULL, 0, ipv67_signing_key_for_peer(p)) < 0) IPV67_RESTORE_RETURN(saved);
        if (family == IPV67_PEER_IPV6) ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, NULL, 0);
        else ipv67_send_raw(src_ipv4, src_port, &reply_hdr, NULL, 0);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PING && (!ipv67_self_set || ipv67_addr_eq(&dst_addr, &ipv67_self))) {
        if (!peer_auth_ok || !p || !p->authenticated || !p->session_established || !peer_addr_ok) {
            IPV67_STATS_INC(auth_drops);
            IPV67_RESTORE_RETURN(saved);
        }
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
        direct_reply = p && ipv67_addr_eq(&src_addr, &p->addr);
        if (direct_reply) {
            if (ipv67_sign_header_key(&reply_hdr, payload, payload_len, ipv67_signing_key_for_peer(p)) < 0) IPV67_RESTORE_RETURN(saved);
            if (family == IPV67_PEER_IPV6) ipv67_send_raw6(src_ipv6, src_port, &reply_hdr, payload, payload_len);
            else ipv67_send_raw(src_ipv4, src_port, &reply_hdr, payload, payload_len);
        } else {
            fwd = ipv67_find_route(&src_addr);
            if (fwd) route_peer = ipv67_peer_for_route(fwd);
            else route_peer = p;
            if (route_peer && route_peer->authenticated && route_peer->session_established && ipv67_peer_addr_verified(route_peer) && ipv67_identity_key_present(route_peer->public_key)) {
                if (ipv67_sign_header_key(&reply_hdr, payload, payload_len, ipv67_signing_key_for_peer(route_peer)) < 0) IPV67_RESTORE_RETURN(saved);
                if (fwd) send_ret = ipv67_send_to_route(fwd, &reply_hdr, payload, payload_len);
                else send_ret = ipv67_send_to_peer(route_peer, &reply_hdr, payload, payload_len);
                if (send_ret < 0) IPV67_STATS_INC(no_route_drops);
            } else {
                IPV67_STATS_INC(no_route_drops);
            }
        }
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PONG && (!ipv67_self_set || ipv67_addr_eq(&dst_addr, &ipv67_self))) {
        if (!peer_auth_ok || !p || !p->authenticated || !p->session_established || !peer_addr_ok) {
            IPV67_STATS_INC(auth_drops);
            IPV67_RESTORE_RETURN(saved);
        }
        ping_state = ipv67_ping_state_get(0);
        if (ping_state && ping_state->active && !ping_state->received &&
            ipv67_addr_eq(&src_addr, &ping_state->target) &&
            payload_len >= sizeof(uint32_t)) {
            memcpy(&token, payload, sizeof(uint32_t));
            if (token == ping_state->token) {
                ping_state->received = 1;
                ping_state->rtt = net_get_ticks() - ping_state->send_time;
            }
        }
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PUNCH && (!peer_auth_ok || !p || !p->session_established || !p->authenticated || !peer_addr_ok)) {
        IPV67_STATS_INC(auth_drops);
        IPV67_RESTORE_RETURN(saved);
    }
    if (hdr.type == IPV67_TYPE_PUNCH) {
        ipv67_handle_punch(&hdr, &src_addr, &dst_addr, payload, payload_len, family, src_ipv4, src_ipv6, src_port, 1);
        if (!ipv67_self_set || ipv67_addr_eq(&dst_addr, &ipv67_self)) IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PEER_REQ) {
        if (route_payload_allowed) ipv67_advertise_to_peer(p, IPV67_TYPE_PEER_ACK);
        else if (p && ipv67_identity_key_set && !p->session_established) ipv67_send_auth_to_peer(p, IPV67_TYPE_AUTH_HELLO);
        IPV67_RESTORE_RETURN(saved);
    }

    if (hdr.type == IPV67_TYPE_PEER_ACK || hdr.type == IPV67_TYPE_ROUTE_ADV) {
        if (hdr.type == IPV67_TYPE_PEER_ACK && !route_payload_allowed && p && ipv67_identity_key_set && !p->session_established) ipv67_send_auth_to_peer(p, IPV67_TYPE_AUTH_HELLO);
        IPV67_RESTORE_RETURN(saved);
    }

    if (ipv67_self_set && !ipv67_addr_eq(&dst_addr, &ipv67_self)) {
        if (!peer_auth_ok || !p || !p->authenticated || !p->session_established || !peer_addr_ok) {
            IPV67_STATS_INC(auth_drops);
            IPV67_RESTORE_RETURN(saved);
        }
        hdr.hop_limit--;
        if (hdr.hop_limit == 0) {
            IPV67_RESTORE_RETURN(saved);
        }
        ipv67_prepare_header(&hdr);
        fwd = ipv67_find_route(&dst_addr);
        forwarded = 0;
        if (fwd && ((family == IPV67_PEER_IPV6 && !ipv67_route_matches_endpoint6(fwd, src_ipv6, src_port)) || (family != IPV67_PEER_IPV6 && !ipv67_route_matches_endpoint4(fwd, src_ipv4, src_port)))) {
            route_peer = ipv67_peer_for_route(fwd);
            if (route_peer && route_peer->authenticated && route_peer->session_established && ipv67_identity_key_present(route_peer->public_key) && ipv67_peer_addr_verified(route_peer) && (!ipv67_self_set || !ipv67_addr_eq(&route_peer->addr, &ipv67_self))) {
                send_ret = ipv67_sign_header_key(&hdr, packet + sizeof(ipv67_header_t), hdr.payload_len, ipv67_signing_key_for_peer(route_peer));
                if (send_ret >= 0) {
                    send_ret = ipv67_send_to_route(fwd, &hdr, packet + sizeof(ipv67_header_t), hdr.payload_len);
                    if (send_ret >= 0) forwarded++;
                }
            }
        } else {
            for (i = 0; i < ipv67_current->peer_cap; i++) {
                if (!ipv67_peers[i].active) continue;
                if (!ipv67_peers[i].authenticated || !ipv67_peers[i].session_established) continue;
                if (!ipv67_peer_addr_verified(&ipv67_peers[i])) continue;
                if (!ipv67_identity_key_present(ipv67_peers[i].public_key)) continue;
                if (family == IPV67_PEER_IPV6 && ipv67_peers[i].family == IPV67_PEER_IPV6 && ipv6_addr_eq_raw(&ipv67_peers[i].ipv6, src_ipv6) && ipv67_peers[i].port == src_port) continue;
                if (family != IPV67_PEER_IPV6 && ipv67_peers[i].family == IPV67_PEER_IPV4 && ipv67_peers[i].ipv4 == src_ipv4 && ipv67_peers[i].port == src_port) continue;
                if (ipv67_self_set && ipv67_peers[i].addr.zone1[0] && ipv67_addr_eq(&ipv67_peers[i].addr, &ipv67_self)) continue;
                memcpy(&peer_copy, &ipv67_peers[i], sizeof(peer_copy));
                send_ret = ipv67_sign_header_key(&hdr, packet + sizeof(ipv67_header_t), hdr.payload_len, ipv67_signing_key_for_peer(&peer_copy));
                if (send_ret >= 0) {
                    send_ret = ipv67_send_to_peer(&peer_copy, &hdr, packet + sizeof(ipv67_header_t), hdr.payload_len);
                    if (send_ret >= 0) forwarded++;
                }
            }
        }
        if (forwarded > 0) IPV67_STATS_ADD(forwarded_packets, forwarded);
        else IPV67_STATS_INC(no_route_drops);
    }
    ipv67_current = saved;
}

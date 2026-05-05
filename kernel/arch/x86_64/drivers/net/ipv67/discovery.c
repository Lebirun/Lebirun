#include "internal.h"

int ipv67_advertise_to_peer(const ipv67_peer_t *peer, uint8_t type) {
    ipv67_header_t hdr;
    ipv67_peer_t peer_copy;
    uint8_t payload_stack[512];
    uint8_t *payload;
    uint64_t payload_size;
    uint16_t plen;
    char self_str[IPV67_ADDR_STR_MAX];
    int count;
    int ret;
    int heap;

    if (!peer || !ipv67_self_set) return IPV67_ERR_INVAL;
    memcpy(&peer_copy, peer, sizeof(peer_copy));
    ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str));
    count = ipv67_peer_count_val + ipv67_route_count;
    if (count < 1) count = 1;
    payload_size = sizeof(ipv67_route_adv_entry_t) * (uint64_t)count;
    if (payload_size > 1024) payload_size = 1024;
    heap = 0;
    if (payload_size <= sizeof(payload_stack)) {
        payload = payload_stack;
    } else {
        payload = (uint8_t *)kmalloc(payload_size);
        if (!payload) return IPV67_ERR_NOMEM;
        heap = 1;
    }
    plen = ipv67_build_route_adv(payload, (uint16_t)payload_size);
    memset(&hdr, 0, sizeof(ipv67_header_t));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = type;
    hdr.payload_len = plen;
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, self_str, strlen(self_str) + 1);
    if (peer_copy.addr.zone1[0]) ipv67_addr_format(&peer_copy.addr, hdr.dst, sizeof(hdr.dst));
    else memcpy(hdr.dst, self_str, strlen(self_str) + 1);
    ipv67_prepare_header(&hdr);
    if (ipv67_is_bootstrap_type(type)) ipv67_sign_header_key(&hdr, payload, plen, ipv67_bootstrap_key);
    else ipv67_sign_header_key(&hdr, payload, plen, ipv67_signing_key_for_peer(&peer_copy));
    ret = ipv67_send_to_peer(&peer_copy, &hdr, payload, plen);
    if (heap) kfree(payload);
    return ret;
}

void ipv67_make_auth_payload(ipv67_peer_t *peer, ipv67_auth_payload_t *auth, uint8_t type) {
    uint8_t proof_data[IPV67_IDENTITY_SIZE * 2 + sizeof(uint64_t) * 2 + 1];
    uint64_t local_challenge;
    uint64_t remote_challenge;

    if (!peer || !auth) return;
    memset(auth, 0, sizeof(*auth));
    if (!ipv67_identity_key_set) return;
    if (peer->local_challenge == 0) peer->local_challenge = rng_get_u64() ^ pit_get_ticks();
    local_challenge = peer->local_challenge;
    remote_challenge = peer->remote_challenge;
    memcpy(auth->public_key, ipv67_identity_public, IPV67_IDENTITY_SIZE);
    auth->challenge = local_challenge;
    memset(proof_data, 0, sizeof(proof_data));
    memcpy(proof_data, ipv67_identity_public, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE, peer->public_key, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2, &local_challenge, sizeof(local_challenge));
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2 + sizeof(local_challenge), &remote_challenge, sizeof(remote_challenge));
    proof_data[sizeof(proof_data) - 1] = type;
    hmac_sha256(ipv67_bootstrap_key, IPV67_AUTH_KEY_SIZE, proof_data, sizeof(proof_data), auth->proof);
}

int ipv67_verify_auth_payload(ipv67_peer_t *peer, const ipv67_auth_payload_t *auth, uint8_t type) {
    uint8_t proof_data[IPV67_IDENTITY_SIZE * 2 + sizeof(uint64_t) * 2 + 1];
    uint8_t expected[IPV67_SIG_SIZE];
    uint64_t local_challenge;
    uint64_t remote_challenge;

    if (!peer || !auth) return 0;
    local_challenge = auth->challenge;
    remote_challenge = peer->local_challenge;
    memset(proof_data, 0, sizeof(proof_data));
    memcpy(proof_data, auth->public_key, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE, ipv67_identity_public, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2, &local_challenge, sizeof(local_challenge));
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2 + sizeof(local_challenge), &remote_challenge, sizeof(remote_challenge));
    proof_data[sizeof(proof_data) - 1] = type;
    hmac_sha256(ipv67_bootstrap_key, IPV67_AUTH_KEY_SIZE, proof_data, sizeof(proof_data), expected);
    return crypto_constant_compare(expected, auth->proof, IPV67_SIG_SIZE) == 0;
}

int ipv67_send_auth_to_peer(ipv67_peer_t *peer, uint8_t type) {
    ipv67_header_t hdr;
    ipv67_auth_payload_t auth;
    char self_str[IPV67_ADDR_STR_MAX];

    if (!peer || !ipv67_self_set || !ipv67_identity_key_set) return IPV67_ERR_INVAL;
    ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str));
    ipv67_make_auth_payload(peer, &auth, type);
    memset(&hdr, 0, sizeof(ipv67_header_t));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = type;
    hdr.payload_len = sizeof(auth);
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, self_str, strlen(self_str) + 1);
    if (peer->addr.zone1[0]) ipv67_addr_format(&peer->addr, hdr.dst, sizeof(hdr.dst));
    else memcpy(hdr.dst, self_str, strlen(self_str) + 1);
    ipv67_prepare_header(&hdr);
    ipv67_sign_header_key(&hdr, (const uint8_t *)&auth, sizeof(auth), ipv67_bootstrap_key);
    peer->handshake_ticks = net_get_ticks();
    return ipv67_send_to_peer(peer, &hdr, (const uint8_t *)&auth, sizeof(auth));
}

int ipv67_probe_peers(void) {
    int i;
    int sent;
    int ret;
    int count;
    uint32_t now;
    ipv67_peer_t snapshot_stack[4];
    ipv67_peer_t *snapshot;
    ipv67_peer_t *live;
    int heap;

    if (!ipv67_self_set) return IPV67_ERR_INVAL;

    ipv67_cleanup_stale();
    if (!ipv67_current || !ipv67_peers || ipv67_current->peer_cap <= 0) return IPV67_ERR_NOPEER;
    count = ipv67_peer_count();
    if (count <= 0) return IPV67_ERR_NOPEER;
    heap = 0;
    if (count <= 4) {
        snapshot = snapshot_stack;
    } else {
        snapshot = (ipv67_peer_t *)kmalloc(sizeof(ipv67_peer_t) * count);
        if (!snapshot) return IPV67_ERR_NOMEM;
        heap = 1;
    }
    count = ipv67_get_peers(snapshot, count);
    if (count <= 0) {
        if (heap) kfree(snapshot);
        return IPV67_ERR_NOPEER;
    }
    sent = 0;
    ret = IPV67_ERR_NOPEER;
    now = net_get_ticks();
    for (i = 0; i < count; i++) {
        if (ipv67_self_set && ipv67_addr_eq(&snapshot[i].addr, &ipv67_self)) {
            sent++;
            ret = IPV67_ERR_OK;
        } else {
            if (now - snapshot[i].last_seen_ticks > IPV67_PEER_TTL_TICKS / 4) {
                if (snapshot[i].missed_probes < 255) snapshot[i].missed_probes++;
            }
            if (snapshot[i].family == IPV67_PEER_IPV6) live = find_peer6(&snapshot[i].ipv6, snapshot[i].port);
            else live = find_peer(snapshot[i].ipv4, snapshot[i].port);
            if (live) live->missed_probes = snapshot[i].missed_probes;
            if (ipv67_identity_key_set && !snapshot[i].session_established) {
                ipv67_send_auth_to_peer(&snapshot[i], IPV67_TYPE_AUTH_HELLO);
                if (snapshot[i].family == IPV67_PEER_IPV6) live = find_peer6(&snapshot[i].ipv6, snapshot[i].port);
                else live = find_peer(snapshot[i].ipv4, snapshot[i].port);
                if (live) {
                    live->local_challenge = snapshot[i].local_challenge;
                    live->handshake_ticks = snapshot[i].handshake_ticks;
                }
            }
            ret = ipv67_advertise_to_peer(&snapshot[i], IPV67_TYPE_PEER_REQ);
            if (ret >= 0) sent++;
        }
    }
    if (heap) kfree(snapshot);
    if (sent > 0) return IPV67_ERR_OK;
    return ret;
}

int ipv67_send(const ipv67_addr_t *dst, const uint8_t *data, uint64_t len) {
    ipv67_header_t hdr;
    ipv67_route_t *route;
    ipv67_peer_t *route_peer;
    ipv67_peer_t peer_copy;
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
    ipv67_prepare_header(&hdr);
    ipv67_sign_header(&hdr, data, (uint16_t)len);

    route = ipv67_find_route(dst);
    if (route) {
        route_peer = ipv67_peer_for_route(route);
        ipv67_sign_header_key(&hdr, data, (uint16_t)len, ipv67_signing_key_for_peer(route_peer));
        return ipv67_send_to_route(route, &hdr, data, (uint16_t)len);
    }

    sent = 0;
    ret = IPV67_ERR_NOROUTE;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active) {
            if (ipv67_peers[i].addr.zone1[0] && ipv67_addr_eq(&ipv67_peers[i].addr, &ipv67_self)) continue;
            memcpy(&peer_copy, &ipv67_peers[i], sizeof(peer_copy));
            ipv67_sign_header_key(&hdr, data, (uint16_t)len, ipv67_signing_key_for_peer(&peer_copy));
            ret = ipv67_send_to_peer(&peer_copy, &hdr, data, (uint16_t)len);
            if (ret >= 0) sent++;
        }
    }
    if (sent > 0) return IPV67_ERR_OK;
    return ret;
}

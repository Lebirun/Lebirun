#include "internal.h"

static int ipv67_punch_token_key_for(const ipv67_addr_t *addr, uint8_t key[IPV67_AUTH_KEY_SIZE]) {
    ipv67_route_t *route;
    ipv67_peer_t *peer;
    uint8_t identity_secret[IPV67_SESSION_KEY_SIZE];

    if (!addr || !key || !ipv67_identity_key_set) return 0;
    memset(key, 0, IPV67_AUTH_KEY_SIZE);
    memset(identity_secret, 0, sizeof(identity_secret));
    route = ipv67_find_route(addr);
    if (route && ipv67_identity_key_present(route->public_key)) {
        ipv67_derive_identity_secret(route->public_key, identity_secret);
    } else {
        peer = find_peer_addr(addr);
        if (peer && ipv67_identity_key_present(peer->public_key)) ipv67_derive_identity_secret(peer->public_key, identity_secret);
    }
    if (!ipv67_identity_key_present(identity_secret)) return 0;
    if (ipv67_auth_key_set) hmac_sha256(ipv67_auth_key, IPV67_AUTH_KEY_SIZE, identity_secret, IPV67_SESSION_KEY_SIZE, key);
    else sha256_hash(identity_secret, IPV67_SESSION_KEY_SIZE, key);
    return ipv67_identity_key_present(key);
}

static uint64_t ipv67_punch_token_for(const ipv67_addr_t *dst) {
    uint8_t material[IPV67_ADDR_STR_MAX * 2 + 2];
    uint8_t proof[IPV67_SIG_SIZE];
    uint8_t token_key[IPV67_AUTH_KEY_SIZE];
    char self_str[IPV67_ADDR_STR_MAX];
    char dst_str[IPV67_ADDR_STR_MAX];
    const char *first;
    const char *second;
    uint64_t token;
    int pos;
    int i;

    if (!dst || !ipv67_self_set) return 0;
    if (!ipv67_punch_token_key_for(dst, token_key)) return 0;
    if (ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str)) < 0) return 0;
    if (ipv67_addr_format(dst, dst_str, sizeof(dst_str)) < 0) return 0;
    if (strcmp(self_str, dst_str) <= 0) {
        first = self_str;
        second = dst_str;
    } else {
        first = dst_str;
        second = self_str;
    }
    memset(material, 0, sizeof(material));
    pos = 0;
    for (i = 0; first[i] && pos < (int)sizeof(material); i++) material[pos++] = (uint8_t)first[i];
    if (pos < (int)sizeof(material)) material[pos++] = 0;
    for (i = 0; second[i] && pos < (int)sizeof(material); i++) material[pos++] = (uint8_t)second[i];
    if (pos < (int)sizeof(material)) material[pos++] = 0;
    hmac_sha256(token_key, IPV67_AUTH_KEY_SIZE, material, (size_t)pos, proof);
    token = 0;
    for (i = 0; i < 8; i++) token = (token << 8) | proof[i];
    if (token == 0) token = 1;
    return token;
}

static int ipv67_punch_token_valid(const ipv67_addr_t *origin, uint64_t token) {
    if (!origin || token == 0) return 0;
    return ipv67_punch_token_for(origin) == token;
}

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
    const uint8_t *signing_key;
    int session_ok;

    if (!peer || !ipv67_self_set) return IPV67_ERR_INVAL;
    memcpy(&peer_copy, peer, sizeof(peer_copy));
    ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str));
    session_ok = peer_copy.authenticated && peer_copy.session_established && ipv67_identity_key_present(peer_copy.public_key) && ipv67_peer_addr_verified(&peer_copy);
    count = 0;
    payload_size = 0;
    if (session_ok) {
        count = ipv67_peer_count_val + ipv67_route_count;
        if (count < 1) count = 1;
        payload_size = sizeof(ipv67_route_adv_entry_t) * (uint64_t)count;
        if (payload_size > sizeof(payload_stack)) payload_size = sizeof(payload_stack);
    }
    if (payload_size == 0) {
        payload = NULL;
    } else {
        payload = payload_stack;
    }
    plen = 0;
    if (payload) plen = ipv67_build_route_adv(payload, (uint16_t)payload_size, &peer_copy);
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
    signing_key = ipv67_signing_key_for_peer(&peer_copy);
    if (signing_key) ret = ipv67_sign_header_key(&hdr, payload, plen, signing_key);
    else if (ipv67_is_bootstrap_type(type)) ret = ipv67_sign_header_key(&hdr, payload, plen, ipv67_bootstrap_key);
    else ret = IPV67_ERR_INVAL;
    if (ret < 0) return ret;
    ret = ipv67_send_to_peer(&peer_copy, &hdr, payload, plen);
    return ret;
}

void ipv67_make_auth_payload(ipv67_peer_t *peer, ipv67_auth_payload_t *auth, uint8_t type) {
    uint8_t proof_data[IPV67_IDENTITY_SIZE * 2 + sizeof(uint64_t) * 2 + 1];
    uint8_t identity_secret[IPV67_SESSION_KEY_SIZE];
    uint8_t empty_key[IPV67_IDENTITY_SIZE];
    uint64_t local_challenge;
    uint64_t remote_challenge;
    const uint8_t *proof_key;
    const uint8_t *peer_key;

    if (!peer || !auth) return;
    memset(auth, 0, sizeof(*auth));
    memset(empty_key, 0, sizeof(empty_key));
    if (!ipv67_identity_key_set) return;
    if (peer->local_challenge == 0) peer->local_challenge = rng_get_u64() ^ pit_get_ticks();
    if (peer->local_challenge == 0) peer->local_challenge = rng_get_u64() ^ net_get_ticks() ^ 1;
    local_challenge = peer->local_challenge;
    remote_challenge = peer->remote_challenge;
    memcpy(auth->public_key, ipv67_identity_public, IPV67_IDENTITY_SIZE);
    auth->challenge = local_challenge;
    auth->response_challenge = remote_challenge;
    peer_key = peer->public_key;
    if (type == IPV67_TYPE_AUTH_HELLO && !peer->session_established) peer_key = empty_key;
    memset(proof_data, 0, sizeof(proof_data));
    memcpy(proof_data, ipv67_identity_public, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE, peer_key, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2, &local_challenge, sizeof(local_challenge));
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2 + sizeof(local_challenge), &remote_challenge, sizeof(remote_challenge));
    proof_data[sizeof(proof_data) - 1] = type;
    proof_key = ipv67_auth_key_set ? ipv67_auth_key : ipv67_bootstrap_key;
    hmac_sha256(proof_key, IPV67_AUTH_KEY_SIZE, proof_data, sizeof(proof_data), auth->proof);
    if (ipv67_identity_key_present(peer->public_key)) {
        ipv67_derive_identity_secret(peer->public_key, identity_secret);
        if (ipv67_identity_key_present(identity_secret)) hmac_sha256(identity_secret, IPV67_SESSION_KEY_SIZE, proof_data, sizeof(proof_data), auth->identity_proof);
    }
}

static void ipv67_refresh_stale_handshake(ipv67_peer_t *peer, uint32_t now) {
    uint32_t age;

    if (!peer) return;
    if (peer->session_established) return;
    if (peer->handshake_ticks == 0) return;
    age = now - peer->handshake_ticks;
    if (age <= IPV67_PEER_TTL_TICKS / 8) return;
    peer->local_challenge = 0;
    peer->remote_challenge = 0;
    peer->authenticated = 0;
}

int ipv67_verify_auth_payload(ipv67_peer_t *peer, const ipv67_auth_payload_t *auth, uint8_t type) {
    uint8_t proof_data[IPV67_IDENTITY_SIZE * 2 + sizeof(uint64_t) * 2 + 1];
    uint8_t expected[IPV67_SIG_SIZE];
    uint8_t identity_expected[IPV67_SIG_SIZE];
    uint8_t identity_secret[IPV67_SESSION_KEY_SIZE];
    uint8_t empty_key[IPV67_IDENTITY_SIZE];
    uint64_t local_challenge;
    uint64_t remote_challenge;
    const uint8_t *proof_key;
    int shared_ok;
    int identity_ok;
    int peer_key_present;

    if (!peer || !auth) return 0;
    if (!ipv67_identity_key_set) return 0;
    if (ipv67_identity_key_set && crypto_constant_compare(auth->public_key, ipv67_identity_public, IPV67_IDENTITY_SIZE) == 0) {
        IPV67_STATS_INC(auth_fail_self);
        return 0;
    }
    local_challenge = auth->challenge;
    remote_challenge = auth->response_challenge;
    peer_key_present = ipv67_identity_key_present(peer->public_key);
    if (local_challenge == 0) {
        IPV67_STATS_INC(auth_fail_challenge);
        return 0;
    }
    if (type != IPV67_TYPE_AUTH_HELLO && remote_challenge == 0) {
        IPV67_STATS_INC(auth_fail_challenge);
        return 0;
    }
    if (remote_challenge != 0 && peer->local_challenge != 0 && remote_challenge != peer->local_challenge) {
        IPV67_STATS_INC(auth_fail_challenge);
        return 0;
    }
    if (type != IPV67_TYPE_AUTH_HELLO && remote_challenge != peer->local_challenge) {
        IPV67_STATS_INC(auth_fail_challenge);
        return 0;
    }
    memset(proof_data, 0, sizeof(proof_data));
    memcpy(proof_data, auth->public_key, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE, ipv67_identity_public, IPV67_IDENTITY_SIZE);
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2, &local_challenge, sizeof(local_challenge));
    memcpy(proof_data + IPV67_IDENTITY_SIZE * 2 + sizeof(local_challenge), &remote_challenge, sizeof(remote_challenge));
    proof_data[sizeof(proof_data) - 1] = type;
    proof_key = ipv67_auth_key_set ? ipv67_auth_key : ipv67_bootstrap_key;
    hmac_sha256(proof_key, IPV67_AUTH_KEY_SIZE, proof_data, sizeof(proof_data), expected);
    shared_ok = crypto_constant_compare(expected, auth->proof, IPV67_SIG_SIZE) == 0;
    identity_ok = 0;
    if (!shared_ok && type == IPV67_TYPE_AUTH_HELLO && (!peer_key_present || !peer->session_established)) {
        memset(empty_key, 0, sizeof(empty_key));
        memset(proof_data, 0, sizeof(proof_data));
        memcpy(proof_data, auth->public_key, IPV67_IDENTITY_SIZE);
        memcpy(proof_data + IPV67_IDENTITY_SIZE, empty_key, IPV67_IDENTITY_SIZE);
        memcpy(proof_data + IPV67_IDENTITY_SIZE * 2, &local_challenge, sizeof(local_challenge));
        memcpy(proof_data + IPV67_IDENTITY_SIZE * 2 + sizeof(local_challenge), &remote_challenge, sizeof(remote_challenge));
        proof_data[sizeof(proof_data) - 1] = type;
        hmac_sha256(proof_key, IPV67_AUTH_KEY_SIZE, proof_data, sizeof(proof_data), expected);
        shared_ok = crypto_constant_compare(expected, auth->proof, IPV67_SIG_SIZE) == 0;
    }
    if (ipv67_auth_key_set && !shared_ok) {
        IPV67_STATS_INC(auth_fail_shared);
        return 0;
    }
    ipv67_derive_identity_secret(auth->public_key, identity_secret);
    if (ipv67_identity_key_present(identity_secret)) {
        hmac_sha256(identity_secret, IPV67_SESSION_KEY_SIZE, proof_data, sizeof(proof_data), identity_expected);
        if (crypto_constant_compare(identity_expected, auth->identity_proof, IPV67_SIG_SIZE) == 0) identity_ok = 1;
    }
    if (identity_ok) return 1;
    if (!ipv67_auth_key_set && shared_ok && type != IPV67_TYPE_AUTH_HELLO) return 1;
    if (type != IPV67_TYPE_AUTH_HELLO) {
        IPV67_STATS_INC(auth_fail_identity);
        return 0;
    }
    if (peer->session_established) {
        IPV67_STATS_INC(auth_fail_identity);
        return 0;
    }
    if (shared_ok) return 1;
    IPV67_STATS_INC(auth_fail_shared);
    return 0;
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
    if (ipv67_sign_header_key(&hdr, (const uint8_t *)&auth, sizeof(auth), ipv67_bootstrap_key) < 0) return IPV67_ERR_TOOLONG;
    peer->handshake_ticks = net_get_ticks();
    return ipv67_send_to_peer(peer, &hdr, (const uint8_t *)&auth, sizeof(auth));
}

static int ipv67_send_punch_payload(const ipv67_addr_t *dst, ipv67_punch_payload_t *payload) {
    ipv67_header_t hdr;
    ipv67_route_t *route;
    ipv67_peer_t *route_peer;
    char self_str[IPV67_ADDR_STR_MAX];
    char dst_str[IPV67_ADDR_STR_MAX];

    if (!dst || !payload || !ipv67_self_set) return IPV67_ERR_INVAL;
    route = ipv67_find_route(dst);
    if (!route) return IPV67_ERR_NOROUTE;
    route_peer = ipv67_peer_for_route(route);
    if (!route_peer || !route_peer->session_established || !route_peer->authenticated || !ipv67_peer_addr_verified(route_peer)) return IPV67_ERR_NOROUTE;
    if (!ipv67_identity_key_present(route_peer->public_key)) return IPV67_ERR_NOROUTE;
    ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str));
    ipv67_addr_format(dst, dst_str, sizeof(dst_str));
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = IPV67_TYPE_PUNCH;
    hdr.payload_len = sizeof(*payload);
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, self_str, strlen(self_str) + 1);
    memcpy(hdr.dst, dst_str, strlen(dst_str) + 1);
    ipv67_prepare_header(&hdr);
    if (ipv67_sign_header_key(&hdr, (const uint8_t *)payload, sizeof(*payload), ipv67_signing_key_for_peer(route_peer)) < 0) return IPV67_ERR_TOOLONG;
    return ipv67_send_to_route(route, &hdr, (const uint8_t *)payload, sizeof(*payload));
}

int ipv67_send_punch_request(const ipv67_addr_t *dst) {
    ipv67_punch_payload_t payload;
    int ret;

    if (!dst || !ipv67_self_set || ipv67_addr_eq(dst, &ipv67_self)) return IPV67_ERR_INVAL;
    memset(&payload, 0, sizeof(payload));
    payload.mode = 0;
    payload.token = ipv67_punch_token_for(dst);
    if (payload.token == 0) return IPV67_ERR_INVAL;
    ipv67_addr_format(&ipv67_self, payload.addr, sizeof(payload.addr));
    ret = ipv67_send_punch_payload(dst, &payload);
    if (ret >= 0) IPV67_STATS_INC(punch_requests_tx);
    return ret;
}

int ipv67_punch(const ipv67_addr_t *dst) {
    return ipv67_send_punch_request(dst);
}

int ipv67_send_punch_observed(const ipv67_addr_t *dst, const ipv67_addr_t *origin, uint8_t family, uint32_t ipv4, const ipv6_addr_t *ipv6, uint16_t port, uint64_t token) {
    ipv67_punch_payload_t payload;
    int ret;

    if (!dst || !origin || !ipv67_self_set) return IPV67_ERR_INVAL;
    memset(&payload, 0, sizeof(payload));
    payload.mode = 1;
    payload.family = family;
    payload.port = port;
    payload.ipv4 = ipv4;
    if (ipv6) memcpy(&payload.ipv6, ipv6, sizeof(ipv6_addr_t));
    payload.token = token;
    ipv67_addr_format(origin, payload.addr, sizeof(payload.addr));
    ret = ipv67_send_punch_payload(dst, &payload);
    if (ret >= 0) IPV67_STATS_INC(punch_observed_tx);
    return ret;
}

void ipv67_handle_punch(const ipv67_header_t *hdr, const ipv67_addr_t *src_addr, const ipv67_addr_t *dst_addr, const uint8_t *payload, uint16_t payload_len, uint8_t rx_family, uint32_t rx_ipv4, const ipv6_addr_t *rx_ipv6, uint16_t rx_port, int peer_session_ok) {
    ipv67_punch_payload_t punch;
    ipv67_addr_t origin;
    ipv67_peer_t *peer;
    ipv67_peer_t *origin_peer;
    ipv67_route_t *origin_route;
    const uint8_t *origin_key;
    int i;
    int nonzero;
    int token_ok;

    if (!hdr || !src_addr || !dst_addr || !payload || payload_len < sizeof(punch)) return;
    if (!peer_session_ok) return;
    IPV67_STATS_INC(punch_packets_rx);
    memcpy(&punch, payload, sizeof(punch));
    punch.addr[IPV67_ADDR_STR_MAX - 1] = '\0';
    if (punch.mode == 0) {
        if (punch.token == 0) return;
        if (ipv67_self_set && !ipv67_addr_eq(dst_addr, &ipv67_self)) {
            ipv67_send_punch_observed(dst_addr, src_addr, rx_family, rx_ipv4, rx_ipv6, rx_port, punch.token);
        }
        return;
    }
    if (punch.mode != 1) return;
    if (!ipv67_self_set || !ipv67_addr_eq(dst_addr, &ipv67_self)) return;
    if (punch.token == 0 || punch.port == 0) return;
    if (ipv67_addr_parse(punch.addr, &origin) != IPV67_ERR_OK) return;
    if (ipv67_addr_eq(&origin, &ipv67_self)) return;
    token_ok = ipv67_punch_token_valid(&origin, punch.token);
    if (!token_ok) return;
    origin_key = NULL;
    origin_route = ipv67_find_route(&origin);
    if (origin_route && ipv67_identity_key_present(origin_route->public_key)) origin_key = origin_route->public_key;
    origin_peer = find_peer_addr(&origin);
    if (!ipv67_identity_key_present(origin_key) && origin_peer && ipv67_identity_key_present(origin_peer->public_key)) origin_key = origin_peer->public_key;
    if (!ipv67_identity_key_present(origin_key)) return;
    peer = NULL;
    if (punch.family == IPV67_PEER_IPV6) {
        nonzero = 0;
        for (i = 0; i < 16; i++) {
            if (punch.ipv6.octets[i] != 0) nonzero = 1;
        }
        if (!nonzero) return;
        peer = ipv67_remember_peer_candidate6(&punch.ipv6, punch.port, &origin, origin_key);
    } else if (punch.family == IPV67_PEER_IPV4) {
        if (punch.ipv4 == 0) return;
        peer = ipv67_remember_peer_candidate4(punch.ipv4, punch.port, &origin, origin_key);
    }
    if (peer) {
        IPV67_STATS_INC(punch_peers_learned);
        if (ipv67_identity_key_set && !peer->session_established) ipv67_send_auth_to_peer(peer, IPV67_TYPE_AUTH_HELLO);
        ipv67_advertise_to_peer(peer, IPV67_TYPE_PEER_REQ);
    }
}

static int ipv67_refresh_route_punches(void) {
    ipv67_route_t route_copy;
    ipv67_peer_t *peer;
    uint32_t now;
    int i;
    int sent;

    if (!ipv67_current || !ipv67_routes || ipv67_current->route_cap <= 0 || !ipv67_self_set) return 0;
    sent = 0;
    now = net_get_ticks();
    for (i = 0; i < ipv67_current->route_cap && sent < 4; i++) {
        if (!ipv67_routes[i].valid) continue;
        if (now - ipv67_routes[i].age_ticks > IPV67_ROUTE_TTL_TICKS) continue;
        if (ipv67_addr_eq(&ipv67_routes[i].dest, &ipv67_self)) continue;
        peer = ipv67_peer_for_route(&ipv67_routes[i]);
        if (!peer || !peer->authenticated || !peer->session_established) continue;
        if (!ipv67_peer_addr_verified(peer)) continue;
        if (!ipv67_identity_key_present(peer->public_key)) continue;
        memcpy(&route_copy, &ipv67_routes[i], sizeof(route_copy));
        if (ipv67_send_punch_request(&route_copy.dest) >= 0) sent++;
    }
    return sent;
}

static void ipv67_probe_io_window(uint32_t timeout_ms) {
    ipv67_context_t *ctx;
    uint64_t timeout_ticks;
    uint64_t start;

    if (!ipv67_current) return;
    ctx = ipv67_current;
    ctx->io_depth++;
    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();
    ipv67_stack_unlock();
    while (pit_get_ticks() - start <= timeout_ticks) {
        __asm__ volatile("sti");
        netif_poll_all();
        ipv67_stack_lock();
        ipv67_current = ctx;
        ipv67_drain_pending_locked();
        ipv67_stack_unlock();
        schedule();
    }
    ipv67_stack_lock();
    ipv67_current = ctx;
    ipv67_drain_pending_locked();
    if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
    ipv67_release_idle_io_state();
}

int ipv67_probe_peers(void) {
    int i;
    int sent;
    int ret;
    int count;
    uint32_t now;
    ipv67_peer_t snapshot;
    ipv67_peer_t *live;
    ipv67_peer_t *auth_peer;

    if (!ipv67_self_set) return IPV67_ERR_INVAL;

    ipv67_cleanup_stale();
    if (!ipv67_current || !ipv67_peers || ipv67_current->peer_cap <= 0) return IPV67_ERR_NOPEER;
    count = ipv67_peer_count();
    if (count <= 0) return IPV67_ERR_NOPEER;
    sent = 0;
    ret = IPV67_ERR_NOPEER;
    now = net_get_ticks();
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (!ipv67_peers[i].active) continue;
        memcpy(&snapshot, &ipv67_peers[i], sizeof(snapshot));
        if (ipv67_self_set && ipv67_addr_eq(&snapshot.addr, &ipv67_self)) {
            sent++;
            ret = IPV67_ERR_OK;
        } else {
            if (now - snapshot.last_seen_ticks > IPV67_PEER_TTL_TICKS / 4) {
                if (snapshot.missed_probes < 255) snapshot.missed_probes++;
            }
            if (snapshot.family == IPV67_PEER_IPV6) live = find_peer6(&snapshot.ipv6, snapshot.port);
            else live = find_peer(snapshot.ipv4, snapshot.port);
            if (live) live->missed_probes = snapshot.missed_probes;
            if (ipv67_identity_key_set && !snapshot.session_established) {
                ipv67_refresh_stale_handshake(&snapshot, now);
                if (live) {
                    live->local_challenge = snapshot.local_challenge;
                    live->remote_challenge = snapshot.remote_challenge;
                    live->authenticated = snapshot.authenticated;
                }
                auth_peer = live ? live : &snapshot;
                ipv67_send_auth_to_peer(auth_peer, IPV67_TYPE_AUTH_HELLO);
                if (snapshot.family == IPV67_PEER_IPV6) live = find_peer6(&snapshot.ipv6, snapshot.port);
                else live = find_peer(snapshot.ipv4, snapshot.port);
                if (live) {
                    memcpy(&snapshot, live, sizeof(snapshot));
                }
                ipv67_drain_pending_locked();
                if (snapshot.family == IPV67_PEER_IPV6) live = find_peer6(&snapshot.ipv6, snapshot.port);
                else live = find_peer(snapshot.ipv4, snapshot.port);
                if (live) memcpy(&snapshot, live, sizeof(snapshot));
            }
            ret = ipv67_advertise_to_peer(&snapshot, IPV67_TYPE_PEER_REQ);
            if (ret >= 0) ipv67_drain_pending_locked();
            ipv67_advertise_asns_to_peer(&snapshot);
            ipv67_drain_pending_locked();
            if (ret >= 0) sent++;
        }
    }
    sent += ipv67_refresh_route_punches();
    ipv67_drain_pending_locked();
    if (sent > 0) ipv67_probe_io_window(500);
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
    ret = ipv67_sign_header(&hdr, data, (uint16_t)len);
    if (ret < 0) return ret;

    route = ipv67_find_route(dst);
    if (route) {
        route_peer = ipv67_peer_for_route(route);
        if (route_peer && route_peer->authenticated && route_peer->session_established && ipv67_identity_key_present(route_peer->public_key) && ipv67_peer_addr_verified(route_peer)) {
            ret = ipv67_sign_header_key(&hdr, data, (uint16_t)len, ipv67_signing_key_for_peer(route_peer));
            if (ret < 0) return ret;
            return ipv67_send_to_route(route, &hdr, data, (uint16_t)len);
        }
    }

    sent = 0;
    ret = IPV67_ERR_NOROUTE;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (ipv67_peers[i].active) {
            if (!ipv67_peers[i].authenticated || !ipv67_peers[i].session_established) continue;
            if (!ipv67_peer_addr_verified(&ipv67_peers[i])) continue;
            if (!ipv67_identity_key_present(ipv67_peers[i].public_key)) continue;
            if (ipv67_peers[i].addr.zone1[0] && ipv67_addr_eq(&ipv67_peers[i].addr, &ipv67_self)) continue;
            memcpy(&peer_copy, &ipv67_peers[i], sizeof(peer_copy));
            ret = ipv67_sign_header_key(&hdr, data, (uint16_t)len, ipv67_signing_key_for_peer(&peer_copy));
            if (ret < 0) return ret;
            ret = ipv67_send_to_peer(&peer_copy, &hdr, data, (uint16_t)len);
            if (ret >= 0) sent++;
        }
    }
    if (sent > 0) return IPV67_ERR_OK;
    return ret;
}

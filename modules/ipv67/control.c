#include "internal.h"

int ipv67_ping(const ipv67_addr_t *dst, uint32_t timeout_ms) {
    ipv67_header_t hdr;
    ipv67_route_t *route;
    ipv67_route_t route_copy;
    ipv67_context_t *ping_ctx;
    ipv67_peer_t *route_peer;
    ipv67_peer_t peer_copy;
    char dst_str[IPV67_ADDR_STR_MAX];
    char src_str[IPV67_ADDR_STR_MAX];
    int i;
    int sent;
    int ret;
    uint32_t token;
    uint64_t timeout_ticks;
    uint64_t start;
    ipv67_ping_state_t *state;

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
    ipv67_prepare_header(&hdr);

    token = (uint32_t)(net_get_ticks() ^ pit_get_ticks() ^ rng_get_u64());
    state = ipv67_ping_state_get(1);
    if (!state) return IPV67_ERR_NOMEM;
    memcpy(&state->target, dst, sizeof(ipv67_addr_t));
    state->token = token;
    state->send_time = net_get_ticks();
    state->rtt = 0;
    state->active = 1;
    state->received = 0;
    ping_ctx = ipv67_current;
    ping_ctx->io_depth++;

    route = ipv67_find_route(dst);
    if (route) {
        memcpy(&route_copy, route, sizeof(route_copy));
        route_peer = ipv67_peer_for_route(route);
        ret = ipv67_sign_header_key(&hdr, (const uint8_t *)&token, sizeof(token), ipv67_signing_key_for_peer(route_peer));
        if (ret < 0) {
            if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
            state->active = 0;
            ipv67_release_idle_io_state();
            return ret;
        }
        ret = ipv67_send_to_route(&route_copy, &hdr, (const uint8_t *)&token, sizeof(token));
        if (ret < 0) {
            ipv67_current = ping_ctx;
            if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
            state->active = 0;
            ipv67_release_idle_io_state();
            return ret;
        }
        ipv67_drain_pending_locked();
        timeout_ticks = pit_ms_to_ticks(timeout_ms);
        start = pit_get_ticks();
        ipv67_stack_unlock();
        while (ping_ctx->ping_state && !ping_ctx->ping_state->received) {
            __asm__ volatile("sti");
            netif_poll_all();
            ipv67_stack_lock();
            ipv67_current = ping_ctx;
            ipv67_drain_pending_locked();
            ipv67_stack_unlock();
            if (pit_get_ticks() - start > timeout_ticks) {
                ipv67_stack_lock();
                ipv67_current = ping_ctx;
                if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
                state->active = 0;
                ipv67_release_idle_io_state();
                return IPV67_ERR_TIMEOUT;
            }
            schedule();
        }
        ipv67_stack_lock();
        ipv67_current = ping_ctx;
        if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
        state->active = 0;
        ret = (int)state->rtt;
        ipv67_release_idle_io_state();
        return ret;
    } else {
        sent = 0;
        ret = IPV67_ERR_NOROUTE;
        for (i = 0; i < ipv67_current->peer_cap; i++) {
            if (ipv67_peers[i].active) {
                if (!ipv67_peers[i].authenticated || !ipv67_peers[i].session_established) continue;
                if (!ipv67_peer_addr_verified(&ipv67_peers[i])) continue;
                if (!ipv67_identity_key_present(ipv67_peers[i].public_key)) continue;
                memcpy(&peer_copy, &ipv67_peers[i], sizeof(peer_copy));
                ret = ipv67_sign_header_key(&hdr, (const uint8_t *)&token, sizeof(token), ipv67_signing_key_for_peer(&peer_copy));
                if (ret < 0) continue;
                ret = ipv67_send_to_peer(&peer_copy, &hdr, (const uint8_t *)&token, sizeof(token));
                if (ret >= 0) sent++;
            }
        }
        if (sent > 0) ret = IPV67_ERR_OK;
    }

    if (ret < 0) {
        if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
        state->active = 0;
        ipv67_release_idle_io_state();
        return ret;
    }

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();
    ipv67_stack_unlock();
    while (ping_ctx->ping_state && !ping_ctx->ping_state->received) {
        __asm__ volatile("sti");
        netif_poll_all();
        ipv67_stack_lock();
        ipv67_current = ping_ctx;
        ipv67_drain_pending_locked();
        ipv67_stack_unlock();
        if (pit_get_ticks() - start > timeout_ticks) {
            ipv67_stack_lock();
            ipv67_current = ping_ctx;
            if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
            state->active = 0;
            ipv67_release_idle_io_state();
            return IPV67_ERR_TIMEOUT;
        }
        schedule();
    }

    ipv67_stack_lock();
    ipv67_current = ping_ctx;
    if (ipv67_current && ipv67_current->io_depth > 0) ipv67_current->io_depth--;
    state->active = 0;
    ret = (int)state->rtt;
    ipv67_release_idle_io_state();
    return ret;
}

int ipv67_init(void) {
    ipv67_context_t *ctx;
    uint16_t port;

    port = ipv67_current && ipv67_current->port ? ipv67_current->port : IPV67_PORT_DEFAULT;
    ctx = ipv67_context_get(port, 1);
    if (!ctx) return IPV67_ERR_NOMEM;
    if (ctx->io_depth > 0) return IPV67_ERR_BUSY;
    ipv67_current = ctx;
    ipv67_rx_flush_port(port);
    ipv67_context_reset(ctx, port);
    return IPV67_ERR_OK;
}

int ipv67_set_port(uint16_t port) {
    return ipv67_use_port(port);
}

int ipv67_set_auth_key(const uint8_t *key, uint64_t len) {
    ipv67_auth_state_t *state;

    if (!key || len != IPV67_AUTH_KEY_SIZE) return IPV67_ERR_INVAL;
    state = ipv67_auth_state_get(1);
    if (!state) return IPV67_ERR_NOMEM;
    memcpy(state->auth_key, key, IPV67_AUTH_KEY_SIZE);
    state->auth_key_set = 1;
    ipv67_next_sequence = rng_get_u64();
    return IPV67_ERR_OK;
}

int ipv67_set_identity_key(const uint8_t *key, uint64_t len) {
    ipv67_auth_state_t *state;

    if (!key || len != IPV67_IDENTITY_SIZE) return IPV67_ERR_INVAL;
    if (!ipv67_identity_selftest()) return IPV67_ERR_INVAL;
    state = ipv67_auth_state_get(1);
    if (!state) return IPV67_ERR_NOMEM;
    memcpy(state->identity_key, key, IPV67_IDENTITY_SIZE);
    ipv67_derive_public_identity(state->identity_key, state->identity_public);
    state->identity_key_set = 1;
    ipv67_next_sequence = rng_get_u64();
    return IPV67_ERR_OK;
}

int ipv67_set_auth_required(int required) {
    if (required && !ipv67_identity_key_set) return IPV67_ERR_INVAL;
    ipv67_auth_required = required ? 1 : 0;
    return IPV67_ERR_OK;
}

int ipv67_get_auth_required(void) {
    return ipv67_auth_required;
}

uint16_t ipv67_get_port(void) {
    return ipv67_port;
}

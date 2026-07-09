#include "internal.h"

static void ipv67_context_free_members(ipv67_context_t *ctx) {
    if (!ctx) return;
    if (ctx->peers) kfree(ctx->peers);
    if (ctx->routes) kfree(ctx->routes);
    if (ctx->asns) kfree(ctx->asns);
    if (ctx->stats) kfree(ctx->stats);
    if (ctx->auth_state) kfree(ctx->auth_state);
    if (ctx->ping_state) kfree(ctx->ping_state);
}

void ipv67_context_reset(ipv67_context_t *ctx, uint16_t port) {
    if (!ctx) return;
    if (ctx->io_depth > 0) return;
    ipv67_context_free_members(ctx);
    memset(ctx, 0, sizeof(ipv67_context_t));
    ctx->port = port ? port : IPV67_PORT_DEFAULT;
    ctx->active = 1;
}

int ipv67_context_destroy(uint16_t port) {
    ipv67_context_t *ctx;
    int i;

    if (port == 0) port = IPV67_PORT_DEFAULT;
    for (i = 0; i < ipv67_context_cap; i++) {
        ctx = ipv67_contexts[i];
        if (!ctx || !ctx->active || ctx->port != port) continue;
        if (ctx->io_depth > 0) return IPV67_ERR_BUSY;
        ipv67_context_free_members(ctx);
        if (ipv67_current == ctx) ipv67_current = NULL;
        kfree(ctx);
        ipv67_contexts[i] = NULL;
        if (ipv67_context_count > 0) ipv67_context_count--;
        if (ipv67_context_count == 0) {
            kfree(ipv67_contexts);
            ipv67_contexts = NULL;
            ipv67_context_cap = 0;
        }
        ipv67_rx_release_empty_storage();
        return IPV67_ERR_OK;
    }
    if (ipv67_context_count == 0) ipv67_rx_release_empty_storage();
    return IPV67_ERR_NOPEER;
}

void ipv67_context_destroy_all(void) {
    ipv67_context_t *ctx;
    int i;

    for (i = 0; i < ipv67_context_cap; i++) {
        ctx = ipv67_contexts[i];
        if (!ctx || !ctx->active) continue;
        ipv67_context_free_members(ctx);
        kfree(ctx);
        ipv67_contexts[i] = NULL;
    }
    if (ipv67_contexts) kfree(ipv67_contexts);
    ipv67_contexts = NULL;
    ipv67_context_cap = 0;
    ipv67_context_count = 0;
    ipv67_current = NULL;
    ipv67_rx_release_empty_storage();
}

static ipv67_context_t *ipv67_context_find(uint16_t port) {
    int i;

    if (port == 0) port = IPV67_PORT_DEFAULT;
    for (i = 0; i < ipv67_context_cap; i++) {
        if (ipv67_contexts[i] && ipv67_contexts[i]->active && ipv67_contexts[i]->port == port) return ipv67_contexts[i];
    }
    return NULL;
}

static int ipv67_ensure_context_cap(int needed) {
    ipv67_context_t **next;
    int next_cap;
    int i;

    if (needed > IPV67_CONTEXT_CAP_MAX) return IPV67_ERR_NOMEM;
    if (needed <= ipv67_context_cap) return IPV67_ERR_OK;
    next_cap = needed;
    if (next_cap > IPV67_CONTEXT_CAP_MAX) next_cap = IPV67_CONTEXT_CAP_MAX;
    next = (ipv67_context_t **)kmalloc(sizeof(ipv67_context_t *) * next_cap);
    if (!next) return IPV67_ERR_NOMEM;
    memset(next, 0, sizeof(ipv67_context_t *) * next_cap);
    for (i = 0; i < ipv67_context_cap; i++) {
        next[i] = ipv67_contexts[i];
    }
    if (ipv67_contexts) kfree(ipv67_contexts);
    ipv67_contexts = next;
    ipv67_context_cap = next_cap;
    return IPV67_ERR_OK;
}

ipv67_context_t *ipv67_context_get(uint16_t port, int create) {
    ipv67_context_t *ctx;
    int i;

    if (port == 0) port = IPV67_PORT_DEFAULT;
    ctx = ipv67_context_find(port);
    if (ctx) return ctx;
    if (!create) return NULL;
    if (ipv67_ensure_context_cap(ipv67_context_count + 1) < 0) return NULL;
    for (i = 0; i < ipv67_context_cap; i++) {
        if (!ipv67_contexts[i]) {
            ctx = (ipv67_context_t *)kmalloc(sizeof(ipv67_context_t));
            if (!ctx) return NULL;
            memset(ctx, 0, sizeof(ipv67_context_t));
            ipv67_contexts[i] = ctx;
            ipv67_context_count++;
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

int ipv67_use_port_existing(uint16_t port) {
    ipv67_context_t *ctx;

    ctx = ipv67_context_get(port, 0);
    if (!ctx) return IPV67_ERR_NOPEER;
    ipv67_current = ctx;
    return IPV67_ERR_OK;
}

int ipv67_port_active(uint16_t port) {
    int active;

    if (!ipv67_stack_trylock()) return port == IPV67_PORT_DEFAULT;
    active = ipv67_context_find(port) != NULL;
    ipv67_stack_unlock();
    return active;
}

int ipv67_ensure_peer_cap(int needed) {
    ipv67_peer_t *next;
    int next_cap;
    int i;

    if (needed > IPV67_PEER_CAP_MAX) needed = IPV67_PEER_CAP_MAX;
    if (needed <= ipv67_current->peer_cap) return IPV67_ERR_OK;
    next_cap = needed;
    if (next_cap > IPV67_PEER_CAP_MAX) next_cap = IPV67_PEER_CAP_MAX;
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

int ipv67_ensure_route_cap(int needed) {
    ipv67_route_t *next;
    int next_cap;
    int i;

    if (needed > IPV67_ROUTE_CAP_MAX) needed = IPV67_ROUTE_CAP_MAX;
    if (needed <= ipv67_current->route_cap) return IPV67_ERR_OK;
    next_cap = needed;
    if (next_cap > IPV67_ROUTE_CAP_MAX) next_cap = IPV67_ROUTE_CAP_MAX;
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

int ipv67_ensure_asn_cap(int needed) {
    ipv67_asn_claim_t *next;
    int next_cap;
    int i;

    if (needed > IPV67_ASN_CAP_MAX) needed = IPV67_ASN_CAP_MAX;
    if (needed <= ipv67_current->asn_cap) return IPV67_ERR_OK;
    next_cap = needed;
    if (next_cap > IPV67_ASN_CAP_MAX) next_cap = IPV67_ASN_CAP_MAX;
    next = (ipv67_asn_claim_t *)kmalloc(sizeof(ipv67_asn_claim_t) * next_cap);
    if (!next) return IPV67_ERR_NOMEM;
    memset(next, 0, sizeof(ipv67_asn_claim_t) * next_cap);
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        memcpy(&next[i], &ipv67_current->asns[i], sizeof(ipv67_asn_claim_t));
    }
    if (ipv67_current->asns) kfree(ipv67_current->asns);
    ipv67_current->asns = next;
    ipv67_current->asn_cap = next_cap;
    return IPV67_ERR_OK;
}

void ipv67_release_empty_tables(void) {
    if (!ipv67_current) return;
    if (ipv67_current->peers && ipv67_current->peer_count_val <= 0) {
        kfree(ipv67_current->peers);
        ipv67_current->peers = NULL;
        ipv67_current->peer_cap = 0;
        ipv67_current->peer_count_val = 0;
    }
    if (ipv67_current->routes && ipv67_current->route_count <= 0) {
        kfree(ipv67_current->routes);
        ipv67_current->routes = NULL;
        ipv67_current->route_cap = 0;
        ipv67_current->route_count = 0;
    }
    if (ipv67_current->asns && ipv67_current->asn_count <= 0) {
        kfree(ipv67_current->asns);
        ipv67_current->asns = NULL;
        ipv67_current->asn_cap = 0;
        ipv67_current->asn_count = 0;
    }
}

ipv67_ping_state_t *ipv67_ping_state_get(int create) {
    ipv67_ping_state_t *state;

    if (!ipv67_current) return NULL;
    if (ipv67_ping_state) return ipv67_ping_state;
    if (!create) return NULL;
    state = (ipv67_ping_state_t *)kmalloc(sizeof(ipv67_ping_state_t));
    if (!state) return NULL;
    memset(state, 0, sizeof(ipv67_ping_state_t));
    ipv67_ping_state = state;
    return state;
}

ipv67_stats_t *ipv67_stats_get(int create) {
    ipv67_stats_t *stats;

    if (!ipv67_current) return NULL;
    if (ipv67_current->stats) return ipv67_current->stats;
    if (!create) return NULL;
    stats = (ipv67_stats_t *)kmalloc(sizeof(ipv67_stats_t));
    if (!stats) return NULL;
    memset(stats, 0, sizeof(ipv67_stats_t));
    ipv67_current->stats = stats;
    return stats;
}

ipv67_auth_state_t *ipv67_auth_state_get(int create) {
    ipv67_auth_state_t *state;

    if (!ipv67_current) return NULL;
    if (ipv67_current->auth_state) return ipv67_current->auth_state;
    if (!create) return NULL;
    state = (ipv67_auth_state_t *)kmalloc(sizeof(ipv67_auth_state_t));
    if (!state) return NULL;
    memset(state, 0, sizeof(ipv67_auth_state_t));
    ipv67_current->auth_state = state;
    return state;
}

void ipv67_release_idle_io_state(void) {
    if (!ipv67_current) return;
    if (ipv67_current->ping_state && !ipv67_current->ping_state->active) {
        kfree(ipv67_current->ping_state);
        ipv67_current->ping_state = NULL;
    }
}

void ipv67_stats_add(uint64_t offset, uint64_t value) {
    ipv67_stats_t *stats;
    uint64_t *field;

    stats = ipv67_stats_get(1);
    if (!stats) return;
    if (offset + sizeof(uint64_t) > sizeof(ipv67_stats_t)) return;
    field = (uint64_t *)((uint8_t *)stats + offset);
    *field += value;
}

int ipv67_get_stats(ipv67_stats_t *out) {
    ipv67_stats_t *stats;

    if (!out || !ipv67_current) return IPV67_ERR_INVAL;
    memset(out, 0, sizeof(ipv67_stats_t));
    stats = ipv67_stats_get(0);
    if (stats) memcpy(out, stats, sizeof(ipv67_stats_t));
    return IPV67_ERR_OK;
}

void ipv67_clear_stats(void) {
    if (!ipv67_current) return;
    if (ipv67_current->stats) {
        kfree(ipv67_current->stats);
        ipv67_current->stats = NULL;
    }
}

#include "internal.h"

void ipv67_context_reset(ipv67_context_t *ctx, uint16_t port) {
    if (!ctx) return;
    if (ctx->io_depth > 0) return;
    if (ctx->peers) kfree(ctx->peers);
    if (ctx->routes) kfree(ctx->routes);
    if (ctx->replay_ids) kfree(ctx->replay_ids);
    memset(ctx, 0, sizeof(ipv67_context_t));
    ctx->port = port ? port : IPV67_PORT_DEFAULT;
    ctx->active = 1;
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

    if (needed <= ipv67_context_cap) return IPV67_ERR_OK;
    next_cap = ipv67_context_cap ? ipv67_context_cap * 2 : 1;
    while (next_cap < needed) next_cap *= 2;
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

    if (needed <= ipv67_current->peer_cap) return IPV67_ERR_OK;
    next_cap = ipv67_current->peer_cap ? ipv67_current->peer_cap * 2 : 1;
    while (next_cap < needed) next_cap *= 2;
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

    if (needed <= ipv67_current->route_cap) return IPV67_ERR_OK;
    next_cap = ipv67_current->route_cap ? ipv67_current->route_cap * 2 : 1;
    while (next_cap < needed) next_cap *= 2;
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

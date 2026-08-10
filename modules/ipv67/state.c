#include "internal.h"

ipv67_context_t **ipv67_contexts;
int ipv67_context_cap;
int ipv67_context_count;
ipv67_context_t *ipv67_current;
static spinlock_t ipv67_stack_lock_state = {0};
const uint8_t ipv67_bootstrap_key[IPV67_AUTH_KEY_SIZE] = {
    0x69, 0x70, 0x76, 0x36, 0x37, 0x2d, 0x62, 0x6f,
    0x6f, 0x74, 0x73, 0x74, 0x72, 0x61, 0x70, 0x31,
    0x2d, 0x6c, 0x65, 0x62, 0x69, 0x72, 0x75, 0x6e,
    0x2d, 0x6d, 0x65, 0x73, 0x68, 0x2d, 0x30, 0x31
};

static spinlock_t ipv67_rx_lock = {0};
static ipv67_pending_rx_t *ipv67_rx_head;
static ipv67_pending_rx_t *ipv67_rx_tail;
static int ipv67_rx_count;
static int ipv67_rx_draining;

void ipv67_stack_lock(void) {
    int spins;

    spins = 0;
    while (!spin_trylock(&ipv67_stack_lock_state)) {
        spins++;
        if ((spins & 0x3ff) == 0) {
            __asm__ volatile("sti");
            schedule();
        }
    }
}

int ipv67_stack_trylock(void) {
    return spin_trylock(&ipv67_stack_lock_state);
}

void ipv67_stack_unlock(void) {
    spin_unlock(&ipv67_stack_lock_state);
}

int ipv67_rx_enqueue(uint8_t family, uint16_t local_port, uint32_t src_ipv4, const ipv6_addr_t *src_ipv6, uint16_t src_port, const uint8_t *packet, uint64_t len) {
    ipv67_pending_rx_t *rx;

    if (!packet || len == 0 || len > IPV67_RX_PACKET_MAX) return 0;
    if (len > SIZE_MAX - sizeof(ipv67_pending_rx_t)) return 0;
    rx = (ipv67_pending_rx_t *)kmalloc(sizeof(ipv67_pending_rx_t) + len);
    if (!rx) return 0;
    memset(rx, 0, sizeof(ipv67_pending_rx_t));
    memcpy(rx->packet, packet, len);
    if (!spin_trylock(&ipv67_rx_lock)) {
        kfree(rx);
        return 0;
    }
    if (ipv67_rx_count == INT32_MAX) {
        spin_unlock(&ipv67_rx_lock);
        kfree(rx);
        return 0;
    }
    rx->family = family;
    rx->local_port = local_port;
    rx->src_ipv4 = src_ipv4;
    if (src_ipv6) memcpy(&rx->src_ipv6, src_ipv6, sizeof(ipv6_addr_t));
    rx->src_port = src_port;
    rx->len = len;
    if (ipv67_rx_tail) ipv67_rx_tail->next = rx;
    else ipv67_rx_head = rx;
    ipv67_rx_tail = rx;
    ipv67_rx_count++;
    spin_unlock(&ipv67_rx_lock);
    return 1;
}

static ipv67_pending_rx_t *ipv67_rx_dequeue(void) {
    ipv67_pending_rx_t *rx;

    if (!spin_trylock(&ipv67_rx_lock)) return NULL;
    rx = ipv67_rx_head;
    if (rx) {
        ipv67_rx_head = rx->next;
        if (!ipv67_rx_head) ipv67_rx_tail = NULL;
        if (ipv67_rx_count > 0) ipv67_rx_count--;
        rx->next = NULL;
    }
    spin_unlock(&ipv67_rx_lock);
    return rx;
}

static void ipv67_rx_release(ipv67_pending_rx_t *rx) {
    if (!rx) return;
    kfree(rx);
}

void ipv67_rx_flush_port(uint16_t port) {
    ipv67_pending_rx_t *rx;
    ipv67_pending_rx_t *next;
    ipv67_pending_rx_t *previous;
    ipv67_pending_rx_t *release_head;

    if (port == 0) port = IPV67_PORT_DEFAULT;
    release_head = NULL;
    spin_lock(&ipv67_rx_lock);
    previous = NULL;
    rx = ipv67_rx_head;
    while (rx) {
        next = rx->next;
        if (rx->local_port == port) {
            if (previous) previous->next = next;
            else ipv67_rx_head = next;
            if (ipv67_rx_tail == rx) ipv67_rx_tail = previous;
            rx->next = release_head;
            release_head = rx;
            if (ipv67_rx_count > 0) ipv67_rx_count--;
        } else {
            previous = rx;
        }
        rx = next;
    }
    spin_unlock(&ipv67_rx_lock);
    while (release_head) {
        next = release_head->next;
        kfree(release_head);
        release_head = next;
    }
}

void ipv67_rx_release_empty_storage(void) {
}

void ipv67_drain_pending_locked(void) {
    ipv67_pending_rx_t *rx;
    int limit;

    if (ipv67_rx_draining) return;
    ipv67_rx_draining = 1;
    spin_lock(&ipv67_rx_lock);
    limit = ipv67_rx_count;
    spin_unlock(&ipv67_rx_lock);
    while (limit-- > 0) {
        rx = ipv67_rx_dequeue();
        if (!rx) {
            ipv67_rx_draining = 0;
            ipv67_rx_release_empty_storage();
            return;
        }
        if (rx->family == IPV67_PEER_IPV6) ipv67_receive6_on_port_locked(rx->local_port, &rx->src_ipv6, rx->src_port, rx->packet, rx->len);
        else ipv67_receive_on_port_locked(rx->local_port, rx->src_ipv4, rx->src_port, rx->packet, rx->len);
        ipv67_rx_release(rx);
    }
    ipv67_rx_draining = 0;
    ipv67_rx_release_empty_storage();
}

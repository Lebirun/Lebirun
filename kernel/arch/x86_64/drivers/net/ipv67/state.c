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
static ipv67_pending_rx_t *ipv67_rx_slots;
static int ipv67_rx_head = -1;
static int ipv67_rx_tail = -1;
static int ipv67_rx_count;
static int ipv67_rx_draining;

static int ipv67_rx_ensure_slots(void) {
    ipv67_pending_rx_t *slots;

    if (ipv67_rx_slots) return 1;
    slots = (ipv67_pending_rx_t *)kmalloc(IPV67_RX_PENDING_MAX * sizeof(ipv67_pending_rx_t));
    if (!slots) return 0;
    memset(slots, 0, IPV67_RX_PENDING_MAX * sizeof(ipv67_pending_rx_t));
    ipv67_rx_slots = slots;
    return 1;
}

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
    uint8_t *copy;
    int slot;
    int i;

    if (!packet || len == 0 || len > IPV67_RX_PACKET_MAX) return 0;
    copy = (uint8_t *)kmalloc(len);
    if (!copy) return 0;
    memcpy(copy, packet, len);
    if (!spin_trylock(&ipv67_rx_lock)) {
        kfree(copy);
        return 0;
    }
    if (!ipv67_rx_ensure_slots()) {
        spin_unlock(&ipv67_rx_lock);
        kfree(copy);
        return 0;
    }
    if (ipv67_rx_count >= IPV67_RX_PENDING_MAX) {
        spin_unlock(&ipv67_rx_lock);
        kfree(copy);
        return 0;
    }
    slot = -1;
    for (i = 0; i < IPV67_RX_PENDING_MAX; i++) {
        if (ipv67_rx_slots[i].state == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&ipv67_rx_lock);
        kfree(copy);
        return 0;
    }
    memset(&ipv67_rx_slots[slot], 0, sizeof(ipv67_rx_slots[slot]));
    ipv67_rx_slots[slot].packet = copy;
    ipv67_rx_slots[slot].next = -1;
    ipv67_rx_slots[slot].state = 1;
    ipv67_rx_slots[slot].family = family;
    ipv67_rx_slots[slot].local_port = local_port;
    ipv67_rx_slots[slot].src_ipv4 = src_ipv4;
    if (src_ipv6) memcpy(&ipv67_rx_slots[slot].src_ipv6, src_ipv6, sizeof(ipv6_addr_t));
    ipv67_rx_slots[slot].src_port = src_port;
    ipv67_rx_slots[slot].len = len;
    if (ipv67_rx_tail >= 0) ipv67_rx_slots[ipv67_rx_tail].next = slot;
    else ipv67_rx_head = slot;
    ipv67_rx_tail = slot;
    ipv67_rx_count++;
    spin_unlock(&ipv67_rx_lock);
    return 1;
}

static ipv67_pending_rx_t *ipv67_rx_dequeue(void) {
    int slot;

    if (!spin_trylock(&ipv67_rx_lock)) return NULL;
    slot = ipv67_rx_head;
    if (slot >= 0) {
        ipv67_rx_head = ipv67_rx_slots[slot].next;
        if (ipv67_rx_head < 0) ipv67_rx_tail = -1;
        if (ipv67_rx_count > 0) ipv67_rx_count--;
        ipv67_rx_slots[slot].next = -1;
        ipv67_rx_slots[slot].state = 2;
    }
    spin_unlock(&ipv67_rx_lock);
    if (slot < 0) return NULL;
    return &ipv67_rx_slots[slot];
}

static void ipv67_rx_release(ipv67_pending_rx_t *rx) {
    uint8_t *packet;

    if (!rx) return;
    spin_lock(&ipv67_rx_lock);
    packet = rx->packet;
    memset(rx, 0, sizeof(*rx));
    spin_unlock(&ipv67_rx_lock);
    if (packet) kfree(packet);
}

void ipv67_rx_flush_port(uint16_t port) {
    uint8_t *packets[IPV67_RX_PENDING_MAX];
    int i;
    int prev;

    memset(packets, 0, sizeof(packets));
    if (port == 0) port = IPV67_PORT_DEFAULT;
    spin_lock(&ipv67_rx_lock);
    if (!ipv67_rx_slots) {
        spin_unlock(&ipv67_rx_lock);
        return;
    }
    for (i = 0; i < IPV67_RX_PENDING_MAX; i++) {
        if (ipv67_rx_slots[i].state == 1 && ipv67_rx_slots[i].local_port == port) {
            packets[i] = ipv67_rx_slots[i].packet;
            memset(&ipv67_rx_slots[i], 0, sizeof(ipv67_rx_slots[i]));
        }
    }
    ipv67_rx_head = -1;
    ipv67_rx_tail = -1;
    ipv67_rx_count = 0;
    prev = -1;
    for (i = 0; i < IPV67_RX_PENDING_MAX; i++) {
        if (ipv67_rx_slots[i].state == 1) {
            ipv67_rx_slots[i].next = -1;
            if (prev >= 0) ipv67_rx_slots[prev].next = i;
            else ipv67_rx_head = i;
            prev = i;
            ipv67_rx_tail = i;
            ipv67_rx_count++;
        }
    }
    spin_unlock(&ipv67_rx_lock);
    for (i = 0; i < IPV67_RX_PENDING_MAX; i++) {
        if (packets[i]) kfree(packets[i]);
    }
}

void ipv67_drain_pending_locked(void) {
    ipv67_pending_rx_t *rx;
    int limit;

    if (ipv67_rx_draining) return;
    ipv67_rx_draining = 1;
    limit = IPV67_RX_PENDING_MAX;
    while (limit-- > 0) {
        rx = ipv67_rx_dequeue();
        if (!rx) {
            ipv67_rx_draining = 0;
            return;
        }
        if (rx->family == IPV67_PEER_IPV6) ipv67_receive6_on_port_locked(rx->local_port, &rx->src_ipv6, rx->src_port, rx->packet, rx->len);
        else ipv67_receive_on_port_locked(rx->local_port, rx->src_ipv4, rx->src_port, rx->packet, rx->len);
        ipv67_rx_release(rx);
    }
    ipv67_rx_draining = 0;
}

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
    ipv67_receive_common_on_port_locked(IPV67_PEER_IPV6, local_port, 0, src_ipv6, src_port, packet, len);
}

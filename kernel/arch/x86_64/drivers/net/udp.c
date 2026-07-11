#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/ipv6.h>
#include <lebirun/drivers/net/dhcp.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <string.h>

static udp_socket_t *udp_sockets = NULL;
static uint16_t udp_ephemeral_port = 49152;
static const udp_port_hook_t *udp_port_hook;

void udp_init(void) {
    udp_sockets = NULL;
    udp_ephemeral_port = 49152;
    udp_port_hook = NULL;
}

int udp_register_port_hook(const udp_port_hook_t *hook) {
    if (!hook) return -1;
    if (udp_port_hook && udp_port_hook != hook) return -2;
    udp_port_hook = hook;
    return 0;
}

void udp_unregister_port_hook(const udp_port_hook_t *hook) {
    if (udp_port_hook == hook) udp_port_hook = NULL;
}

static uint16_t udp_pseudo_checksum(ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint64_t len) {
    uint64_t sum;
    uint16_t *ptr;
    uint64_t remaining;

    sum = 0;
    ptr = (uint16_t *)data;
    remaining = len;

    sum += (src.octets[0] << 8) | src.octets[1];
    sum += (src.octets[2] << 8) | src.octets[3];
    sum += (dest.octets[0] << 8) | dest.octets[1];
    sum += (dest.octets[2] << 8) | dest.octets[3];
    sum += IP_PROTO_UDP;
    sum += len;
    while (remaining > 1) {
        sum += ntohs(*ptr++);
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += (*((uint8_t *)ptr)) << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons(~sum);
}

int udp_send(netif_t *netif, ipv4_addr_t dest, uint16_t src_port, uint16_t dest_port, uint8_t *data, uint64_t len) {
    return udp_send_from(netif, netif->ipv4, dest, src_port, dest_port, data, len);
}

int udp_send_from(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint16_t src_port, uint16_t dest_port, uint8_t *data, uint64_t len) {
    uint64_t udp_len;
    uint8_t *packet;
    udp_header_t *udp;
    int result;

    if (!netif || (!data && len != 0)) return -1;
    if (len > UINT16_MAX - sizeof(udp_header_t)) return -1;

    udp_len = sizeof(udp_header_t) + len;
    packet = (uint8_t *)kmalloc(udp_len);
    if (!packet) return -1;

    udp = (udp_header_t *)packet;
    udp->src_port = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->length = htons(udp_len);
    udp->checksum = 0;

    memcpy(packet + sizeof(udp_header_t), data, len);

    udp->checksum = udp_pseudo_checksum(src, dest, packet, udp_len);
    if (udp->checksum == 0) udp->checksum = 0xFFFF;

    result = ipv4_send(netif, dest, IP_PROTO_UDP, packet, udp_len);
    kfree(packet);

    return result;
}

static uint16_t udp6_pseudo_checksum(ipv6_addr_t src, ipv6_addr_t dest, uint8_t *data, uint64_t len) {
    uint64_t sum;
    uint16_t *ptr;
    uint64_t remaining;
    int i;

    sum = 0;
    ptr = (uint16_t *)src.octets;
    for (i = 0; i < 8; i++) {
        sum += ntohs(ptr[i]);
    }

    ptr = (uint16_t *)dest.octets;
    for (i = 0; i < 8; i++) {
        sum += ntohs(ptr[i]);
    }

    sum += (len >> 16) & 0xFFFF;
    sum += len & 0xFFFF;
    sum += IP_PROTO_UDP;

    ptr = (uint16_t *)data;
    remaining = len;
    while (remaining > 1) {
        sum += ntohs(*ptr++);
        remaining -= 2;
    }

    if (remaining == 1) {
        sum += (*((uint8_t *)ptr)) << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons(~sum);
}

int udp_send6(netif_t *netif, ipv6_addr_t dest, uint16_t src_port, uint16_t dest_port, uint8_t *data, uint64_t len) {
    uint64_t udp_len;
    uint8_t *packet;
    udp_header_t *udp;
    int result;

    if (!netif || (!data && len != 0)) return -1;
    if (len > UINT16_MAX - sizeof(udp_header_t)) return -1;

    udp_len = sizeof(udp_header_t) + len;
    packet = (uint8_t *)kmalloc(udp_len);
    if (!packet) return -1;

    udp = (udp_header_t *)packet;
    udp->src_port = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->length = htons(udp_len);
    udp->checksum = 0;

    memcpy(packet + sizeof(udp_header_t), data, len);

    udp->checksum = udp6_pseudo_checksum(netif->ipv6, dest, packet, udp_len);
    if (udp->checksum == 0) udp->checksum = 0xFFFF;

    result = ipv6_send(netif, dest, IP_PROTO_UDP, packet, udp_len);
    kfree(packet);

    return result;
}

void udp_receive(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint64_t len) {
    udp_header_t *udp;
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t udp_len;
    uint8_t *payload;
    uint64_t payload_len;
    udp_socket_t *sock;
    uint32_t src_ipv4;

    (void)dest;
    if (!netif || !data || len < sizeof(udp_header_t)) return;

    udp = (udp_header_t *)data;
    src_port = ntohs(udp->src_port);
    dest_port = ntohs(udp->dest_port);
    udp_len = ntohs(udp->length);

    if (udp_len < sizeof(udp_header_t) || udp_len > len) return;
    if (udp->checksum != 0 && udp_pseudo_checksum(src, dest, data, udp_len) != 0) return;

    payload = data + sizeof(udp_header_t);
    payload_len = udp_len - sizeof(udp_header_t);

    if (dest_port == DHCP_CLIENT_PORT) {
        dhcp_receive(netif, payload, payload_len);
        return;
    }

    if (src_port == DNS_PORT) {
        dns_receive(netif, src, src_port, payload, payload_len);
    }

    if (udp_port_hook && udp_port_hook->port_active && udp_port_hook->receive4 && udp_port_hook->port_active(dest_port)) {
        src_ipv4 = ((uint32_t)src.octets[0] << 24) |
                   ((uint32_t)src.octets[1] << 16) |
                   ((uint32_t)src.octets[2] << 8) |
                   (uint32_t)src.octets[3];
        udp_port_hook->receive4(dest_port, src_ipv4, src_port, payload, payload_len);
        return;
    }

    sock = udp_sockets;
    while (sock) {
        if (sock->local_port == dest_port) {
            if (payload_len <= sock->recv_buffer_size) {
                memcpy(sock->recv_buffer, payload, payload_len);
                sock->recv_len = payload_len;
                sock->recv_from_ip = src;
                sock->recv_from_port = src_port;
                sock->has_data = 1;
            }
            return;
        }
        sock = sock->next;
    }
}

void udp_receive6(netif_t *netif, ipv6_addr_t src, ipv6_addr_t dest, uint8_t *data, uint64_t len) {
    udp_header_t *udp;
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t udp_len;
    uint8_t *payload;
    uint64_t payload_len;

    (void)dest;
    if (!netif || !data || len < sizeof(udp_header_t)) return;

    udp = (udp_header_t *)data;
    src_port = ntohs(udp->src_port);
    dest_port = ntohs(udp->dest_port);
    udp_len = ntohs(udp->length);

    if (udp_len < sizeof(udp_header_t) || udp_len > len) return;
    if (udp->checksum == 0 || udp6_pseudo_checksum(src, dest, data, udp_len) != 0) return;

    payload = data + sizeof(udp_header_t);
    payload_len = udp_len - sizeof(udp_header_t);

    if (udp_port_hook && udp_port_hook->port_active && udp_port_hook->receive6 && udp_port_hook->port_active(dest_port)) {
        udp_port_hook->receive6(dest_port, &src, src_port, payload, payload_len);
        return;
    }

    (void)src;
    (void)src_port;
    (void)dest_port;
    (void)payload;
    (void)payload_len;
}

udp_socket_t *udp_socket_create(uint16_t port) {
    udp_socket_t *sock;

    sock = (udp_socket_t *)kmalloc(sizeof(udp_socket_t));
    if (!sock) return NULL;

    memset(sock, 0, sizeof(udp_socket_t));

    sock->local_port = port ? port : udp_ephemeral_port++;
    sock->recv_buffer_size = 4096;
    sock->recv_buffer = (uint8_t *)kmalloc(sock->recv_buffer_size);
    if (!sock->recv_buffer) {
        kfree(sock);
        return NULL;
    }

    sock->netif = netif_get_default();
    sock->next = udp_sockets;
    udp_sockets = sock;

    return sock;
}

void udp_socket_close(udp_socket_t *sock) {
    udp_socket_t **prev;

    if (!sock) return;

    prev = &udp_sockets;
    while (*prev) {
        if (*prev == sock) {
            *prev = sock->next;
            break;
        }
        prev = &(*prev)->next;
    }

    if (sock->recv_buffer) {
        kfree(sock->recv_buffer);
    }
    kfree(sock);
}

int udp_socket_send(udp_socket_t *sock, ipv4_addr_t dest, uint16_t port, uint8_t *data, uint64_t len) {
    if (!sock || !sock->netif) return -1;
    return udp_send(sock->netif, dest, sock->local_port, port, data, len);
}

int udp_socket_recv(udp_socket_t *sock, uint8_t *buffer, uint64_t len, ipv4_addr_t *from_ip, uint16_t *from_port, uint64_t timeout_ms) {
    uint64_t timeout_ticks;
    uint64_t start;
    uint64_t copy_len;

    if (!sock) return -1;

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();

    while (!sock->has_data) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (pit_get_ticks() - start > timeout_ticks) {
            return -1;
        }
        schedule();
    }

    copy_len = sock->recv_len < len ? sock->recv_len : len;
    memcpy(buffer, sock->recv_buffer, copy_len);

    if (from_ip) *from_ip = sock->recv_from_ip;
    if (from_port) *from_port = sock->recv_from_port;

    sock->has_data = 0;
    sock->recv_len = 0;

    return copy_len;
}

#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/ethernet.h>
#include <lebirun/drivers/net/arp.h>
#include <lebirun/drivers/net/icmp.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/drivers/net/dhcp.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

static uint16_t ip_id_counter = 1;

void ipv4_init(void) {
    ip_id_counter = 1;
}

uint16_t ipv4_checksum(void *data, uint64_t len) {
    uint64_t sum;
    uint16_t *ptr;

    sum = 0;
    ptr = (uint16_t *)data;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *((uint8_t *)ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

int ipv4_is_local(netif_t *netif, ipv4_addr_t ip) {
    uint32_t local;
    uint32_t mask;
    uint32_t target;

    if (!netif) return 0;

    local = ipv4_to_u32(netif->ipv4);
    mask = ipv4_to_u32(netif->netmask);
    target = ipv4_to_u32(ip);

    return (local & mask) == (target & mask);
}

int ipv4_send(netif_t *netif, ipv4_addr_t dest, uint8_t protocol, uint8_t *data, uint64_t len) {
    uint64_t total_len;
    uint8_t *packet;
    ipv4_header_t *ip;
    mac_addr_t dest_mac;
    ipv4_addr_t next_hop;
    int result;

    if (!netif) return -1;
    if (len > ETH_MTU - sizeof(ipv4_header_t)) return -1;

    total_len = sizeof(ipv4_header_t) + len;
    packet = (uint8_t *)kmalloc(total_len);
    if (!packet) return -1;

    ip = (ipv4_header_t *)packet;
    memset(ip, 0, sizeof(ipv4_header_t));

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = htons(total_len);
    ip->identification = htons(ip_id_counter++);
    ip->flags_fragment = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src = netif->ipv4;
    ip->dest = dest;

    ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

    memcpy(packet + sizeof(ipv4_header_t), data, len);

    next_hop = dest;

    if (ipv4_eq(dest, IPV4_BROADCAST)) {
        dest_mac = MAC_BROADCAST;
    } else {
        if (!ipv4_is_local(netif, dest)) {
            next_hop = netif->gateway;
        }

        if (arp_resolve(netif, next_hop, &dest_mac) < 0) {
            if (ipv4_eq(next_hop, dest) && !ipv4_eq(netif->gateway, IPV4_ZERO)) {
                next_hop = netif->gateway;
                if (arp_resolve(netif, next_hop, &dest_mac) < 0) {
                    kfree(packet);
                    return -1;
                }
            } else {
                kfree(packet);
                return -1;
            }
        }
    }

    result = eth_send(netif, dest_mac, ETH_TYPE_IPV4, packet, total_len);
    kfree(packet);

    return result;
}

void ipv4_receive(netif_t *netif, uint8_t *packet, uint64_t len) {
    ipv4_header_t *ip;
    uint64_t header_len;
    uint16_t total_len;
    uint16_t checksum;
    uint8_t *payload;
    uint64_t payload_len;

    if (!netif || !packet) return;
    if (len < sizeof(ipv4_header_t)) return;

    ip = (ipv4_header_t *)packet;

    if ((ip->version_ihl >> 4) != 4) return;

    header_len = (ip->version_ihl & 0x0F) * 4;
    if (header_len < 20 || header_len > len) return;

    total_len = ntohs(ip->total_length);
    if (total_len > len) return;

    checksum = ip->checksum;
    ip->checksum = 0;
    if (ipv4_checksum(ip, header_len) != checksum) {
        return;
    }
    ip->checksum = checksum;

    if (!ipv4_eq(ip->dest, netif->ipv4) &&
        !ipv4_eq(ip->dest, IPV4_BROADCAST) &&
        !(ipv4_eq(netif->ipv4, IPV4_ZERO) && !netif->dhcp_configured) &&
        !(!netif->dhcp_configured && dhcp_is_negotiating())) {
        return;
    }

    payload = packet + header_len;
    payload_len = total_len - header_len;

    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            icmp_receive(netif, ip->src, payload, payload_len);
            break;

        case IP_PROTO_UDP:
            udp_receive(netif, ip->src, ip->dest, payload, payload_len);
            break;

        case IP_PROTO_TCP:
            tcp_receive(netif, ip->src, ip->dest, payload, payload_len);
            break;

        default:
            break;
    }
}

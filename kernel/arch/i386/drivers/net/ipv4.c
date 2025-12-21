#include <kernel/drivers/net/ipv4.h>
#include <kernel/drivers/net/ethernet.h>
#include <kernel/drivers/net/arp.h>
#include <kernel/drivers/net/icmp.h>
#include <kernel/drivers/net/udp.h>
#include <kernel/drivers/net/tcp.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

static uint16_t ip_id_counter = 1;

void ipv4_init(void) {
    ip_id_counter = 1;
}

uint16_t ipv4_checksum(void *data, uint32_t len) {
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *)data;

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
    if (!netif) return 0;

    uint32_t local = ipv4_to_u32(netif->ipv4);
    uint32_t mask = ipv4_to_u32(netif->netmask);
    uint32_t target = ipv4_to_u32(ip);

    return (local & mask) == (target & mask);
}

int ipv4_send(netif_t *netif, ipv4_addr_t dest, uint8_t protocol, uint8_t *data, uint32_t len) {
    if (!netif) return -1;
    if (len > ETH_MTU - sizeof(ipv4_header_t)) return -1;

    uint32_t total_len = sizeof(ipv4_header_t) + len;
    uint8_t *packet = (uint8_t *)kmalloc(total_len);
    if (!packet) return -1;

    ipv4_header_t *ip = (ipv4_header_t *)packet;
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

    mac_addr_t dest_mac;
    ipv4_addr_t next_hop = dest;

    if (ipv4_eq(dest, IPV4_BROADCAST)) {
        dest_mac = MAC_BROADCAST;
    } else {
        if (!ipv4_is_local(netif, dest)) {
            next_hop = netif->gateway;
        }

        if (arp_resolve(netif, next_hop, &dest_mac) < 0) {
            kfree(packet);
            return -1;
        }
    }

    int result = eth_send(netif, dest_mac, ETH_TYPE_IPV4, packet, total_len);
    kfree(packet);

    return result;
}

void ipv4_receive(netif_t *netif, uint8_t *packet, uint32_t len) {
    if (!netif || !packet) return;
    if (len < sizeof(ipv4_header_t)) return;

    ipv4_header_t *ip = (ipv4_header_t *)packet;

    if ((ip->version_ihl >> 4) != 4) return;

    uint32_t header_len = (ip->version_ihl & 0x0F) * 4;
    if (header_len < 20 || header_len > len) return;

    uint16_t total_len = ntohs(ip->total_length);
    if (total_len > len) return;

    uint16_t checksum = ip->checksum;
    ip->checksum = 0;
    if (ipv4_checksum(ip, header_len) != checksum) {
        return;
    }
    ip->checksum = checksum;

    if (!ipv4_eq(ip->dest, netif->ipv4) &&
        !ipv4_eq(ip->dest, IPV4_BROADCAST)) {
        return;
    }

    uint8_t *payload = packet + header_len;
    uint32_t payload_len = total_len - header_len;

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

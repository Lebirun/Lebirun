#include <kernel/drivers/net/ipv6.h>
#include <kernel/drivers/net/ethernet.h>
#include <kernel/drivers/net/icmpv6.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

void ipv6_init(void) {
}

void ipv6_create_link_local(mac_addr_t mac, ipv6_addr_t *out) {
    memset(out, 0, sizeof(ipv6_addr_t));
    out->octets[0] = 0xFE;
    out->octets[1] = 0x80;
    out->octets[8] = mac.addr[0] ^ 0x02;
    out->octets[9] = mac.addr[1];
    out->octets[10] = mac.addr[2];
    out->octets[11] = 0xFF;
    out->octets[12] = 0xFE;
    out->octets[13] = mac.addr[3];
    out->octets[14] = mac.addr[4];
    out->octets[15] = mac.addr[5];
}

uint16_t ipv6_checksum(ipv6_addr_t *src, ipv6_addr_t *dest, uint8_t next_header, uint8_t *data, uint64_t len) {
    uint64_t sum = 0;
    uint16_t *ptr;

    ptr = (uint16_t *)src->octets;
    for (int i = 0; i < 8; i++) {
        sum += ntohs(ptr[i]);
    }

    ptr = (uint16_t *)dest->octets;
    for (int i = 0; i < 8; i++) {
        sum += ntohs(ptr[i]);
    }

    sum += len;
    sum += next_header;

    ptr = (uint16_t *)data;
    uint64_t data_len = len;
    while (data_len > 1) {
        sum += ntohs(*ptr++);
        data_len -= 2;
    }

    if (data_len == 1) {
        sum += (*((uint8_t *)ptr)) << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons(~sum);
}

static void ipv6_mac_from_multicast(ipv6_addr_t *ip, mac_addr_t *mac) {
    mac->addr[0] = 0x33;
    mac->addr[1] = 0x33;
    mac->addr[2] = ip->octets[12];
    mac->addr[3] = ip->octets[13];
    mac->addr[4] = ip->octets[14];
    mac->addr[5] = ip->octets[15];
}

int ipv6_send(netif_t *netif, ipv6_addr_t dest, uint8_t next_header, uint8_t *data, uint64_t len) {
    if (!netif) return -1;
    if (len > ETH_MTU - sizeof(ipv6_header_t)) return -1;

    uint64_t total_len = sizeof(ipv6_header_t) + len;
    uint8_t *packet = (uint8_t *)kmalloc(total_len);
    if (!packet) return -1;

    ipv6_header_t *ip6 = (ipv6_header_t *)packet;
    memset(ip6, 0, sizeof(ipv6_header_t));

    ip6->version_tc_flow = htonl(0x60000000);
    ip6->payload_length = htons(len);
    ip6->next_header = next_header;
    ip6->hop_limit = 64;
    ip6->src = netif->ipv6;
    ip6->dest = dest;

    memcpy(packet + sizeof(ipv6_header_t), data, len);

    mac_addr_t dest_mac;

    if (dest.octets[0] == 0xFF) {
        ipv6_mac_from_multicast(&dest, &dest_mac);
    } else {
        dest_mac = MAC_BROADCAST;
    }

    int result = eth_send(netif, dest_mac, ETH_TYPE_IPV6, packet, total_len);
    kfree(packet);

    return result;
}

void ipv6_receive(netif_t *netif, uint8_t *packet, uint64_t len) {
    if (!netif || !packet) return;
    if (len < sizeof(ipv6_header_t)) return;

    ipv6_header_t *ip6 = (ipv6_header_t *)packet;

    uint64_t version_tc_flow = ntohl(ip6->version_tc_flow);
    if ((version_tc_flow >> 28) != 6) return;

    uint16_t payload_len = ntohs(ip6->payload_length);
    if (sizeof(ipv6_header_t) + payload_len > len) return;

    uint8_t *payload = packet + sizeof(ipv6_header_t);

    switch (ip6->next_header) {
        case IP_PROTO_ICMPV6:
            icmpv6_receive(netif, &ip6->src, payload, payload_len);
            break;

        default:
            break;
    }
}

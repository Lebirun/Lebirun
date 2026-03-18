#include <kernel/drivers/net/icmpv6.h>
#include <kernel/drivers/net/ipv6.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

void icmpv6_receive(netif_t *netif, ipv6_addr_t *src, uint8_t *data, uint64_t len) {
    if (!netif || !src || !data || len < sizeof(icmpv6_header_t)) return;

    icmpv6_header_t *icmp = (icmpv6_header_t *)data;

    switch (icmp->type) {
        case ICMPV6_ECHO_REQUEST: {
            uint64_t reply_len = len;
            uint8_t *reply = (uint8_t *)kmalloc(reply_len);
            if (!reply) return;

            memcpy(reply, data, len);
            icmpv6_header_t *reply_icmp = (icmpv6_header_t *)reply;
            reply_icmp->type = ICMPV6_ECHO_REPLY;
            reply_icmp->checksum = 0;
            reply_icmp->checksum = ipv6_checksum(&netif->ipv6, src, IP_PROTO_ICMPV6, reply, reply_len);

            ipv6_send(netif, *src, IP_PROTO_ICMPV6, reply, reply_len);
            kfree(reply);
            break;
        }

        case ICMPV6_NEIGHBOR_SOLICITATION: {
            if (len < 24) return;
            ipv6_addr_t *target = (ipv6_addr_t *)(data + 8);
            if (ipv6_eq(*target, netif->ipv6)) {
                uint8_t reply[32];
                memset(reply, 0, sizeof(reply));

                icmpv6_header_t *na = (icmpv6_header_t *)reply;
                na->type = ICMPV6_NEIGHBOR_ADVERTISEMENT;
                na->code = 0;
                na->data = htonl(0x60000000);

                memcpy(reply + 8, &netif->ipv6, 16);

                reply[24] = 2;
                reply[25] = 1;
                memcpy(reply + 26, &netif->mac, 6);

                na->checksum = 0;
                na->checksum = ipv6_checksum(&netif->ipv6, src, IP_PROTO_ICMPV6, reply, 32);

                ipv6_send(netif, *src, IP_PROTO_ICMPV6, reply, 32);
            }
            break;
        }

        case ICMPV6_ROUTER_ADVERTISEMENT:
            break;

        default:
            break;
    }
}

int icmpv6_send_echo_request(netif_t *netif, ipv6_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint64_t len) {
    if (!netif) return -1;

    uint64_t icmp_len = 8 + len;
    uint8_t *packet = (uint8_t *)kmalloc(icmp_len);
    if (!packet) return -1;

    memset(packet, 0, icmp_len);
    icmpv6_header_t *icmp = (icmpv6_header_t *)packet;
    icmp->type = ICMPV6_ECHO_REQUEST;
    icmp->code = 0;
    icmp->data = htonl((id << 16) | seq);

    if (data && len > 0) {
        memcpy(packet + 8, data, len);
    }

    icmp->checksum = 0;
    icmp->checksum = ipv6_checksum(&netif->ipv6, &dest, IP_PROTO_ICMPV6, packet, icmp_len);

    int result = ipv6_send(netif, dest, IP_PROTO_ICMPV6, packet, icmp_len);
    kfree(packet);

    return result;
}

int icmpv6_send_neighbor_solicitation(netif_t *netif, ipv6_addr_t target) {
    if (!netif) return -1;

    uint8_t packet[32];
    memset(packet, 0, sizeof(packet));

    icmpv6_header_t *ns = (icmpv6_header_t *)packet;
    ns->type = ICMPV6_NEIGHBOR_SOLICITATION;
    ns->code = 0;
    ns->data = 0;

    memcpy(packet + 8, &target, 16);

    packet[24] = 1;
    packet[25] = 1;
    memcpy(packet + 26, &netif->mac, 6);

    ipv6_addr_t dest;
    memset(&dest, 0, sizeof(dest));
    dest.octets[0] = 0xFF;
    dest.octets[1] = 0x02;
    dest.octets[11] = 0x01;
    dest.octets[12] = 0xFF;
    dest.octets[13] = target.octets[13];
    dest.octets[14] = target.octets[14];
    dest.octets[15] = target.octets[15];

    ns->checksum = 0;
    ns->checksum = ipv6_checksum(&netif->ipv6, &dest, IP_PROTO_ICMPV6, packet, 32);

    return ipv6_send(netif, dest, IP_PROTO_ICMPV6, packet, 32);
}

int icmpv6_send_router_solicitation(netif_t *netif) {
    if (!netif) return -1;

    uint8_t packet[16];
    memset(packet, 0, sizeof(packet));

    icmpv6_header_t *rs = (icmpv6_header_t *)packet;
    rs->type = ICMPV6_ROUTER_SOLICITATION;
    rs->code = 0;
    rs->data = 0;

    packet[8] = 1;
    packet[9] = 1;
    memcpy(packet + 10, &netif->mac, 6);

    ipv6_addr_t all_routers;
    memset(&all_routers, 0, sizeof(all_routers));
    all_routers.octets[0] = 0xFF;
    all_routers.octets[1] = 0x02;
    all_routers.octets[15] = 0x02;

    rs->checksum = 0;
    rs->checksum = ipv6_checksum(&netif->ipv6, &all_routers, IP_PROTO_ICMPV6, packet, 16);

    return ipv6_send(netif, all_routers, IP_PROTO_ICMPV6, packet, 16);
}

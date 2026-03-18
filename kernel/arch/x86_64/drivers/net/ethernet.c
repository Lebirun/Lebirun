#include <kernel/drivers/net/ethernet.h>
#include <kernel/drivers/net/arp.h>
#include <kernel/drivers/net/ipv4.h>
#include <kernel/drivers/net/ipv6.h>
#include <kernel/mem_map.h>
#include <string.h>

int eth_send(netif_t *netif, mac_addr_t dest, uint16_t ethertype, uint8_t *data, uint64_t len) {
    if (!netif || !netif->send) return -1;
    if (len > ETH_MTU) return -1;

    uint64_t frame_len = ETH_HEADER_LEN + len;
    uint8_t *frame = (uint8_t *)kmalloc(frame_len);
    if (!frame) return -1;

    eth_header_t *eth = (eth_header_t *)frame;
    memcpy(&eth->dest, &dest, ETH_ALEN);
    memcpy(&eth->src, &netif->mac, ETH_ALEN);
    eth->ethertype = htons(ethertype);

    memcpy(frame + ETH_HEADER_LEN, data, len);

    int result = netif->send(netif, frame, frame_len);
    kfree(frame);

    return result;
}

void eth_receive(netif_t *netif, uint8_t *frame, uint64_t len) {
    if (!netif || !frame || len < ETH_HEADER_LEN) return;

    eth_header_t *eth = (eth_header_t *)frame;
    uint16_t ethertype = ntohs(eth->ethertype);
    uint8_t *payload = frame + ETH_HEADER_LEN;
    uint64_t payload_len = len - ETH_HEADER_LEN;

    switch (ethertype) {
        case ETH_TYPE_ARP:
            if (payload_len >= sizeof(arp_packet_t)) {
                arp_receive(netif, (arp_packet_t *)payload);
            }
            break;

        case ETH_TYPE_IPV4:
            ipv4_receive(netif, payload, payload_len);
            break;

        case ETH_TYPE_IPV6:
            ipv6_receive(netif, payload, payload_len);
            break;

        default:
            break;
    }
}

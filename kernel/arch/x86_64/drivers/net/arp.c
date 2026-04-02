#include <kernel/drivers/net/arp.h>
#include <kernel/drivers/net/ethernet.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <kernel/pit.h>
#include <kernel/task.h>
#include <string.h>

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
}

void arp_add_entry(ipv4_addr_t ip, mac_addr_t mac) {
    int oldest_idx;
    uint64_t oldest_time;
    int i;

    oldest_idx = 0;
    oldest_time = 0xFFFFFFFF;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ipv4_eq(arp_cache[i].ip, ip)) {
            arp_cache[i].mac = mac;
            arp_cache[i].timestamp = net_get_ticks();
            return;
        }

        if (!arp_cache[i].valid) {
            oldest_idx = i;
            oldest_time = 0;
        } else if (arp_cache[i].timestamp < oldest_time) {
            oldest_time = arp_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    arp_cache[oldest_idx].ip = ip;
    arp_cache[oldest_idx].mac = mac;
    arp_cache[oldest_idx].timestamp = net_get_ticks();
    arp_cache[oldest_idx].valid = 1;
}

int arp_resolve(netif_t *netif, ipv4_addr_t ip, mac_addr_t *mac_out) {
    uint64_t timeout_ticks;
    uint64_t start;
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ipv4_eq(arp_cache[i].ip, ip)) {
            uint64_t age = net_get_ticks() - arp_cache[i].timestamp;
            if (age < ARP_ENTRY_TIMEOUT) {
                *mac_out = arp_cache[i].mac;
                return 0;
            }
            arp_cache[i].valid = 0;
        }
    }

    arp_request(netif, ip);

    timeout_ticks = pit_ms_to_ticks(3000);
    start = pit_get_ticks();
    while (pit_get_ticks() - start < timeout_ticks) {
        __asm__ volatile("sti");
        netif_poll_all();

        for (i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && ipv4_eq(arp_cache[i].ip, ip)) {
                *mac_out = arp_cache[i].mac;
                return 0;
            }
        }
        schedule();
    }

    return -1;
}

void arp_request(netif_t *netif, ipv4_addr_t target_ip) {
    arp_packet_t arp;

    if (!netif) return;

    memset(&arp, 0, sizeof(arp));

    arp.hw_type = htons(ARP_HW_ETHER);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = ETH_ALEN;
    arp.proto_len = 4;
    arp.opcode = htons(ARP_OP_REQUEST);

    memcpy(&arp.sender_mac, &netif->mac, ETH_ALEN);
    arp.sender_ip = netif->ipv4;
    memset(&arp.target_mac, 0, ETH_ALEN);
    arp.target_ip = target_ip;

    eth_send(netif, MAC_BROADCAST, ETH_TYPE_ARP, (uint8_t *)&arp, sizeof(arp));
}

void arp_receive(netif_t *netif, arp_packet_t *arp) {
    uint16_t opcode;

    if (!netif || !arp) return;

    if (ntohs(arp->hw_type) != ARP_HW_ETHER) return;
    if (ntohs(arp->proto_type) != ETH_TYPE_IPV4) return;
    if (arp->hw_len != ETH_ALEN) return;
    if (arp->proto_len != 4) return;

    arp_add_entry(arp->sender_ip, arp->sender_mac);

    opcode = ntohs(arp->opcode);

    if (opcode == ARP_OP_REQUEST) {
        if (ipv4_eq(arp->target_ip, netif->ipv4)) {
            arp_packet_t reply;
            memset(&reply, 0, sizeof(reply));

            reply.hw_type = htons(ARP_HW_ETHER);
            reply.proto_type = htons(ETH_TYPE_IPV4);
            reply.hw_len = ETH_ALEN;
            reply.proto_len = 4;
            reply.opcode = htons(ARP_OP_REPLY);

            memcpy(&reply.sender_mac, &netif->mac, ETH_ALEN);
            reply.sender_ip = netif->ipv4;
            memcpy(&reply.target_mac, &arp->sender_mac, ETH_ALEN);
            reply.target_ip = arp->sender_ip;

            eth_send(netif, arp->sender_mac, ETH_TYPE_ARP, (uint8_t *)&reply, sizeof(reply));
        }
    }
}

void arp_print_cache(void) {
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            printf("%u.%u.%u.%u -> %02X:%02X:%02X:%02X:%02X:%02X\n",
                   arp_cache[i].ip.octets[0], arp_cache[i].ip.octets[1],
                   arp_cache[i].ip.octets[2], arp_cache[i].ip.octets[3],
                   arp_cache[i].mac.addr[0], arp_cache[i].mac.addr[1],
                   arp_cache[i].mac.addr[2], arp_cache[i].mac.addr[3],
                   arp_cache[i].mac.addr[4], arp_cache[i].mac.addr[5]);
        }
    }
}

int arp_get_cache(uint64_t *ips, uint8_t *macs, int max_entries) {
    int count;
    int i;

    count = 0;
    for (i = 0; i < ARP_CACHE_SIZE && count < max_entries; i++) {
        if (arp_cache[i].valid) {
            ips[count] = (arp_cache[i].ip.octets[0] << 24) |
                        (arp_cache[i].ip.octets[1] << 16) |
                        (arp_cache[i].ip.octets[2] << 8) |
                        (arp_cache[i].ip.octets[3]);
            memcpy(&macs[count * 6], arp_cache[i].mac.addr, 6);
            count++;
        }
    }
    return count;
}

#include <lebirun/drivers/net/arp.h>
#include <lebirun/drivers/net/ethernet.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <string.h>

static arp_entry_t *arp_cache;
static int arp_cache_capacity;

void KERNEL_INIT arp_init(void) {
    arp_cache = NULL;
    arp_cache_capacity = 0;
}

static int arp_ensure_cache(void) {
    arp_entry_t *new_cache;
    int new_capacity;

    if (arp_cache) return 0;

    new_capacity = 4;
    new_cache = (arp_entry_t *)kmalloc((uint64_t)new_capacity * sizeof(arp_entry_t));
    if (!new_cache) return -1;

    memset(new_cache, 0, (uint64_t)new_capacity * sizeof(arp_entry_t));
    arp_cache = new_cache;
    arp_cache_capacity = new_capacity;
    return 0;
}

static int arp_grow_cache(void) {
    arp_entry_t *new_cache;
    int new_capacity;

    if (arp_cache_capacity >= ARP_CACHE_SIZE) return 0;

    new_capacity = arp_cache_capacity * 2;
    if (new_capacity < 4) new_capacity = 4;
    if (new_capacity > ARP_CACHE_SIZE) new_capacity = ARP_CACHE_SIZE;

    new_cache = (arp_entry_t *)kmalloc((uint64_t)new_capacity * sizeof(arp_entry_t));
    if (!new_cache) return -1;

    memset(new_cache, 0, (uint64_t)new_capacity * sizeof(arp_entry_t));
    if (arp_cache && arp_cache_capacity > 0) {
        memcpy(new_cache, arp_cache, (uint64_t)arp_cache_capacity * sizeof(arp_entry_t));
        kfree(arp_cache);
    }

    arp_cache = new_cache;
    arp_cache_capacity = new_capacity;
    return 0;
}

void arp_add_entry(ipv4_addr_t ip, mac_addr_t mac) {
    int oldest_idx;
    uint64_t oldest_time;
    int i;
    int free_idx;

    if (arp_ensure_cache() < 0) return;
    oldest_idx = 0;
    oldest_time = 0xFFFFFFFF;
    free_idx = -1;

    for (i = 0; i < arp_cache_capacity; i++) {
        if (arp_cache[i].valid && ipv4_eq(arp_cache[i].ip, ip)) {
            arp_cache[i].mac = mac;
            arp_cache[i].timestamp = net_get_ticks();
            return;
        }

        if (!arp_cache[i].valid && free_idx < 0) {
            free_idx = i;
        } else if (arp_cache[i].timestamp < oldest_time) {
            oldest_time = arp_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    if (free_idx < 0 && arp_cache_capacity < ARP_CACHE_SIZE) {
        if (arp_grow_cache() == 0) free_idx = arp_cache_capacity / 2;
    }
    if (free_idx >= 0) oldest_idx = free_idx;

    arp_cache[oldest_idx].ip = ip;
    arp_cache[oldest_idx].mac = mac;
    arp_cache[oldest_idx].timestamp = net_get_ticks();
    arp_cache[oldest_idx].valid = 1;
}

int arp_lookup(ipv4_addr_t ip, mac_addr_t *mac_out) {
    uint64_t age;
    int i;

    if (!arp_cache) return -1;

    for (i = 0; i < arp_cache_capacity; i++) {
        if (arp_cache[i].valid && ipv4_eq(arp_cache[i].ip, ip)) {
            age = net_get_ticks() - arp_cache[i].timestamp;
            if (age < ARP_ENTRY_TIMEOUT) {
                if (mac_out) *mac_out = arp_cache[i].mac;
                return 0;
            }
            arp_cache[i].valid = 0;
            return -1;
        }
    }
    return -1;
}

int arp_resolve(netif_t *netif, ipv4_addr_t ip, mac_addr_t *mac_out) {
    uint64_t timeout_ticks;
    uint64_t retry_ticks;
    uint64_t start;
    uint64_t last_request;
    int i;

    if (arp_lookup(ip, mac_out) == 0) return 0;

    timeout_ticks = pit_ms_to_ticks(3000);
    retry_ticks = pit_ms_to_ticks(250);
    start = pit_get_ticks();
    last_request = start - retry_ticks;
    while (pit_get_ticks() - start < timeout_ticks) {
        if (pit_get_ticks() - last_request >= retry_ticks) {
            arp_request(netif, ip);
            last_request = pit_get_ticks();
        }
        __asm__ volatile("sti");
        netif_poll_all();

        if (!arp_cache) {
            schedule();
            continue;
        }

        for (i = 0; i < arp_cache_capacity; i++) {
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

    if (!arp_cache) return;

    for (i = 0; i < arp_cache_capacity; i++) {
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
    if (!arp_cache) return 0;

    for (i = 0; i < arp_cache_capacity && count < max_entries; i++) {
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

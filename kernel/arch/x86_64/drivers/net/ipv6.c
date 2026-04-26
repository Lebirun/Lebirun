#include <lebirun/drivers/net/ipv6.h>
#include <lebirun/drivers/net/ethernet.h>
#include <lebirun/drivers/net/icmpv6.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

#define IPV6_NEIGHBOR_CACHE_SIZE 32
#define IPV6_NEIGHBOR_TTL       300000

typedef struct {
    ipv6_addr_t ip;
    mac_addr_t mac;
    uint64_t timestamp;
    uint8_t valid;
} ipv6_neighbor_entry_t;

extern uint64_t net_get_ticks(void);

static ipv6_neighbor_entry_t ipv6_neighbors[IPV6_NEIGHBOR_CACHE_SIZE];

static int ipv6_is_zero_addr(ipv6_addr_t ip) {
    int i;

    for (i = 0; i < 16; i++) {
        if (ip.octets[i] != 0) return 0;
    }
    return 1;
}

static int ipv6_is_multicast(ipv6_addr_t ip) {
    return ip.octets[0] == 0xff;
}

static int ipv6_is_link_local(ipv6_addr_t ip) {
    return ip.octets[0] == 0xfe && (ip.octets[1] & 0xc0) == 0x80;
}

static int ipv6_prefix_match(ipv6_addr_t a, ipv6_addr_t b, uint8_t prefix) {
    int full;
    int rem;
    int i;
    uint8_t mask;

    if (prefix == 0) return 1;
    if (prefix > 128) prefix = 128;

    full = prefix / 8;
    rem = prefix % 8;

    for (i = 0; i < full; i++) {
        if (a.octets[i] != b.octets[i]) return 0;
    }

    if (rem == 0) return 1;

    mask = (uint8_t)(0xff << (8 - rem));
    return (a.octets[full] & mask) == (b.octets[full] & mask);
}

static void ipv6_multicast_mac(ipv6_addr_t ip, mac_addr_t *mac) {
    mac->addr[0] = 0x33;
    mac->addr[1] = 0x33;
    mac->addr[2] = ip.octets[12];
    mac->addr[3] = ip.octets[13];
    mac->addr[4] = ip.octets[14];
    mac->addr[5] = ip.octets[15];
}

static ipv6_addr_t ipv6_next_hop(netif_t *netif, ipv6_addr_t dest) {
    if (ipv6_is_multicast(dest)) return dest;
    if (ipv6_is_link_local(dest)) return dest;
    if (netif && netif->ipv6_prefix > 0 && ipv6_prefix_match(netif->ipv6, dest, netif->ipv6_prefix)) return dest;
    if (netif && !ipv6_is_zero_addr(netif->ipv6_gateway)) return netif->ipv6_gateway;
    return dest;
}

void ipv6_init(void) {
    memset(ipv6_neighbors, 0, sizeof(ipv6_neighbors));
}

void ipv6_neighbor_update(ipv6_addr_t ip, mac_addr_t mac) {
    int i;
    int free_idx;
    int oldest_idx;
    uint64_t oldest_age;
    uint64_t age;

    if (ipv6_is_multicast(ip) || ipv6_is_zero_addr(ip)) return;

    free_idx = -1;
    oldest_idx = 0;
    oldest_age = 0;

    for (i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
        if (ipv6_neighbors[i].valid && ipv6_eq(ipv6_neighbors[i].ip, ip)) {
            ipv6_neighbors[i].mac = mac;
            ipv6_neighbors[i].timestamp = net_get_ticks();
            return;
        }
        if (!ipv6_neighbors[i].valid && free_idx < 0) {
            free_idx = i;
        }
        if (ipv6_neighbors[i].valid) {
            age = net_get_ticks() - ipv6_neighbors[i].timestamp;
            if (age > oldest_age) {
                oldest_age = age;
                oldest_idx = i;
            }
        }
    }

    if (free_idx < 0) free_idx = oldest_idx;
    ipv6_neighbors[free_idx].ip = ip;
    ipv6_neighbors[free_idx].mac = mac;
    ipv6_neighbors[free_idx].timestamp = net_get_ticks();
    ipv6_neighbors[free_idx].valid = 1;
}

int ipv6_neighbor_lookup(ipv6_addr_t ip, mac_addr_t *mac) {
    int i;
    uint64_t age;

    if (!mac) return -1;

    for (i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
        if (ipv6_neighbors[i].valid && ipv6_eq(ipv6_neighbors[i].ip, ip)) {
            age = net_get_ticks() - ipv6_neighbors[i].timestamp;
            if (age > IPV6_NEIGHBOR_TTL) {
                ipv6_neighbors[i].valid = 0;
                return -1;
            }
            *mac = ipv6_neighbors[i].mac;
            return 0;
        }
    }
    return -1;
}

void ipv6_create_link_local(mac_addr_t mac, ipv6_addr_t *out) {
    memset(out, 0, sizeof(ipv6_addr_t));
    out->octets[0] = 0xfe;
    out->octets[1] = 0x80;
    out->octets[8] = mac.addr[0] ^ 0x02;
    out->octets[9] = mac.addr[1];
    out->octets[10] = mac.addr[2];
    out->octets[11] = 0xff;
    out->octets[12] = 0xfe;
    out->octets[13] = mac.addr[3];
    out->octets[14] = mac.addr[4];
    out->octets[15] = mac.addr[5];
}

uint16_t ipv6_checksum(ipv6_addr_t *src, ipv6_addr_t *dest, uint8_t next_header, uint8_t *data, uint64_t len) {
    uint64_t sum;
    uint16_t *ptr;
    uint64_t data_len;
    int i;

    sum = 0;

    ptr = (uint16_t *)src->octets;
    for (i = 0; i < 8; i++) {
        sum += ntohs(ptr[i]);
    }

    ptr = (uint16_t *)dest->octets;
    for (i = 0; i < 8; i++) {
        sum += ntohs(ptr[i]);
    }

    sum += (len >> 16) & 0xffff;
    sum += len & 0xffff;
    sum += next_header;

    ptr = (uint16_t *)data;
    data_len = len;
    while (data_len > 1) {
        sum += ntohs(*ptr++);
        data_len -= 2;
    }

    if (data_len == 1) {
        sum += (*((uint8_t *)ptr)) << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return htons(~sum);
}

int ipv6_send(netif_t *netif, ipv6_addr_t dest, uint8_t next_header, uint8_t *data, uint64_t len) {
    uint64_t total_len;
    uint8_t *packet;
    ipv6_header_t *ip6;
    mac_addr_t dest_mac;
    ipv6_addr_t lookup_ip;
    int result;

    if (!netif) return -1;
    if (len > ETH_MTU - sizeof(ipv6_header_t)) return -1;

    if (ipv6_is_multicast(dest)) {
        ipv6_multicast_mac(dest, &dest_mac);
    } else {
        lookup_ip = ipv6_next_hop(netif, dest);
        if (ipv6_neighbor_lookup(lookup_ip, &dest_mac) < 0) {
            icmpv6_send_neighbor_solicitation(netif, lookup_ip);
            return -1;
        }
    }

    total_len = sizeof(ipv6_header_t) + len;
    packet = (uint8_t *)kmalloc(total_len);
    if (!packet) return -1;

    ip6 = (ipv6_header_t *)packet;
    memset(ip6, 0, sizeof(ipv6_header_t));

    ip6->version_tc_flow = htonl(0x60000000);
    ip6->payload_length = htons(len);
    ip6->next_header = next_header;
    ip6->hop_limit = 64;
    ip6->src = netif->ipv6;
    ip6->dest = dest;

    memcpy(packet + sizeof(ipv6_header_t), data, len);

    result = eth_send(netif, dest_mac, ETH_TYPE_IPV6, packet, total_len);
    kfree(packet);

    return result;
}

void ipv6_receive(netif_t *netif, uint8_t *packet, uint64_t len, mac_addr_t *src_mac) {
    ipv6_header_t *ip6;
    uint32_t version_tc_flow;
    uint16_t payload_len;
    uint8_t *payload;
    ipv6_addr_t all_nodes;
    int deliver;

    if (!netif || !packet) return;
    if (len < sizeof(ipv6_header_t)) return;

    ip6 = (ipv6_header_t *)packet;

    version_tc_flow = ntohl(ip6->version_tc_flow);
    if ((version_tc_flow >> 28) != 6) return;

    payload_len = ntohs(ip6->payload_length);
    if (sizeof(ipv6_header_t) + payload_len > len) return;

    if (src_mac) {
        ipv6_neighbor_update(ip6->src, *src_mac);
    }

    memset(&all_nodes, 0, sizeof(all_nodes));
    all_nodes.octets[0] = 0xff;
    all_nodes.octets[1] = 0x02;
    all_nodes.octets[15] = 0x01;

    deliver = 0;
    if (ipv6_eq(ip6->dest, netif->ipv6)) deliver = 1;
    if (ipv6_eq(ip6->dest, all_nodes)) deliver = 1;
    if (ipv6_is_multicast(ip6->dest) && ip6->dest.octets[11] == 0x01 && ip6->dest.octets[12] == 0xff &&
        ip6->dest.octets[13] == netif->ipv6.octets[13] &&
        ip6->dest.octets[14] == netif->ipv6.octets[14] &&
        ip6->dest.octets[15] == netif->ipv6.octets[15]) deliver = 1;

    if (!deliver) return;

    payload = packet + sizeof(ipv6_header_t);

    switch (ip6->next_header) {
        case IP_PROTO_ICMPV6:
            icmpv6_receive(netif, &ip6->src, payload, payload_len, src_mac);
            break;

        case IP_PROTO_UDP:
            udp_receive6(netif, ip6->src, ip6->dest, payload, payload_len);
            break;

        default:
            break;
    }
}

#ifndef NET_TYPES_H
#define NET_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define ETH_ALEN 6
#define ETH_MTU 1500
#define ETH_FRAME_MAX 1518
#define ETH_HEADER_LEN 14

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IPV6 0x86DD

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17
#define IP_PROTO_ICMPV6 58

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2
#define ARP_HW_ETHER   1

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

#define ICMPV6_ECHO_REQUEST 128
#define ICMPV6_ECHO_REPLY   129
#define ICMPV6_NEIGHBOR_SOLICITATION 135
#define ICMPV6_NEIGHBOR_ADVERTISEMENT 136
#define ICMPV6_ROUTER_SOLICITATION 133
#define ICMPV6_ROUTER_ADVERTISEMENT 134

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2
#define DHCP_MAGIC      0x63825363

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_DECLINE  4
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6
#define DHCP_MSG_RELEASE  7
#define DHCP_MSG_INFORM   8

#define DHCP_OPT_PAD          0
#define DHCP_OPT_SUBNET       1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS          6
#define DHCP_OPT_HOSTNAME     12
#define DHCP_OPT_DOMAIN       15
#define DHCP_OPT_BROADCAST    28
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_LIST   55
#define DHCP_OPT_END          255

#define DNS_PORT 53
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DNS_TYPE_A     1
#define DNS_TYPE_AAAA  28
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN   1

#define DNS_FLAG_QR     0x8000
#define DNS_FLAG_RD     0x0100
#define DNS_FLAG_RA     0x0080
#define DNS_FLAG_RCODE  0x000F

typedef struct {
    uint8_t addr[ETH_ALEN];
} __attribute__((packed)) mac_addr_t;

typedef struct {
    uint8_t octets[4];
} __attribute__((packed)) ipv4_addr_t;

typedef struct {
    uint8_t octets[16];
} __attribute__((packed)) ipv6_addr_t;

typedef struct {
    mac_addr_t dest;
    mac_addr_t src;
    uint16_t ethertype;
} __attribute__((packed)) eth_header_t;

typedef struct {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    ipv4_addr_t src;
    ipv4_addr_t dest;
} __attribute__((packed)) ipv4_header_t;

typedef struct {
    uint32_t version_tc_flow;
    uint16_t payload_length;
    uint8_t next_header;
    uint8_t hop_limit;
    ipv6_addr_t src;
    ipv6_addr_t dest;
} __attribute__((packed)) ipv6_header_t;

typedef struct {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t opcode;
    mac_addr_t sender_mac;
    ipv4_addr_t sender_ip;
    mac_addr_t target_mac;
    ipv4_addr_t target_ip;
} __attribute__((packed)) arp_packet_t;

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint32_t data;
} __attribute__((packed)) icmpv6_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

typedef struct {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    ipv4_addr_t ciaddr;
    ipv4_addr_t yiaddr;
    ipv4_addr_t siaddr;
    ipv4_addr_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[308];
} __attribute__((packed)) dhcp_packet_t;

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t rdlength;
} __attribute__((packed)) dns_rr_header_t;

typedef struct net_buffer {
    uint8_t *data;
    uint32_t len;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    struct net_buffer *next;
} net_buffer_t;

struct netif;
typedef struct netif netif_t;

typedef int (*netif_send_t)(netif_t *netif, uint8_t *data, uint32_t len);
typedef int (*netif_poll_t)(netif_t *netif);

struct netif {
    char name[16];
    mac_addr_t mac;
    ipv4_addr_t ipv4;
    ipv4_addr_t netmask;
    ipv4_addr_t gateway;
    ipv4_addr_t dns_server;
    ipv4_addr_t dns_server2;
    ipv6_addr_t ipv6;
    ipv6_addr_t ipv6_gateway;
    uint8_t ipv6_prefix;
    uint32_t mtu;
    uint8_t link_up;
    uint8_t dhcp_configured;
    netif_send_t send;
    netif_poll_t poll;
    void *driver_data;
    netif_t *next;
};

#define ARP_CACHE_SIZE 64
#define ARP_ENTRY_TIMEOUT 300000

typedef struct {
    ipv4_addr_t ip;
    mac_addr_t mac;
    uint32_t timestamp;
    uint8_t valid;
} arp_entry_t;

#define TCP_STATE_CLOSED      0
#define TCP_STATE_LISTEN      1
#define TCP_STATE_SYN_SENT    2
#define TCP_STATE_SYN_RCVD    3
#define TCP_STATE_ESTABLISHED 4
#define TCP_STATE_FIN_WAIT1   5
#define TCP_STATE_FIN_WAIT2   6
#define TCP_STATE_CLOSE_WAIT  7
#define TCP_STATE_CLOSING     8
#define TCP_STATE_LAST_ACK    9
#define TCP_STATE_TIME_WAIT   10

#define TCP_WINDOW_SIZE 8192
#define TCP_MSS 1460

typedef struct tcp_socket {
    uint8_t state;
    ipv4_addr_t local_ip;
    ipv4_addr_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t send_next;
    uint32_t send_una;
    uint32_t recv_next;
    uint16_t send_window;
    uint16_t recv_window;
    uint8_t *recv_buffer;
    uint32_t recv_buffer_size;
    uint32_t recv_buffer_head;
    uint32_t recv_buffer_tail;
    uint8_t *send_buffer;
    uint32_t send_buffer_size;
    uint32_t send_buffer_head;
    uint32_t send_buffer_tail;
    uint32_t retransmit_timeout;
    uint32_t last_ack_time;
    netif_t *netif;
    struct tcp_socket *next;
} tcp_socket_t;

typedef struct udp_socket {
    ipv4_addr_t local_ip;
    uint16_t local_port;
    uint8_t *recv_buffer;
    uint32_t recv_buffer_size;
    uint32_t recv_len;
    ipv4_addr_t recv_from_ip;
    uint16_t recv_from_port;
    uint8_t has_data;
    netif_t *netif;
    struct udp_socket *next;
} udp_socket_t;

static inline uint16_t htons(uint16_t x) {
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}

static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

static inline uint32_t ipv4_to_u32(ipv4_addr_t ip) {
    return (ip.octets[0] << 24) | (ip.octets[1] << 16) |
           (ip.octets[2] << 8) | ip.octets[3];
}

static inline ipv4_addr_t u32_to_ipv4(uint32_t val) {
    ipv4_addr_t ip;
    ip.octets[0] = (val >> 24) & 0xFF;
    ip.octets[1] = (val >> 16) & 0xFF;
    ip.octets[2] = (val >> 8) & 0xFF;
    ip.octets[3] = val & 0xFF;
    return ip;
}

static inline int ipv4_eq(ipv4_addr_t a, ipv4_addr_t b) {
    return a.octets[0] == b.octets[0] && a.octets[1] == b.octets[1] &&
           a.octets[2] == b.octets[2] && a.octets[3] == b.octets[3];
}

static inline int mac_eq(mac_addr_t a, mac_addr_t b) {
    for (int i = 0; i < ETH_ALEN; i++) {
        if (a.addr[i] != b.addr[i]) return 0;
    }
    return 1;
}

static inline int ipv6_eq(ipv6_addr_t a, ipv6_addr_t b) {
    for (int i = 0; i < 16; i++) {
        if (a.octets[i] != b.octets[i]) return 0;
    }
    return 1;
}

#define IPV4_ADDR(a,b,c,d) ((ipv4_addr_t){{a,b,c,d}})
#define IPV4_ZERO IPV4_ADDR(0,0,0,0)
#define IPV4_BROADCAST IPV4_ADDR(255,255,255,255)

extern const mac_addr_t MAC_BROADCAST;
extern const mac_addr_t MAC_ZERO;
extern const ipv6_addr_t IPV6_ZERO;

#endif

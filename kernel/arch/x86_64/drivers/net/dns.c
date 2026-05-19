#include <lebirun/drivers/net/dns.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <string.h>

static ipv4_addr_t g_dns_server;
static ipv4_addr_t g_dns_server2;
static dns_cache_entry_t *dns_cache = NULL;
static int dns_cache_capacity = 0;
#define DNS_CACHE_INIT 1
static uint16_t dns_id_counter = 1;

static ipv4_addr_t pending_result;
static ipv6_addr_t pending_result6;
static uint8_t pending_resolved;
static uint16_t pending_id;
static uint16_t pending_qtype;

static int dns_ensure_cache(void) {
    if (dns_cache)
        return 0;

    dns_cache_capacity = DNS_CACHE_INIT;
    dns_cache = (dns_cache_entry_t *)kmalloc(dns_cache_capacity * sizeof(dns_cache_entry_t));
    if (!dns_cache) {
        dns_cache_capacity = 0;
        return -1;
    }
    memset(dns_cache, 0, dns_cache_capacity * sizeof(dns_cache_entry_t));
    return 0;
}

void dns_init(void) {
    dns_cache = NULL;
    dns_cache_capacity = 0;
    g_dns_server = IPV4_ADDR(8, 8, 8, 8);
    g_dns_server2 = IPV4_ADDR(8, 8, 4, 4);
    dns_id_counter = 1;
    pending_resolved = 0;
    pending_qtype = DNS_TYPE_A;
}

void dns_set_server(ipv4_addr_t server) {
    g_dns_server = server;
}

void dns_set_server2(ipv4_addr_t server) {
    g_dns_server2 = server;
}

static int dns_cache_lookup(const char *name, ipv4_addr_t *out_ipv4) {
    int i;
    int j;
    int match;
    uint64_t age;

    if (!dns_cache)
        return -1;

    for (i = 0; i < dns_cache_capacity; i++) {
        if (dns_cache[i].has_ipv4) {
            match = 1;
            for (j = 0; name[j] && j < 255; j++) {
                if (dns_cache[i].name[j] != name[j]) {
                    match = 0;
                    break;
                }
            }
            if (match && dns_cache[i].name[j] != '\0') {
                match = 0;
            }
            if (match) {
                age = net_get_ticks() - dns_cache[i].timestamp;
                if (age < dns_cache[i].ttl * 1000 && age < DNS_CACHE_TTL) {
                    *out_ipv4 = dns_cache[i].ipv4;
                    return 0;
                }
                dns_cache[i].has_ipv4 = 0;
            }
        }
    }
    return -1;
}

void dns_cache_add(const char *name, ipv4_addr_t ip, uint64_t ttl) {
    int oldest_idx;
    uint64_t oldest_time;
    int i;
    int len;

    if (dns_ensure_cache() < 0)
        return;

    oldest_idx = 0;
    oldest_time = 0xFFFFFFFF;

    for (i = 0; i < dns_cache_capacity; i++) {
        if (!dns_cache[i].has_ipv4) {
            oldest_idx = i;
            break;
        }
        if (dns_cache[i].timestamp < oldest_time) {
            oldest_time = dns_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    len = 0;
    while (name[len] && len < 255) {
        dns_cache[oldest_idx].name[len] = name[len];
        len++;
    }
    dns_cache[oldest_idx].name[len] = 0;
    dns_cache[oldest_idx].ipv4 = ip;
    dns_cache[oldest_idx].has_ipv4 = 1;
    dns_cache[oldest_idx].timestamp = net_get_ticks();
    dns_cache[oldest_idx].ttl = ttl;
}

static int dns_encode_name(const char *name, uint8_t *buffer, int buffer_size) {
    int offset;
    int label_start;
    int i;
    int label_len;
    int j;

    offset = 0;
    label_start = 0;

    for (i = 0; ; i++) {
        if (name[i] == '.' || name[i] == '\0') {
            label_len = i - label_start;
            if (label_len > 63) return -1;
            if (offset + 1 + label_len >= buffer_size) return -1;
            buffer[offset++] = label_len;
            for (j = label_start; j < i; j++) {
                buffer[offset++] = name[j];
            }
            label_start = i + 1;
            if (name[i] == '\0') break;
        }
    }
    if (offset >= buffer_size) return -1;
    buffer[offset++] = 0;
    return offset;
}

static int dns_decode_name(uint8_t *packet, uint64_t packet_len, uint64_t offset, char *name, int max_len) {
    int name_offset;
    int jumped;
    int jump_count;
    uint8_t len;
    uint16_t ptr;
    int i;

    name_offset = 0;
    jumped = 0;
    jump_count = 0;

    while (offset < packet_len && jump_count < 10) {
        len = packet[offset];
        if (len == 0) {
            offset++;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (offset + 1 >= packet_len) return -1;
            ptr = ((len & 0x3F) << 8) | packet[offset + 1];
            if (!jumped) offset += 2;
            jumped = 1;
            offset = ptr;
            jump_count++;
            continue;
        }
        offset++;
        if (name_offset > 0 && name_offset < max_len - 1) {
            name[name_offset++] = '.';
        }
        for (i = 0; i < len && offset < packet_len && name_offset < max_len - 1; i++) {
            name[name_offset++] = packet[offset++];
        }
    }
    name[name_offset] = '\0';
    return jumped ? -1 : (int)offset;
}

int dns_resolve(const char *hostname, ipv4_addr_t *out_ipv4) {
    return dns_resolve_timeout(hostname, out_ipv4, 5000);
}

int dns_resolve_timeout(const char *hostname, ipv4_addr_t *out_ipv4, uint64_t timeout_ms) {
    netif_t *netif;
    uint8_t *query;
    dns_header_t *hdr;
    uint16_t id;
    int name_len;
    int send_result;
    uint8_t *qtype;
    uint64_t query_len;
    uint64_t timeout_ticks;
    uint64_t start;

    if (!hostname || !out_ipv4) return -1;

    net_ensure_hw();

    if (dns_cache_lookup(hostname, out_ipv4) == 0) {
        return 0;
    }

    netif = netif_get_default();
    if (!netif) return -1;

    query = (uint8_t *)kmalloc(512);
    if (!query) return -1;
    memset(query, 0, 512);

    hdr = (dns_header_t *)query;
    id = dns_id_counter++;
    hdr->id = htons(id);
    hdr->flags = htons(DNS_FLAG_RD);
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    name_len = dns_encode_name(hostname, query + sizeof(dns_header_t), 512 - (int)sizeof(dns_header_t) - 4);
    if (name_len < 0) { kfree(query); return -1; }

    qtype = query + sizeof(dns_header_t) + name_len;
    qtype[0] = 0;
    qtype[1] = DNS_TYPE_A;
    qtype[2] = 0;
    qtype[3] = DNS_CLASS_IN;

    query_len = sizeof(dns_header_t) + name_len + 4;

    pending_id = id;
    pending_qtype = DNS_TYPE_A;
    pending_resolved = 0;

    send_result = udp_send(netif, g_dns_server, 53, DNS_PORT, query, query_len);
    kfree(query);
    if (send_result < 0) return -1;

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();
    while (!pending_resolved) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (task_has_pending_signals()) {
            return -1;
        }
        if (pit_get_ticks() - start > timeout_ticks) {
            return -1;
        }
        schedule();
    }

    *out_ipv4 = pending_result;
    dns_cache_add(hostname, pending_result, 300);
    return 0;
}

int dns_resolve6(const char *hostname, ipv6_addr_t *out_ipv6) {
    netif_t *netif;
    uint8_t *query;
    dns_header_t *hdr;
    uint16_t id;
    int name_len;
    int send_result;
    uint8_t *qtype;
    uint64_t query_len;
    uint64_t timeout_ticks;
    uint64_t start;

    if (!hostname || !out_ipv6) return -1;

    net_ensure_hw();

    netif = netif_get_default();
    if (!netif) return -1;

    query = (uint8_t *)kmalloc(512);
    if (!query) return -1;
    memset(query, 0, 512);

    hdr = (dns_header_t *)query;
    id = dns_id_counter++;
    hdr->id = htons(id);
    hdr->flags = htons(DNS_FLAG_RD);
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    name_len = dns_encode_name(hostname, query + sizeof(dns_header_t), 512 - (int)sizeof(dns_header_t) - 4);
    if (name_len < 0) { kfree(query); return -1; }

    qtype = query + sizeof(dns_header_t) + name_len;
    qtype[0] = 0;
    qtype[1] = DNS_TYPE_AAAA;
    qtype[2] = 0;
    qtype[3] = DNS_CLASS_IN;

    query_len = sizeof(dns_header_t) + name_len + 4;

    pending_id = id;
    pending_qtype = DNS_TYPE_AAAA;
    pending_resolved = 0;

    send_result = udp_send(netif, g_dns_server, 53, DNS_PORT, query, query_len);
    kfree(query);
    if (send_result < 0) return -1;

    timeout_ticks = pit_ms_to_ticks(5000);
    start = pit_get_ticks();
    while (!pending_resolved) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (task_has_pending_signals()) {
            return -1;
        }
        if (pit_get_ticks() - start > timeout_ticks) {
            return -1;
        }
        schedule();
    }

    memcpy(out_ipv6, &pending_result6, sizeof(ipv6_addr_t));
    return 0;
}

void dns_receive(netif_t *netif, ipv4_addr_t src, uint16_t src_port, uint8_t *data, uint64_t len) {
    dns_header_t *hdr;
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint64_t offset;
    uint16_t qi;
    uint16_t ai;
    uint8_t l;
    char name[256];
    int new_offset;
    uint16_t rtype;
    uint16_t rdlength;

    (void)netif;

    if (len < sizeof(dns_header_t)) return;

    if (src_port != DNS_PORT) return;

    if (!ipv4_eq(src, g_dns_server) && !ipv4_eq(src, g_dns_server2)) return;

    hdr = (dns_header_t *)data;
    id = ntohs(hdr->id);
    flags = ntohs(hdr->flags);

    if (!(flags & DNS_FLAG_QR)) return;
    if (id != pending_id) return;
    if ((flags & DNS_FLAG_RCODE) != 0) return;

    qdcount = ntohs(hdr->qdcount);
    ancount = ntohs(hdr->ancount);

    if (ancount == 0) return;

    offset = sizeof(dns_header_t);

    for (qi = 0; qi < qdcount; qi++) {
        while (offset < len) {
            l = data[offset];
            if (l == 0) { offset++; break; }
            if ((l & 0xC0) == 0xC0) { offset += 2; break; }
            offset += l + 1;
        }
        offset += 4;
    }

    for (ai = 0; ai < ancount && offset < len; ai++) {
        new_offset = dns_decode_name(data, len, offset, name, sizeof(name));
        if (new_offset < 0) {
            while (offset < len) {
                l = data[offset];
                if (l == 0) { offset++; break; }
                if ((l & 0xC0) == 0xC0) { offset += 2; break; }
                offset += l + 1;
            }
        } else {
            offset = new_offset;
        }

        if (offset + 10 > len) break;
        rtype = (data[offset] << 8) | data[offset + 1];
        rdlength = (data[offset + 8] << 8) | data[offset + 9];
        offset += 10;

        if (pending_qtype == DNS_TYPE_A && rtype == DNS_TYPE_A && rdlength == 4 && offset + 4 <= len) {
            pending_result.octets[0] = data[offset];
            pending_result.octets[1] = data[offset + 1];
            pending_result.octets[2] = data[offset + 2];
            pending_result.octets[3] = data[offset + 3];
            pending_resolved = 1;
            return;
        }

        if (pending_qtype == DNS_TYPE_AAAA && rtype == DNS_TYPE_AAAA && rdlength == 16 && offset + 16 <= len) {
            memcpy(&pending_result6.octets[0], data + offset, 16);
            pending_resolved = 1;
            return;
        }

        offset += rdlength;
    }
}

void dns_cache_print(void) {
    int i;

    if (!dns_cache)
        return;

    for (i = 0; i < dns_cache_capacity; i++) {
        if (dns_cache[i].has_ipv4) {
            printf("%s -> %u.%u.%u.%u\n",
                   dns_cache[i].name,
                   dns_cache[i].ipv4.octets[0], dns_cache[i].ipv4.octets[1],
                   dns_cache[i].ipv4.octets[2], dns_cache[i].ipv4.octets[3]);
        }
    }
}

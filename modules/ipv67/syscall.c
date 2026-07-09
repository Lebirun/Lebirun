#include <lebirun/lke.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/drivers/net/ipv67.h>
#include <lebirun/drivers/net/dns.h>
#include <string.h>

#define EFAULT 14
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22

#define SYSCALL_IPV67 280

int ipv67_get_peer_at(int index, ipv67_peer_t *out);
int ipv67_get_route_at(int index, ipv67_route_t *out);
int ipv67_get_asn_at(int index, ipv67_asn_claim_t *out);
void ipv67_rx_flush_port(uint16_t port);
int ipv67_context_destroy(uint16_t port);

#define IPV67_CMD_INIT         0
#define IPV67_CMD_SET_ADDR     1
#define IPV67_CMD_GET_ADDR     2
#define IPV67_CMD_ADD_PEER     3
#define IPV67_CMD_REMOVE_PEER  4
#define IPV67_CMD_GET_PEERS    5
#define IPV67_CMD_PEER_COUNT   6
#define IPV67_CMD_SEND         7
#define IPV67_CMD_PING         8
#define IPV67_CMD_SET_PORT     9
#define IPV67_CMD_GET_PORT     10
#define IPV67_CMD_ADDR_PARSE   11
#define IPV67_CMD_ADDR_FORMAT  12
#define IPV67_CMD_ADD_PEER6    13
#define IPV67_CMD_ADD_PEER_HOST 14
#define IPV67_CMD_ADD_ENDPOINT 15
#define IPV67_CMD_PROBE_PEERS  16
#define IPV67_CMD_GET_ROUTES   17
#define IPV67_CMD_ROUTE_COUNT  18
#define IPV67_CMD_SET_AUTH_KEY 19
#define IPV67_CMD_SET_IDENTITY_KEY 20
#define IPV67_CMD_SET_ASN      21
#define IPV67_CMD_REMOVE_ASN   22
#define IPV67_CMD_GET_ASNS     23
#define IPV67_CMD_ASN_COUNT    24
#define IPV67_CMD_GET_STATS    25
#define IPV67_CMD_CLEAR_STATS  26
#define IPV67_CMD_SET_AUTH_REQUIRED 28
#define IPV67_CMD_GET_AUTH_REQUIRED 29
#define IPV67_CMD_GET_LOCAL_ASN 30
#define IPV67_CMD_PUNCH        31
#define IPV67_CMD_SHUTDOWN     32

typedef struct {
    int      cmd;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
} __attribute__((packed)) ipv67_syscall_req_t;

typedef struct {
    uint32_t ipv4;
    ipv6_addr_t ipv6;
    uint16_t port;
    uint8_t family;
    uint8_t authenticated;
    uint8_t session_established;
    uint8_t missed_probes;
    uint8_t addr_verified;
    char alias[IPV67_ALIAS_SIZE];
    uint8_t public_key[IPV67_IDENTITY_SIZE];
    char     addr_str[IPV67_ADDR_STR_MAX];
} __attribute__((packed)) ipv67_peer_user_t;

typedef struct {
    uint32_t next_hop_ipv4;
    ipv6_addr_t next_hop_ipv6;
    uint16_t next_hop_port;
    uint8_t next_hop_family;
    uint8_t hops;
    uint8_t metric;
    uint32_t sequence;
    uint8_t public_key[IPV67_IDENTITY_SIZE];
    char dest_str[IPV67_ADDR_STR_MAX];
} __attribute__((packed)) ipv67_route_user_t;

typedef struct {
    uint32_t asn;
    uint8_t specificity;
    uint8_t flags;
    uint8_t conflict_count;
    char country[IPV67_ASN_COUNTRY_SIZE];
    char source[IPV67_ASN_SOURCE_SIZE];
    char name[IPV67_ASN_NAME_SIZE];
    char label[IPV67_ASN_LABEL_SIZE];
    char start_str[IPV67_ADDR_STR_MAX];
    char end_str[IPV67_ADDR_STR_MAX];
} __attribute__((packed)) ipv67_asn_user_t;

static uint64_t get_user_pd_ipv67(void) {
    if (!current_task) return 0;
    if (current_task->cr3) return current_task->cr3;
    return current_task->pml4_phys;
}

static int user_range_ok_ipv67(uint64_t addr, uint64_t size) {
    uint64_t end;
    if (size == 0) return 0;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return 0;
    end = addr + size - 1;
    if (end < addr || end >= KERNEL_VMA) return 0;
    return 1;
}

static int user_range_mapped_ipv67(uint64_t addr, uint64_t size) {
    uint64_t pd;
    uint64_t start;
    uint64_t end;
    uint64_t p;
    if (!user_range_ok_ipv67(addr, size)) return 0;
    pd = get_user_pd_ipv67();
    if (!pd) return 0;
    start = addr & ~0xFFFu;
    end = (addr + size - 1) & ~0xFFFu;
    for (p = start; ; p += 0x1000) {
        if (vmm_get_phys_in_pml4(pd, p) == 0) return 0;
        if (p == end) break;
        if (p > end) return 0;
    }
    return 1;
}

static int ipv67_readonly_cmd(int cmd) {
    if (cmd == IPV67_CMD_GET_ADDR) return 1;
    if (cmd == IPV67_CMD_GET_PEERS) return 1;
    if (cmd == IPV67_CMD_PEER_COUNT) return 1;
    if (cmd == IPV67_CMD_GET_ROUTES) return 1;
    if (cmd == IPV67_CMD_ROUTE_COUNT) return 1;
    if (cmd == IPV67_CMD_GET_ASNS) return 1;
    if (cmd == IPV67_CMD_ASN_COUNT) return 1;
    if (cmd == IPV67_CMD_GET_STATS) return 1;
    if (cmd == IPV67_CMD_GET_AUTH_REQUIRED) return 1;
    if (cmd == IPV67_CMD_GET_LOCAL_ASN) return 1;
    if (cmd == IPV67_CMD_GET_PORT) return 1;
    return 0;
}

static int ipv67_existing_context_cmd(int cmd) {
    if (cmd == IPV67_CMD_CLEAR_STATS) return 1;
    return 0;
}

static int ipv67_empty_readonly_result(const ipv67_syscall_req_t *req) {
    ipv67_addr_t addr;
    ipv67_stats_t stats;

    if (!req) return -EINVAL;
    if (req->cmd == IPV67_CMD_GET_ADDR) {
        if (!user_range_mapped_ipv67(req->arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        memset(&addr, 0, sizeof(addr));
        memcpy((void *)req->arg1, &addr, sizeof(addr));
        return 0;
    }
    if (req->cmd == IPV67_CMD_GET_STATS) {
        if (!user_range_mapped_ipv67(req->arg1, sizeof(ipv67_stats_t))) return -EFAULT;
        memset(&stats, 0, sizeof(stats));
        memcpy((void *)req->arg1, &stats, sizeof(stats));
        return 0;
    }
    if (req->cmd == IPV67_CMD_GET_LOCAL_ASN) return 0;
    if (req->cmd == IPV67_CMD_GET_AUTH_REQUIRED) return 0;
    if (req->cmd == IPV67_CMD_GET_PORT) return req->arg4 ? (int)(uint16_t)req->arg4 : IPV67_PORT_DEFAULT;
    return 0;
}

static int sys_ipv67_impl(const char *req_ptr, int unused1, int unused2) {
    ipv67_syscall_req_t req;
    ipv67_addr_t addr;
    ipv6_addr_t addr6;
    ipv67_peer_t kernel_peer;
    ipv67_route_t kernel_route;
    ipv67_asn_claim_t kernel_asn;
    ipv67_stats_t kernel_stats;
    ipv67_peer_user_t user_peer;
    ipv67_route_user_t user_route;
    ipv67_asn_user_t user_asn;
    ipv67_asn_claim_t asn_claim;
    char addr_str[IPV67_ADDR_STR_MAX];
    char host_str[256];
    const char *src;
    ipv4_addr_t resolved4;
    ipv6_addr_t resolved6;
    uint64_t req_addr;
    int local_asn;
    int count;
    int max;
    int i;
    int ret;
    uint64_t j;

    (void)unused1;
    (void)unused2;
    req_addr = (uint64_t)(uintptr_t)req_ptr;
    if (!user_range_mapped_ipv67(req_addr, sizeof(ipv67_syscall_req_t))) return -EFAULT;
    memcpy(&req, (const void *)req_addr, sizeof(ipv67_syscall_req_t));
    if (req.cmd != IPV67_CMD_ADDR_PARSE && req.cmd != IPV67_CMD_ADDR_FORMAT && req.cmd != IPV67_CMD_SET_PORT && req.cmd != IPV67_CMD_SHUTDOWN) {
        if (ipv67_readonly_cmd(req.cmd) || ipv67_existing_context_cmd(req.cmd)) {
            ret = ipv67_use_port_existing((uint16_t)req.arg4);
            if (ret < 0) {
                if (ipv67_readonly_cmd(req.cmd)) return ipv67_empty_readonly_result(&req);
                return 0;
            }
        } else {
            ret = ipv67_use_port((uint16_t)req.arg4);
        }
        if (ret < 0) return ret;
    }

    switch (req.cmd) {
    case IPV67_CMD_INIT:
        return ipv67_init();

    case IPV67_CMD_SHUTDOWN:
        ipv67_rx_flush_port((uint16_t)req.arg4);
        return ipv67_context_destroy((uint16_t)req.arg4);

    case IPV67_CMD_SET_ADDR:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_set_self_addr(&addr);

    case IPV67_CMD_GET_ADDR:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        ipv67_self_addr(&addr);
        memcpy((void *)req.arg1, &addr, sizeof(ipv67_addr_t));
        return 0;

    case IPV67_CMD_ADD_PEER:
        if (!user_range_mapped_ipv67(req.arg3, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr, (const void *)req.arg3, sizeof(ipv67_addr_t));
        return ipv67_add_peer_with_addr((uint32_t)req.arg1, (uint16_t)req.arg2, &addr);

    case IPV67_CMD_ADD_PEER6:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv6_addr_t))) return -EFAULT;
        if (!user_range_mapped_ipv67(req.arg3, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr6, (const void *)req.arg1, sizeof(ipv6_addr_t));
        memcpy(&addr, (const void *)req.arg3, sizeof(ipv67_addr_t));
        return ipv67_add_peer6(&addr6, (uint16_t)req.arg2, &addr);

    case IPV67_CMD_ADD_PEER_HOST:
        if (!user_range_mapped_ipv67(req.arg1, 1)) return -EFAULT;
        if (!user_range_mapped_ipv67(req.arg3, sizeof(ipv67_addr_t))) return -EFAULT;
        for (j = 0; j < sizeof(host_str); j++) {
            if (!user_range_mapped_ipv67(req.arg1 + j, 1)) break;
            host_str[j] = ((const char *)req.arg1)[j];
            if (host_str[j] == '\0') break;
        }
        host_str[sizeof(host_str) - 1] = '\0';
        memcpy(&addr, (const void *)req.arg3, sizeof(ipv67_addr_t));
        ret = dns_resolve_timeout(host_str, &resolved4, 5000);
        if (ret == 0) {
            return ipv67_add_peer_with_addr(ipv4_to_u32(resolved4), (uint16_t)req.arg2, &addr);
        }
        ret = dns_resolve6(host_str, &resolved6);
        if (ret == 0) {
            return ipv67_add_peer6(&resolved6, (uint16_t)req.arg2, &addr);
        }
        return IPV67_ERR_NOROUTE;

    case IPV67_CMD_ADD_ENDPOINT:
        if (req.arg3 == IPV67_PEER_IPV4) {
            return ipv67_add_peer_with_addr((uint32_t)req.arg1, (uint16_t)req.arg2, NULL);
        }
        if (req.arg3 == IPV67_PEER_IPV6) {
            if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv6_addr_t))) return -EFAULT;
            memcpy(&addr6, (const void *)req.arg1, sizeof(ipv6_addr_t));
            return ipv67_add_peer6(&addr6, (uint16_t)req.arg2, NULL);
        }
        if (!user_range_mapped_ipv67(req.arg1, 1)) return -EFAULT;
        for (j = 0; j < sizeof(host_str); j++) {
            if (!user_range_mapped_ipv67(req.arg1 + j, 1)) break;
            host_str[j] = ((const char *)req.arg1)[j];
            if (host_str[j] == '\0') break;
        }
        host_str[sizeof(host_str) - 1] = '\0';
        ret = dns_resolve_timeout(host_str, &resolved4, 5000);
        if (ret == 0) {
            return ipv67_add_peer_with_addr(ipv4_to_u32(resolved4), (uint16_t)req.arg2, NULL);
        }
        ret = dns_resolve6(host_str, &resolved6);
        if (ret == 0) {
            return ipv67_add_peer6(&resolved6, (uint16_t)req.arg2, NULL);
        }
        return IPV67_ERR_NOROUTE;

    case IPV67_CMD_REMOVE_PEER:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_remove_peer_by_addr(&addr);

    case IPV67_CMD_GET_PEERS:
        if (req.arg2 == 0) return 0;
        max = (int)req.arg2;
        if ((uint64_t)max != req.arg2 || max < 0) return -EINVAL;
        count = ipv67_peer_count();
        if (max > count) max = count;
        if (max == 0) return 0;
        if (!user_range_mapped_ipv67(req.arg1, (uint64_t)max * sizeof(ipv67_peer_user_t))) return -EFAULT;
        for (i = 0; i < max; i++) {
            ret = ipv67_get_peer_at(i, &kernel_peer);
            if (ret < 0) return i;
            memset(&user_peer, 0, sizeof(user_peer));
            user_peer.ipv4 = kernel_peer.ipv4;
            memcpy(&user_peer.ipv6, &kernel_peer.ipv6, sizeof(ipv6_addr_t));
            user_peer.port = kernel_peer.port;
            user_peer.family = kernel_peer.family;
            user_peer.authenticated = kernel_peer.authenticated;
            user_peer.session_established = kernel_peer.session_established;
            user_peer.missed_probes = kernel_peer.missed_probes;
            user_peer.addr_verified = ipv67_peer_addr_verified(&kernel_peer) ? 1 : 0;
            memcpy(user_peer.alias, kernel_peer.alias, IPV67_ALIAS_SIZE);
            memcpy(user_peer.public_key, kernel_peer.public_key, IPV67_IDENTITY_SIZE);
            ipv67_addr_format(&kernel_peer.addr, user_peer.addr_str, IPV67_ADDR_STR_MAX);
            memcpy((void *)(req.arg1 + (uint64_t)i * sizeof(ipv67_peer_user_t)), &user_peer, sizeof(ipv67_peer_user_t));
        }
        return max;

    case IPV67_CMD_PEER_COUNT:
        return ipv67_peer_count();

    case IPV67_CMD_ROUTE_COUNT:
        return ipv67_route_count_get();

    case IPV67_CMD_ASN_COUNT:
        return ipv67_asn_count_get();

    case IPV67_CMD_GET_LOCAL_ASN:
        return ipv67_get_local_asn();

    case IPV67_CMD_GET_STATS:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_stats_t))) return -EFAULT;
        memset(&kernel_stats, 0, sizeof(kernel_stats));
        ret = ipv67_get_stats(&kernel_stats);
        if (ret < 0) return ret;
        memcpy((void *)req.arg1, &kernel_stats, sizeof(kernel_stats));
        return 0;

    case IPV67_CMD_CLEAR_STATS:
        ipv67_clear_stats();
        return 0;

    case IPV67_CMD_GET_ROUTES:
        if (req.arg2 == 0) return 0;
        max = (int)req.arg2;
        if ((uint64_t)max != req.arg2 || max < 0) return -EINVAL;
        count = ipv67_route_count_get();
        if (max > count) max = count;
        if (max == 0) return 0;
        if (!user_range_mapped_ipv67(req.arg1, (uint64_t)max * sizeof(ipv67_route_user_t))) return -EFAULT;
        for (i = 0; i < max; i++) {
            ret = ipv67_get_route_at(i, &kernel_route);
            if (ret < 0) return i;
            memset(&user_route, 0, sizeof(user_route));
            user_route.next_hop_ipv4 = kernel_route.next_hop_ipv4;
            memcpy(&user_route.next_hop_ipv6, &kernel_route.next_hop_ipv6, sizeof(ipv6_addr_t));
            user_route.next_hop_port = kernel_route.next_hop_port;
            user_route.next_hop_family = kernel_route.next_hop_family;
            user_route.hops = kernel_route.hops;
            user_route.metric = kernel_route.metric;
            user_route.sequence = kernel_route.sequence;
            memcpy(user_route.public_key, kernel_route.public_key, IPV67_IDENTITY_SIZE);
            ipv67_addr_format(&kernel_route.dest, user_route.dest_str, IPV67_ADDR_STR_MAX);
            memcpy((void *)(req.arg1 + (uint64_t)i * sizeof(ipv67_route_user_t)), &user_route, sizeof(ipv67_route_user_t));
        }
        return max;

    case IPV67_CMD_GET_ASNS:
        if (req.arg2 == 0) return 0;
        max = (int)req.arg2;
        if ((uint64_t)max != req.arg2 || max < 0) return -EINVAL;
        count = ipv67_asn_count_get();
        if (max > count) max = count;
        if (max == 0) return 0;
        if (!user_range_mapped_ipv67(req.arg1, (uint64_t)max * sizeof(ipv67_asn_user_t))) return -EFAULT;
        for (i = 0; i < max; i++) {
            ret = ipv67_get_asn_at(i, &kernel_asn);
            if (ret < 0) return i;
            memset(&user_asn, 0, sizeof(user_asn));
            user_asn.asn = kernel_asn.asn;
            user_asn.specificity = kernel_asn.specificity;
            user_asn.flags = kernel_asn.flags;
            user_asn.conflict_count = kernel_asn.conflict_count;
            memcpy(user_asn.country, kernel_asn.country, IPV67_ASN_COUNTRY_SIZE);
            memcpy(user_asn.source, kernel_asn.source, IPV67_ASN_SOURCE_SIZE);
            memcpy(user_asn.name, kernel_asn.name, IPV67_ASN_NAME_SIZE);
            memcpy(user_asn.label, kernel_asn.label, IPV67_ASN_LABEL_SIZE);
            ipv67_addr_format(&kernel_asn.start, user_asn.start_str, IPV67_ADDR_STR_MAX);
            ipv67_addr_format(&kernel_asn.end, user_asn.end_str, IPV67_ADDR_STR_MAX);
            memcpy((void *)(req.arg1 + (uint64_t)i * sizeof(ipv67_asn_user_t)), &user_asn, sizeof(user_asn));
        }
        return max;

    case IPV67_CMD_SET_ASN:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_asn_user_t))) return -EFAULT;
        memcpy(&user_asn, (const void *)req.arg1, sizeof(user_asn));
        user_asn.start_str[IPV67_ADDR_STR_MAX - 1] = '\0';
        user_asn.end_str[IPV67_ADDR_STR_MAX - 1] = '\0';
        memset(&asn_claim, 0, sizeof(asn_claim));
        asn_claim.asn = user_asn.asn;
        if (asn_claim.asn == 0) {
            local_asn = ipv67_get_local_asn();
            if (local_asn <= 0) return -EINVAL;
            asn_claim.asn = (uint32_t)local_asn;
        }
        asn_claim.specificity = user_asn.specificity;
        asn_claim.flags = user_asn.flags;
        memcpy(asn_claim.country, user_asn.country, IPV67_ASN_COUNTRY_SIZE);
        memcpy(asn_claim.name, user_asn.name, IPV67_ASN_NAME_SIZE);
        memcpy(asn_claim.label, user_asn.label, IPV67_ASN_LABEL_SIZE);
        ret = ipv67_addr_parse(user_asn.start_str, &asn_claim.start);
        if (ret < 0) return ret;
        ret = ipv67_addr_parse(user_asn.end_str, &asn_claim.end);
        if (ret < 0) return ret;
        return ipv67_set_asn_claim(&asn_claim);

    case IPV67_CMD_REMOVE_ASN:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_asn_user_t))) return -EFAULT;
        memcpy(&user_asn, (const void *)req.arg1, sizeof(user_asn));
        user_asn.start_str[IPV67_ADDR_STR_MAX - 1] = '\0';
        user_asn.end_str[IPV67_ADDR_STR_MAX - 1] = '\0';
        ret = ipv67_addr_parse(user_asn.start_str, &addr);
        if (ret < 0) return ret;
        ret = ipv67_addr_parse(user_asn.end_str, &asn_claim.end);
        if (ret < 0) return ret;
        if (user_asn.asn == 0) {
            local_asn = ipv67_get_local_asn();
            if (local_asn <= 0) return -EINVAL;
            user_asn.asn = (uint32_t)local_asn;
        }
        return ipv67_remove_asn_claim(user_asn.asn, &addr, &asn_claim.end);

    case IPV67_CMD_SEND:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        if (req.arg3 > 0 && !user_range_mapped_ipv67(req.arg2, req.arg3)) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_send(&addr, (const uint8_t *)req.arg2, (uint64_t)req.arg3);

    case IPV67_CMD_PING:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_ping(&addr, (uint32_t)req.arg2);

    case IPV67_CMD_PUNCH:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_punch(&addr);

    case IPV67_CMD_PROBE_PEERS:
        return ipv67_probe_peers();

    case IPV67_CMD_SET_AUTH_KEY:
        if (!user_range_mapped_ipv67(req.arg1, IPV67_AUTH_KEY_SIZE)) return -EFAULT;
        return ipv67_set_auth_key((const uint8_t *)req.arg1, req.arg2);

    case IPV67_CMD_SET_IDENTITY_KEY:
        if (!user_range_mapped_ipv67(req.arg1, IPV67_IDENTITY_SIZE)) return -EFAULT;
        return ipv67_set_identity_key((const uint8_t *)req.arg1, req.arg2);

    case IPV67_CMD_SET_AUTH_REQUIRED:
        return ipv67_set_auth_required(req.arg1 ? 1 : 0);

    case IPV67_CMD_GET_AUTH_REQUIRED:
        return ipv67_get_auth_required();

    case IPV67_CMD_SET_PORT:
        return ipv67_set_port((uint16_t)req.arg1);

    case IPV67_CMD_GET_PORT:
        return (int)ipv67_get_port();

    case IPV67_CMD_ADDR_PARSE:
        if (!user_range_mapped_ipv67(req.arg1, 1)) return -EFAULT;
        if (!user_range_mapped_ipv67(req.arg2, sizeof(ipv67_addr_t))) return -EFAULT;
        src = (const char *)req.arg1;
        for (j = 0; j < IPV67_ADDR_STR_MAX; j++) {
            if (!user_range_mapped_ipv67(req.arg1 + j, 1)) break;
            addr_str[j] = src[j];
            if (src[j] == '\0') break;
        }
        addr_str[IPV67_ADDR_STR_MAX - 1] = '\0';
        ret = ipv67_addr_parse(addr_str, &addr);
        if (ret == IPV67_ERR_OK) {
            memcpy((void *)req.arg2, &addr, sizeof(ipv67_addr_t));
        }
        return ret;

    case IPV67_CMD_ADDR_FORMAT:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        if (!user_range_mapped_ipv67(req.arg2, IPV67_ADDR_STR_MAX)) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        ret = ipv67_addr_format(&addr, addr_str, sizeof(addr_str));
        if (ret == IPV67_ERR_OK) {
            memcpy((void *)req.arg2, addr_str, strlen(addr_str) + 1);
        }
        return ret;

    default:
        return -EINVAL;
    }
}

int ipv67_syscall_entry(const char *req_ptr, int unused1, int unused2) {
    int ret;

    ipv67_stack_lock();
    ret = sys_ipv67_impl(req_ptr, unused1, unused2);
    ipv67_stack_unlock();
    return ret;
}

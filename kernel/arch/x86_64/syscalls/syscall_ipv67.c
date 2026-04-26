#include "syscall_defs.h"
#include <lebirun/drivers/net/ipv67.h>
#include <lebirun/drivers/net/dns.h>
#include <string.h>

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

extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);

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
    char     addr_str[IPV67_ADDR_STR_MAX];
} __attribute__((packed)) ipv67_peer_user_t;

typedef struct {
    uint32_t next_hop_ipv4;
    ipv6_addr_t next_hop_ipv6;
    uint16_t next_hop_port;
    uint8_t next_hop_family;
    uint8_t hops;
    char dest_str[IPV67_ADDR_STR_MAX];
} __attribute__((packed)) ipv67_route_user_t;

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

static int sys_ipv67(const char *req_ptr, int unused1, int unused2) {
    ipv67_syscall_req_t req;
    ipv67_addr_t addr;
    ipv6_addr_t addr6;
    ipv67_peer_t *kernel_peers;
    ipv67_route_t *kernel_routes;
    ipv67_peer_user_t user_peer;
    ipv67_route_user_t user_route;
    char addr_str[IPV67_ADDR_STR_MAX];
    char host_str[256];
    const char *src;
    ipv4_addr_t resolved4;
    ipv6_addr_t resolved6;
    uint64_t req_addr;
    int count;
    int max;
    int i;
    int ret;
    uint64_t j;

    (void)unused1;
    (void)unused2;
    kernel_peers = NULL;
    kernel_routes = NULL;

    req_addr = (uint64_t)(uintptr_t)req_ptr;
    if (!user_range_mapped_ipv67(req_addr, sizeof(ipv67_syscall_req_t))) return -EFAULT;
    memcpy(&req, (const void *)req_addr, sizeof(ipv67_syscall_req_t));
    if (req.cmd != IPV67_CMD_ADDR_PARSE && req.cmd != IPV67_CMD_ADDR_FORMAT && req.cmd != IPV67_CMD_SET_PORT) {
        ret = ipv67_use_port((uint16_t)req.arg4);
        if (ret < 0) return ret;
    }

    switch (req.cmd) {
    case IPV67_CMD_INIT:
        ipv67_init();
        return 0;

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
        if (max > IPV67_MAX_PEERS) max = IPV67_MAX_PEERS;
        if (!user_range_mapped_ipv67(req.arg1, (uint64_t)max * sizeof(ipv67_peer_user_t))) return -EFAULT;
        kernel_peers = (ipv67_peer_t *)kmalloc((uint64_t)max * sizeof(ipv67_peer_t));
        if (!kernel_peers) return -ENOMEM;
        count = ipv67_get_peers(kernel_peers, max);
        for (i = 0; i < count; i++) {
            memset(&user_peer, 0, sizeof(user_peer));
            user_peer.ipv4 = kernel_peers[i].ipv4;
            memcpy(&user_peer.ipv6, &kernel_peers[i].ipv6, sizeof(ipv6_addr_t));
            user_peer.port = kernel_peers[i].port;
            user_peer.family = kernel_peers[i].family;
            ipv67_addr_format(&kernel_peers[i].addr, user_peer.addr_str, IPV67_ADDR_STR_MAX);
            memcpy((void *)(req.arg1 + (uint64_t)i * sizeof(ipv67_peer_user_t)), &user_peer, sizeof(ipv67_peer_user_t));
        }
        kfree(kernel_peers);
        return count;

    case IPV67_CMD_PEER_COUNT:
        return ipv67_peer_count();

    case IPV67_CMD_ROUTE_COUNT:
        return ipv67_route_count_get();

    case IPV67_CMD_GET_ROUTES:
        if (req.arg2 == 0) return 0;
        max = (int)req.arg2;
        if ((uint64_t)max != req.arg2 || max < 0) return -EINVAL;
        if (max > IPV67_MAX_ROUTES) max = IPV67_MAX_ROUTES;
        if (!user_range_mapped_ipv67(req.arg1, (uint64_t)max * sizeof(ipv67_route_user_t))) return -EFAULT;
        kernel_routes = (ipv67_route_t *)kmalloc((uint64_t)max * sizeof(ipv67_route_t));
        if (!kernel_routes) return -ENOMEM;
        count = ipv67_get_routes(kernel_routes, max);
        for (i = 0; i < count; i++) {
            memset(&user_route, 0, sizeof(user_route));
            user_route.next_hop_ipv4 = kernel_routes[i].next_hop_ipv4;
            memcpy(&user_route.next_hop_ipv6, &kernel_routes[i].next_hop_ipv6, sizeof(ipv6_addr_t));
            user_route.next_hop_port = kernel_routes[i].next_hop_port;
            user_route.next_hop_family = kernel_routes[i].next_hop_family;
            user_route.hops = kernel_routes[i].hops;
            ipv67_addr_format(&kernel_routes[i].dest, user_route.dest_str, IPV67_ADDR_STR_MAX);
            memcpy((void *)(req.arg1 + (uint64_t)i * sizeof(ipv67_route_user_t)), &user_route, sizeof(ipv67_route_user_t));
        }
        kfree(kernel_routes);
        return count;

    case IPV67_CMD_SEND:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        if (req.arg3 > 0 && !user_range_mapped_ipv67(req.arg2, req.arg3)) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_send(&addr, (const uint8_t *)req.arg2, (uint64_t)req.arg3);

    case IPV67_CMD_PING:
        if (!user_range_mapped_ipv67(req.arg1, sizeof(ipv67_addr_t))) return -EFAULT;
        memcpy(&addr, (const void *)req.arg1, sizeof(ipv67_addr_t));
        return ipv67_ping(&addr, (uint32_t)req.arg2);

    case IPV67_CMD_PROBE_PEERS:
        return ipv67_probe_peers();

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

void syscalls_ipv67_init(void) {
    syscall_table[SYSCALL_IPV67] = sys_ipv67;
}

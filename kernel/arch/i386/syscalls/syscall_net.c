#include "syscall_defs.h"

typedef struct {
    char name[16];
    uint8_t mac[6];
    uint8_t _pad1[2];
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint32_t mtu;
    uint8_t link_up;
    uint8_t dhcp_configured;
    uint8_t _pad2[2];
} __attribute__((packed)) netinfo_user_t;

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} __attribute__((packed)) arp_user_entry_t;

typedef struct {
    const char *url;
    uint8_t *buffer;
    uint32_t buffer_size;
    uint32_t *out_size;
    int *status_code;
} __attribute__((packed)) http_request_user_t;

extern int arp_get_cache(uint32_t *ips, uint8_t *macs, int max_entries);
extern int ping_one(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms);

#include <stdarg.h>

static void klog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    console_write_to(0, buf, (size_t)len);
    printf("%s", buf);
}

static int sys_net_ifconfig(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    netif_t *netif = netif_get_default();
    if (!netif) {
        klog("No network interface found\n");
        return -1;
    }
    netif_print_info(netif);
    klog("ifconfig: info printed for %s\n", netif->name);
    return 0;
}

static int sys_net_ping(int ip_packed, const char *unused2, int count) {
    (void)unused2;
    klog("[DEBUG] sys_net_ping called with ip=0x%08X count=%d\n", ip_packed, count);
    ipv4_addr_t target = u32_to_ipv4((uint32_t)ip_packed);
    if (count <= 0) count = 4;
    if (count > 100) count = 100;
    return ping(target, count, 3000);
}

static int sys_net_arp(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    arp_print_cache();
    klog("arp: cache printed\n");
    return 0;
}

static int sys_net_dns(int unused, const char *hostname, int result_ptr) {
    (void)unused;
    klog("[DEBUG] sys_net_dns called with hostname=%s\n", hostname ? hostname : "(null)");
    if (!hostname) return -1;
    ipv4_addr_t resolved;
    int ret = dns_resolve(hostname, &resolved);
    if (ret == 0) {
        klog("DNS: %s -> %u.%u.%u.%u\n", hostname,
               resolved.octets[0], resolved.octets[1],
               resolved.octets[2], resolved.octets[3]);
        if (result_ptr) {
            *(uint32_t *)result_ptr = ipv4_to_u32(resolved);
        }
    } else {
        klog("DNS: Failed to resolve %s\n", hostname);
    }
    return ret;
}

static int sys_net_dhcp(int cmd, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    netif_t *netif = netif_get_default();
    if (!netif) {
        klog("No network interface found\n");
        return -1;
    }
    if (cmd == 0) {
        if (dhcp_is_bound(netif)) {
            klog("DHCP: Bound\n");
            return 1;
        } else {
            klog("DHCP: Not bound\n");
            return 0;
        }
    } else if (cmd == 1) {
        klog("DHCP: Starting...\n");
        dhcp_start(netif);
        return 0;
    }
    return -1;
}

static int sys_net_getinfo(int buf_ptr, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    if (!buf_ptr) return -1;
    
    netif_t *netif = netif_get_default();
    if (!netif) return -1;
    
    netinfo_user_t *info = (netinfo_user_t *)buf_ptr;
    
    for (int i = 0; i < 15 && netif->name[i]; i++) {
        info->name[i] = netif->name[i];
    }
    info->name[15] = '\0';
    
    for (int i = 0; i < 6; i++) {
        info->mac[i] = netif->mac.addr[i];
    }
    
    info->ipv4 = ipv4_to_u32(netif->ipv4);
    info->netmask = ipv4_to_u32(netif->netmask);
    info->gateway = ipv4_to_u32(netif->gateway);
    info->dns = ipv4_to_u32(netif->dns_server);
    info->mtu = netif->mtu;
    info->link_up = netif->link_up;
    info->dhcp_configured = netif->dhcp_configured;
    
    klog("netinfo: %s ip=%u.%u.%u.%u\n", netif->name, netif->ipv4.octets[0], netif->ipv4.octets[1], netif->ipv4.octets[2], netif->ipv4.octets[3]);

    return 0;
}

static int sys_net_arp_get(int buf_ptr, const char *count_ptr, int max_entries) {
    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    
    uint32_t ips[16];
    uint8_t macs[16 * 6];
    
    if (max_entries > 16) max_entries = 16;
    
    int count = arp_get_cache(ips, macs, max_entries);
    
    arp_user_entry_t *entries = (arp_user_entry_t *)buf_ptr;
    for (int i = 0; i < count; i++) {
        entries[i].ip = ips[i];
        for (int j = 0; j < 6; j++) {
            entries[i].mac[j] = macs[i * 6 + j];
        }
    }
    
    *(int *)count_ptr = count;
    
    return 0;
}

static int sys_net_ping_one(int ip_packed, const char *seq_ptr, int timeout_ms) {
    ipv4_addr_t target = u32_to_ipv4((uint32_t)ip_packed);
    uint16_t seq = (uint16_t)(int)(size_t)seq_ptr;
    if (timeout_ms <= 0) timeout_ms = 3000;
    return ping_one(target, seq, (uint32_t)timeout_ms);
}

static int sys_net_dns_resolve(int hostname_ptr, const char *result_ptr, int unused) {
    (void)unused;
    const char *hostname = (const char *)hostname_ptr;
    if (!hostname || !result_ptr) return -1;
    
    ipv4_addr_t resolved;
    int ret = dns_resolve(hostname, &resolved);
    if (ret == 0) {
        *(uint32_t *)result_ptr = ipv4_to_u32(resolved);
    }
    return ret;
}

static int sys_net_http_get(int req_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;
    
    http_request_user_t *req = (http_request_user_t *)req_ptr;
    if (!req->url || !req->buffer || req->buffer_size == 0) return -1;
    
    uint32_t url_addr = (uint32_t)req->url;
    uint32_t buf_addr = (uint32_t)req->buffer;
    if (url_addr >= 0xC0000000 || url_addr < 0x1000) return -1;
    if (buf_addr >= 0xC0000000 || buf_addr < 0x1000) return -1;
    
    uint32_t downloaded = 0;
    int ret = http_download(req->url, req->buffer, req->buffer_size, &downloaded);
    
    if (req->out_size) {
        uint32_t out_addr = (uint32_t)req->out_size;
        if (out_addr < 0xC0000000 && out_addr >= 0x1000) {
            *(req->out_size) = downloaded;
        }
    }
    
    if (req->status_code) {
        uint32_t status_addr = (uint32_t)req->status_code;
        if (status_addr < 0xC0000000 && status_addr >= 0x1000) {
            *(req->status_code) = (ret == 0) ? 200 : -1;
        }
    }
    
    return ret;
}

void syscalls_net_init(void) {
    syscall_table[SYSCALL_NET_IFCONFIG] = sys_net_ifconfig;
    syscall_table[SYSCALL_NET_PING] = sys_net_ping;
    syscall_table[SYSCALL_NET_ARP] = sys_net_arp;
    syscall_table[SYSCALL_NET_DNS] = sys_net_dns;
    syscall_table[SYSCALL_NET_DHCP] = sys_net_dhcp;
    syscall_table[SYSCALL_NET_GETINFO] = sys_net_getinfo;
    syscall_table[SYSCALL_NET_ARP_GET] = sys_net_arp_get;
    syscall_table[SYSCALL_NET_PING_ONE] = sys_net_ping_one;
    syscall_table[SYSCALL_NET_DNS_RESOLVE] = sys_net_dns_resolve;
    syscall_table[SYSCALL_NET_HTTP_GET] = sys_net_http_get;
}

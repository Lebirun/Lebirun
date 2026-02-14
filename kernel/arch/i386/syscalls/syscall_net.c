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
    int max_redirects;
    uint8_t *headers_buf;
    uint32_t headers_buf_size;
    uint32_t *out_headers_len;
} __attribute__((packed)) http_request_user_t;

typedef struct {
    const char *url;
    const char *content_type;
    const uint8_t *post_body;
    uint32_t post_body_len;
    uint8_t *buffer;
    uint32_t buffer_size;
    uint32_t *out_size;
    int *status_code;
} __attribute__((packed)) http_post_request_user_t;

extern int arp_get_cache(uint32_t *ips, uint8_t *macs, int max_entries);
extern int ping_one(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms);

#include <stdarg.h>

static uint32_t get_user_pd(void) {
    if (!current_task) return 0;
    if (current_task->cr3) return current_task->cr3;
    return current_task->pd_phys;
}

static int user_range_ok(uint32_t addr, uint32_t size) {
    if (size == 0) return 0;
    if (addr < 0x1000 || addr >= 0xC0000000) return 0;
    uint32_t end = addr + size - 1;
    if (end < addr) return 0;
    if (end >= 0xC0000000) return 0;
    return 1;
}

static int user_range_mapped(uint32_t addr, uint32_t size) {
    if (!user_range_ok(addr, size)) return 0;
    uint32_t pd = get_user_pd();
    if (!pd) return 0;
    uint32_t start = addr & ~0xFFFu;
    uint32_t end = (addr + size - 1) & ~0xFFFu;
    for (uint32_t p = start;; p += 0x1000) {
        if (vmm_get_phys_in_pd(pd, p) == 0) return 0;
        if (p == end) break;
        if (p > end) return 0;
    }
    return 1;
}

static int copy_from_user(void *dst, uint32_t src_user, uint32_t size) {
    if (!dst || size == 0) return -1;
    if (!user_range_mapped(src_user, size)) return -1;
    memcpy(dst, (const void *)src_user, size);
    return 0;
}

static int copy_to_user(uint32_t dst_user, const void *src, uint32_t size) {
    if (!src || size == 0) return -1;
    if (!user_range_mapped(dst_user, size)) return -1;
    memcpy((void *)dst_user, src, size);
    return 0;
}

static int copy_user_string(char *dst, uint32_t dst_size, const char *src_user) {
    if (!dst || dst_size == 0) return -1;
    dst[0] = '\0';
    if (!src_user) return -1;
    uint32_t addr = (uint32_t)(uintptr_t)src_user;
    if (addr < 0x1000 || addr >= 0xC0000000) return -1;
    uint32_t pd = get_user_pd();
    if (!pd) return -1;

    uint32_t i = 0;
    while (i + 1 < dst_size) {
        uint32_t cur = addr + i;
        if (cur >= 0xC0000000 || cur < addr) return -1;
        if (vmm_get_phys_in_pd(pd, cur & ~0xFFFu) == 0) return -1;
        char c = *(const char *)cur;
        dst[i++] = c;
        if (c == '\0') return 0;
    }
    dst[dst_size - 1] = '\0';
    return -1;
}

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
    char hostbuf[256];
    if (copy_user_string(hostbuf, sizeof(hostbuf), hostname) != 0) return -1;
    ipv4_addr_t resolved;
    int ret = dns_resolve(hostbuf, &resolved);
    if (ret == 0) {
        klog("DNS: %s -> %u.%u.%u.%u\n", hostbuf,
               resolved.octets[0], resolved.octets[1],
               resolved.octets[2], resolved.octets[3]);
        if (result_ptr) {
            uint32_t out = ipv4_to_u32(resolved);
            if (copy_to_user((uint32_t)result_ptr, &out, sizeof(out)) != 0) return -1;
        }
    } else {
        klog("DNS: Failed to resolve %s\n", hostbuf);
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
    
    netinfo_user_t info;
    memset(&info, 0, sizeof(info));

    for (int i = 0; i < 15 && netif->name[i]; i++) {
        info.name[i] = netif->name[i];
    }
    info.name[15] = '\0';

    for (int i = 0; i < 6; i++) {
        info.mac[i] = netif->mac.addr[i];
    }

    info.ipv4 = ipv4_to_u32(netif->ipv4);
    info.netmask = ipv4_to_u32(netif->netmask);
    info.gateway = ipv4_to_u32(netif->gateway);
    info.dns = ipv4_to_u32(netif->dns_server);
    info.mtu = netif->mtu;
    info.link_up = netif->link_up;
    info.dhcp_configured = netif->dhcp_configured;

    if (copy_to_user((uint32_t)buf_ptr, &info, sizeof(info)) != 0) return -1;
    
    klog("netinfo: %s ip=%u.%u.%u.%u\n", netif->name, netif->ipv4.octets[0], netif->ipv4.octets[1], netif->ipv4.octets[2], netif->ipv4.octets[3]);

    return 0;
}

static int sys_net_arp_get(int buf_ptr, const char *count_ptr, int max_entries) {
    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    
    uint32_t ips[16];
    uint8_t macs[16 * 6];
    
    if (max_entries > 16) max_entries = 16;
    
    int count = arp_get_cache(ips, macs, max_entries);
    
    if (count > 0) {
        uint32_t need = (uint32_t)count * (uint32_t)sizeof(arp_user_entry_t);
        if (!user_range_mapped((uint32_t)buf_ptr, need)) return -1;
    }
    if (!user_range_mapped((uint32_t)(uintptr_t)count_ptr, sizeof(int))) return -1;

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
    char hostbuf[256];
    if (copy_user_string(hostbuf, sizeof(hostbuf), hostname) != 0) return -1;
    if (!user_range_mapped((uint32_t)(uintptr_t)result_ptr, sizeof(uint32_t))) return -1;
    
    ipv4_addr_t resolved;
    int ret = dns_resolve(hostbuf, &resolved);
    if (ret == 0) {
        *(uint32_t *)result_ptr = ipv4_to_u32(resolved);
    }
    return ret;
}

static int sys_net_http_get(int req_ptr, const char *unused1, int unused2) {
    http_request_user_t req;
    char *url_buf;
    uint32_t user_buf_addr;
    uint32_t max_dl;
    uint8_t *kbuf;
    uint8_t *khdr;
    uint32_t downloaded;
    int status_code;
    int ret;
    int max_redir;
    uint32_t hdr_len;
    uint32_t hdr_buf_sz;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;

    if (copy_from_user(&req, (uint32_t)req_ptr, sizeof(req)) != 0) return -1;
    if (!req.url || !req.buffer || req.buffer_size == 0) return -1;

    url_buf = (char *)kmalloc(512);
    if (!url_buf) return -1;

    if (copy_user_string(url_buf, 512, req.url) != 0) { kfree(url_buf); return -1; }

    user_buf_addr = (uint32_t)(uintptr_t)req.buffer;
    if (!user_range_ok(user_buf_addr, req.buffer_size)) { kfree(url_buf); return -1; }

    max_dl = req.buffer_size;
    if (max_dl > 1024 * 1024) max_dl = 1024 * 1024;

    kbuf = (uint8_t *)kmalloc(max_dl);
    if (!kbuf) { kfree(url_buf); return -1; }

    max_redir = req.max_redirects;
    if (max_redir < 0) max_redir = 0;
    if (max_redir > 20) max_redir = 20;

    khdr = NULL;
    hdr_buf_sz = 0;
    if (req.headers_buf && req.headers_buf_size > 0) {
        hdr_buf_sz = req.headers_buf_size;
        if (hdr_buf_sz > 8192) hdr_buf_sz = 8192;
        if (!user_range_ok((uint32_t)(uintptr_t)req.headers_buf, hdr_buf_sz)) {
            kfree(kbuf); kfree(url_buf);
            return -1;
        }
        khdr = (uint8_t *)kmalloc(hdr_buf_sz);
        if (!khdr) {
            kfree(kbuf); kfree(url_buf);
            return -1;
        }
    }

    downloaded = 0;
    status_code = 0;
    hdr_len = 0;
    ret = http_download_ex(url_buf, kbuf, max_dl, &downloaded, &status_code,
                           max_redir, khdr, hdr_buf_sz, &hdr_len);

    if (ret == 0 && downloaded > 0) {
        if (!user_range_mapped(user_buf_addr, downloaded)) {
            kfree(kbuf);
            if (khdr) kfree(khdr);
            kfree(url_buf);
            return -1;
        }
        memcpy((void *)user_buf_addr, kbuf, downloaded);
    }
    kfree(kbuf);
    kfree(url_buf);

    if (khdr && hdr_len > 0) {
        memcpy((void *)(uintptr_t)req.headers_buf, khdr, hdr_len);
        kfree(khdr);
    } else if (khdr) {
        kfree(khdr);
    }

    if (req.out_size) {
        uint32_t out_addr = (uint32_t)(uintptr_t)req.out_size;
        if (user_range_mapped(out_addr, sizeof(uint32_t))) {
            *(req.out_size) = downloaded;
        }
    }

    if (req.status_code) {
        uint32_t status_addr = (uint32_t)(uintptr_t)req.status_code;
        if (user_range_mapped(status_addr, sizeof(int))) {
            *(req.status_code) = status_code;
        }
    }

    if (req.out_headers_len) {
        uint32_t hlen_addr = (uint32_t)(uintptr_t)req.out_headers_len;
        if (user_range_mapped(hlen_addr, sizeof(uint32_t))) {
            *(req.out_headers_len) = hdr_len;
        }
    }

    return ret;
}

static int sys_net_http_post(int req_ptr, const char *unused1, int unused2) {
    http_post_request_user_t req;
    char *url_buf;
    char *ct_buf;
    uint32_t user_buf_addr;
    uint32_t max_dl;
    uint8_t *kbuf;
    uint8_t *kbody;
    uint32_t downloaded;
    int status;
    int ret;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;

    if (copy_from_user(&req, (uint32_t)req_ptr, sizeof(req)) != 0) return -1;
    if (!req.url || !req.buffer || req.buffer_size == 0) return -1;

    url_buf = (char *)kmalloc(512);
    if (!url_buf) return -1;
    ct_buf = (char *)kmalloc(128);
    if (!ct_buf) { kfree(url_buf); return -1; }

    if (copy_user_string(url_buf, 512, req.url) != 0) { kfree(url_buf); kfree(ct_buf); return -1; }

    ct_buf[0] = '\0';
    if (req.content_type) {
        if (copy_user_string(ct_buf, 128, req.content_type) != 0) { kfree(url_buf); kfree(ct_buf); return -1; }
    }

    user_buf_addr = (uint32_t)(uintptr_t)req.buffer;
    if (!user_range_ok(user_buf_addr, req.buffer_size)) { kfree(url_buf); kfree(ct_buf); return -1; }

    max_dl = req.buffer_size;
    if (max_dl > 1024 * 1024) max_dl = 1024 * 1024;

    kbody = NULL;
    if (req.post_body && req.post_body_len > 0) {
        if (req.post_body_len > 65536) { kfree(url_buf); kfree(ct_buf); return -1; }
        if (!user_range_mapped((uint32_t)(uintptr_t)req.post_body, req.post_body_len)) { kfree(url_buf); kfree(ct_buf); return -1; }
        kbody = (uint8_t *)kmalloc(req.post_body_len);
        if (!kbody) { kfree(url_buf); kfree(ct_buf); return -1; }
        memcpy(kbody, (const void *)(uintptr_t)req.post_body, req.post_body_len);
    }

    kbuf = (uint8_t *)kmalloc(max_dl);
    if (!kbuf) {
        if (kbody) kfree(kbody);
        kfree(url_buf); kfree(ct_buf);
        return -1;
    }

    downloaded = 0;
    status = 0;
    ret = http_post_download(url_buf,
                             ct_buf[0] ? ct_buf : NULL,
                             kbody, kbody ? req.post_body_len : 0,
                             kbuf, max_dl,
                             &downloaded, &status);

    if (ret == 0 && downloaded > 0) {
        if (!user_range_mapped(user_buf_addr, downloaded)) {
            kfree(kbuf);
            if (kbody) kfree(kbody);
            kfree(url_buf); kfree(ct_buf);
            return -1;
        }
        memcpy((void *)user_buf_addr, kbuf, downloaded);
    }
    kfree(kbuf);
    if (kbody) kfree(kbody);
    kfree(url_buf); kfree(ct_buf);

    if (req.out_size) {
        uint32_t out_addr = (uint32_t)(uintptr_t)req.out_size;
        if (user_range_mapped(out_addr, sizeof(uint32_t))) {
            *(req.out_size) = downloaded;
        }
    }

    if (req.status_code) {
        uint32_t status_addr = (uint32_t)(uintptr_t)req.status_code;
        if (user_range_mapped(status_addr, sizeof(int))) {
            *(req.status_code) = status;
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
    syscall_table[SYSCALL_NET_HTTP_POST] = sys_net_http_post;
}

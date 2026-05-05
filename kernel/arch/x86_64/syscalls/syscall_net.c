#include "syscall_defs.h"

typedef struct {
    char name[16];
    uint8_t mac[6];
    uint8_t _pad1[2];
    uint64_t ipv4;
    uint64_t netmask;
    uint64_t gateway;
    uint64_t dns;
    uint64_t mtu;
    uint8_t link_up;
    uint8_t dhcp_configured;
    uint8_t _pad2[2];
} __attribute__((packed)) netinfo_user_t;

typedef struct {
    uint64_t ip;
    uint8_t mac[6];
} __attribute__((packed)) arp_user_entry_t;

typedef struct {
    const char *url;
    uint8_t *buffer;
    uint64_t buffer_size;
    uint64_t *out_size;
    int *status_code;
    int max_redirects;
    uint8_t *headers_buf;
    uint64_t headers_buf_size;
    uint64_t *out_headers_len;
} __attribute__((packed)) http_request_user_t;

typedef struct {
    const char *url;
    const char *content_type;
    const uint8_t *post_body;
    uint64_t post_body_len;
    uint8_t *buffer;
    uint64_t buffer_size;
    uint64_t *out_size;
    int *status_code;
} __attribute__((packed)) http_post_request_user_t;

typedef struct {
    const char *url;
    uint64_t *out_buffer;
    uint64_t *out_size;
    int *status_code;
    int max_redirects;
    uint8_t *headers_buf;
    uint64_t headers_buf_size;
    uint64_t *out_headers_len;
} __attribute__((packed)) http_get_alloc_req_t;

extern int arp_get_cache(uint64_t *ips, uint8_t *macs, int max_entries);
extern int ping_one(ipv4_addr_t target, uint16_t seq, uint64_t timeout_ms);

#include <stdarg.h>

#define HTTP_ALLOC_MMAP_BASE  0x10000000u
#define HTTP_ALLOC_MMAP_LIMIT 0x40000000u

static uint64_t net_align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

static uint64_t get_user_pd(void) {
    if (!current_task) return 0;
    if (current_task->cr3) return current_task->cr3;
    return current_task->pml4_phys;
}

static int user_range_ok(uint64_t addr, uint64_t size) {
    uint64_t end;
    if (size == 0) return 0;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return 0;
    end = addr + size - 1;
    if (end < addr) return 0;
    if (end >= KERNEL_VMA) return 0;
    return 1;
}

static int user_range_mapped(uint64_t addr, uint64_t size) {
    uint64_t pd;
    uint64_t start;
    uint64_t end;
    if (!user_range_ok(addr, size)) return 0;
    pd = get_user_pd();
    if (!pd) return 0;
    start = addr & ~0xFFFu;
    end = (addr + size - 1) & ~0xFFFu;
    for (uint64_t p = start;; p += 0x1000) {
        if (vmm_get_phys_in_pml4(pd, p) == 0) return 0;
        if (p == end) break;
        if (p > end) return 0;
    }
    return 1;
}

static int copy_from_user(void *dst, uint64_t src_user, uint64_t size) {
    if (!dst || size == 0) return -1;
    if (!user_range_mapped(src_user, size)) return -1;
    memcpy(dst, (const void *)src_user, size);
    return 0;
}

static int copy_to_user(uint64_t dst_user, const void *src, uint64_t size) {
    if (!src || size == 0) return -1;
    if (!user_range_mapped(dst_user, size)) return -1;
    memcpy((void *)dst_user, src, size);
    return 0;
}

static int copy_user_string(char *dst, uint64_t dst_size, const char *src_user) {
    uint64_t addr;
    uint64_t pd;
    uint64_t i;
    if (!dst || dst_size == 0) return -1;
    dst[0] = '\0';
    if (!src_user) return -1;
    addr = (uint64_t)(uintptr_t)src_user;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return -1;
    pd = get_user_pd();
    if (!pd) return -1;

    i = 0;
    while (i + 1 < dst_size) {
        uint64_t cur = addr + i;
        char c;
        if (cur >= KERNEL_VMA || cur < addr) return -1;
        if (vmm_get_phys_in_pml4(pd, cur & ~0xFFFu) == 0) return -1;
        c = *(const char *)cur;
        dst[i++] = c;
        if (c == '\0') return 0;
    }
    dst[dst_size - 1] = '\0';
    return -1;
}

static void klog(const char *fmt, ...) {
    char buf[256];
    int len;
    va_list ap;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    console_write_to(0, buf, (size_t)len);
}

static void klog_con(int con_id, const char *fmt, ...) {
    char buf[256];
    int len;
    va_list ap;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    console_write_to(con_id, buf, (size_t)len);
}

static int sys_net_ifconfig(int unused, const char *unused2, int unused3) {
    int con_id;
    netif_t *netif;
    ipv4_addr_t ip;
    ipv4_addr_t netmask;
    ipv4_addr_t gateway;

    con_id = current_task ? current_task->console_id : 0;
    net_ensure_hw();
    netif = netif_get_default();
    if (!netif) {
        klog_con(con_id, "No network interface found\n");
        return -1;
    }
    if (unused != 0) {
        ip = u32_to_ipv4((uint64_t)(uint32_t)unused);
        netmask = u32_to_ipv4((uint64_t)(uint32_t)(uintptr_t)unused2);
        gateway = u32_to_ipv4((uint64_t)(uint32_t)unused3);
        if (ipv4_eq(netif->ipv4, ip) && ipv4_eq(netif->netmask, netmask) && ipv4_eq(netif->gateway, gateway) && !netif->dhcp_configured) {
            klog_con(con_id, "ifconfig: set %s ip=%u.%u.%u.%u netmask=%u.%u.%u.%u gateway=%u.%u.%u.%u\n",
                     netif->name,
                     ip.octets[0], ip.octets[1], ip.octets[2], ip.octets[3],
                     netmask.octets[0], netmask.octets[1], netmask.octets[2], netmask.octets[3],
                     gateway.octets[0], gateway.octets[1], gateway.octets[2], gateway.octets[3]);
            return 0;
        }
        dhcp_stop(netif);
        netif_set_ipv4(netif, ip, netmask, gateway);
        netif->dhcp_configured = 0;
        klog_con(con_id, "ifconfig: set %s ip=%u.%u.%u.%u netmask=%u.%u.%u.%u gateway=%u.%u.%u.%u\n",
                 netif->name,
                 ip.octets[0], ip.octets[1], ip.octets[2], ip.octets[3],
                 netmask.octets[0], netmask.octets[1], netmask.octets[2], netmask.octets[3],
                 gateway.octets[0], gateway.octets[1], gateway.octets[2], gateway.octets[3]);
        return 0;
    }
    netif_print_info(netif);
    klog("ifconfig: info printed for %s\n", netif->name);
    return 0;
}

static int sys_net_ping(int ip_packed, const char *unused2, int count) {
    (void)unused2;
    net_ensure_hw();
    klog("[DEBUG] sys_net_ping called with ip=0x%08X count=%d\n", ip_packed, count);
    ipv4_addr_t target = u32_to_ipv4((uint64_t)ip_packed);
    if (count <= 0) count = 4;
    if (count > 100) count = 100;
    return ping(target, count, 3000);
}

static int sys_net_arp(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    net_ensure_hw();
    arp_print_cache();
    klog("arp: cache printed\n");
    return 0;
}

static int sys_net_dns(int unused, const char *hostname, int result_ptr) {
    char hostbuf[256];
    int ret;
    (void)unused;
    net_ensure_hw();
    if (copy_user_string(hostbuf, sizeof(hostbuf), hostname) != 0) return -1;
    ipv4_addr_t resolved;
    ret = dns_resolve(hostbuf, &resolved);
    if (ret == 0) {
        klog("DNS: %s -> %u.%u.%u.%u\n", hostbuf,
               resolved.octets[0], resolved.octets[1],
               resolved.octets[2], resolved.octets[3]);
        if (result_ptr) {
            uint64_t out = ipv4_to_u32(resolved);
            if (copy_to_user((uint64_t)result_ptr, &out, sizeof(out)) != 0) return -1;
        }
    } else {
        klog("DNS: Failed to resolve %s\n", hostbuf);
    }
    return ret;
}

static int sys_net_dhcp(int cmd, const char *unused2, int unused3) {
    int con_id;
    int i;
    netif_t *netif;

    (void)unused2; (void)unused3;
    con_id = current_task ? current_task->console_id : 0;
    net_ensure_hw();
    netif = netif_get_default();
    if (!netif) {
        klog_con(con_id, "No network interface found\n");
        return -1;
    }
    if (cmd == 0) {
        if (dhcp_is_bound(netif)) {
            klog_con(con_id, "DHCP: Bound\n");
            return 1;
        } else {
            klog_con(con_id, "DHCP: Not bound\n");
            return 0;
        }
    } else if (cmd == 1) {
        klog_con(con_id, "DHCP: Starting...\n");
        dhcp_start(netif);
        return 0;
    } else if (cmd == 2) {
        if (dhcp_is_bound(netif)) {
            klog_con(con_id, "DHCP: Already configured (%u.%u.%u.%u)\n",
                     netif->ipv4.octets[0], netif->ipv4.octets[1],
                     netif->ipv4.octets[2], netif->ipv4.octets[3]);
            return 0;
        }
        for (i = 0; i < 10; i++) {
            sleep_ms(10);
            netif_poll_all();
            if (netif->link_up) break;
            if (task_has_pending_signals()) return -EINTR;
        }
        if (!netif->link_up) {
            klog_con(con_id, "DHCP: No link detected\n");
            return -1;
        }
        klog_con(con_id, "DHCP: Link up, starting DHCP...\n");
        dhcp_start(netif);
        for (i = 0; i < 500; i++) {
            sleep_ms(10);
            netif_poll_all();
            if (dhcp_is_bound(netif)) break;
            if (task_has_pending_signals()) return -EINTR;
        }
        if (dhcp_is_bound(netif)) {
            klog_con(con_id, "DHCP: Configured:\n");
            klog_con(con_id, "  IP: %u.%u.%u.%u\n",
                     netif->ipv4.octets[0], netif->ipv4.octets[1],
                     netif->ipv4.octets[2], netif->ipv4.octets[3]);
            klog_con(con_id, "  Netmask: %u.%u.%u.%u\n",
                     netif->netmask.octets[0], netif->netmask.octets[1],
                     netif->netmask.octets[2], netif->netmask.octets[3]);
            klog_con(con_id, "  Gateway: %u.%u.%u.%u\n",
                     netif->gateway.octets[0], netif->gateway.octets[1],
                     netif->gateway.octets[2], netif->gateway.octets[3]);
            klog_con(con_id, "  DNS: %u.%u.%u.%u\n",
                     netif->dns_server.octets[0], netif->dns_server.octets[1],
                     netif->dns_server.octets[2], netif->dns_server.octets[3]);
            console_writer_flush();
            return 0;
        }
        klog_con(con_id, "DHCP: Timed out\n");
        console_writer_flush();
        return -1;
    }
    return -1;
}

static int sys_net_getinfo(int buf_ptr, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    if (!buf_ptr) return -1;
    net_ensure_hw();
    
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

    if (copy_to_user((uint64_t)buf_ptr, &info, sizeof(info)) != 0) return -1;
    
    klog("netinfo: %s ip=%u.%u.%u.%u\n", netif->name, netif->ipv4.octets[0], netif->ipv4.octets[1], netif->ipv4.octets[2], netif->ipv4.octets[3]);

    return 0;
}

static int sys_net_arp_get(int buf_ptr, const char *count_ptr, int max_entries) {
    uint64_t ips[16];
    uint8_t macs[16 * 6];
    int count;
    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    net_ensure_hw();
    
    
    if (max_entries > 16) max_entries = 16;
    
    count = arp_get_cache(ips, macs, max_entries);
    
    if (count > 0) {
        uint64_t need = (uint64_t)count * (uint64_t)sizeof(arp_user_entry_t);
        if (!user_range_mapped((uint64_t)buf_ptr, need)) return -1;
    }
    if (!user_range_mapped((uint64_t)(uintptr_t)count_ptr, sizeof(int))) return -1;

    arp_user_entry_t *entries = (arp_user_entry_t *)(uintptr_t)buf_ptr;
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
    uint16_t seq;
    net_ensure_hw();
    ipv4_addr_t target = u32_to_ipv4((uint64_t)ip_packed);
    seq = (uint16_t)(int)(size_t)seq_ptr;
    if (timeout_ms <= 0) timeout_ms = 3000;
    return ping_one(target, seq, (uint64_t)timeout_ms);
}

static int sys_net_dns_resolve(int hostname_ptr, const char *result_ptr, int unused) {
    const char *hostname;
    char hostbuf[256];
    int ret;
    (void)unused;
    hostname = (const char *)(uintptr_t)hostname_ptr;
    if (!hostname || !result_ptr) return -1;
    net_ensure_hw();
    if (copy_user_string(hostbuf, sizeof(hostbuf), hostname) != 0) return -1;
    if (!user_range_mapped((uint64_t)(uintptr_t)result_ptr, sizeof(uint64_t))) return -1;
    
    ipv4_addr_t resolved;
    ret = dns_resolve(hostbuf, &resolved);
    if (ret == 0) {
        *(uint64_t *)result_ptr = ipv4_to_u32(resolved);
    }
    return ret;
}

static int sys_net_http_get(int req_ptr, const char *unused1, int unused2) {
    char *url_buf;
    uint64_t user_buf_addr;
    uint8_t *kbuf;
    uint8_t *khdr;
    uint64_t downloaded;
    int status_code;
    int ret;
    int max_redir;
    uint64_t hdr_len;
    uint64_t hdr_buf_sz;
    uint64_t copy_len;
    http_request_user_t req;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;
    net_ensure_hw();

    if (copy_from_user(&req, (uint64_t)req_ptr, sizeof(req)) != 0) return -1;
    if (!req.url || !req.buffer || req.buffer_size == 0) return -1;

    url_buf = (char *)kmalloc(512);
    if (!url_buf) return -1;

    if (copy_user_string(url_buf, 512, req.url) != 0) { kfree(url_buf); return -1; }

    user_buf_addr = (uint64_t)(uintptr_t)req.buffer;
    if (!user_range_ok(user_buf_addr, req.buffer_size)) { kfree(url_buf); return -1; }

    max_redir = req.max_redirects;
    if (max_redir < 0) max_redir = 0;
    if (max_redir > 20) max_redir = 20;

    khdr = NULL;
    hdr_buf_sz = 0;
    if (req.headers_buf && req.headers_buf_size > 0) {
        hdr_buf_sz = req.headers_buf_size;
        if (hdr_buf_sz > 8192) hdr_buf_sz = 8192;
        if (!user_range_ok((uint64_t)(uintptr_t)req.headers_buf, hdr_buf_sz)) {
            kfree(url_buf);
            return -1;
        }
        khdr = (uint8_t *)kmalloc(hdr_buf_sz);
        if (!khdr) {
            kfree(url_buf);
            return -1;
        }
    }

    kbuf = NULL;
    downloaded = 0;
    status_code = 0;
    hdr_len = 0;
    ret = http_download_alloc(url_buf, &kbuf, &downloaded, &status_code,
                              max_redir, khdr, hdr_buf_sz, &hdr_len);

    if (ret == 0 && kbuf && downloaded > 0) {
        copy_len = downloaded < req.buffer_size ? downloaded : req.buffer_size;
        if (!user_range_mapped(user_buf_addr, copy_len)) {
            kfree(kbuf);
            if (khdr) kfree(khdr);
            kfree(url_buf);
            return -1;
        }
        memcpy((void *)user_buf_addr, kbuf, copy_len);
        downloaded = copy_len;
    }
    if (kbuf) kfree(kbuf);
    kfree(url_buf);

    if (khdr && hdr_len > 0) {
        if (user_range_mapped((uint64_t)(uintptr_t)req.headers_buf, hdr_len))
            memcpy((void *)(uintptr_t)req.headers_buf, khdr, hdr_len);
        kfree(khdr);
    } else if (khdr) {
        kfree(khdr);
    }

    if (req.out_size) {
        uint64_t out_addr = (uint64_t)(uintptr_t)req.out_size;
        if (user_range_mapped(out_addr, sizeof(uint64_t))) {
            *(req.out_size) = downloaded;
        }
    }

    if (req.status_code) {
        uint64_t status_addr = (uint64_t)(uintptr_t)req.status_code;
        if (user_range_mapped(status_addr, sizeof(int))) {
            *(req.status_code) = status_code;
        }
    }

    if (req.out_headers_len) {
        uint64_t hlen_addr = (uint64_t)(uintptr_t)req.out_headers_len;
        if (user_range_mapped(hlen_addr, sizeof(uint64_t))) {
            *(req.out_headers_len) = hdr_len;
        }
    }

    return ret;
}

static int sys_net_http_post(int req_ptr, const char *unused1, int unused2) {
    char *url_buf;
    char *ct_buf;
    uint64_t user_buf_addr;
    uint64_t max_dl;
    uint8_t *kbuf;
    uint8_t *kbody;
    uint64_t downloaded;
    int status;
    int ret;
    http_post_request_user_t req;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;
    net_ensure_hw();

    if (copy_from_user(&req, (uint64_t)req_ptr, sizeof(req)) != 0) return -1;
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

    user_buf_addr = (uint64_t)(uintptr_t)req.buffer;
    if (!user_range_ok(user_buf_addr, req.buffer_size)) { kfree(url_buf); kfree(ct_buf); return -1; }

    max_dl = req.buffer_size;
    if (max_dl > 1024 * 1024) max_dl = 1024 * 1024;

    kbody = NULL;
    if (req.post_body && req.post_body_len > 0) {
        if (req.post_body_len > 65536) { kfree(url_buf); kfree(ct_buf); return -1; }
        if (!user_range_mapped((uint64_t)(uintptr_t)req.post_body, req.post_body_len)) { kfree(url_buf); kfree(ct_buf); return -1; }
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
        uint64_t out_addr = (uint64_t)(uintptr_t)req.out_size;
        if (user_range_mapped(out_addr, sizeof(uint64_t))) {
            *(req.out_size) = downloaded;
        }
    }

    if (req.status_code) {
        uint64_t status_addr = (uint64_t)(uintptr_t)req.status_code;
        if (user_range_mapped(status_addr, sizeof(int))) {
            *(req.status_code) = status;
        }
    }

    return ret;
}

static int sys_net_http_get_alloc(int req_ptr, const char *unused1, int unused2) {
    char *url_buf;
    uint8_t *kbuf;
    uint8_t *khdr;
    uint64_t downloaded;
    int status_code;
    int ret;
    int max_redir;
    uint64_t hdr_len;
    uint64_t hdr_buf_sz;
    uint64_t alloc_size;
    uint64_t base;
    uint64_t page_count;
    uint64_t *new_pages;
    uint64_t old_count;
    uint64_t new_count;
    uint64_t *expanded;
    http_get_alloc_req_t req;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;
    if (!current_task) return -1;
    net_ensure_hw();

    if (copy_from_user(&req, (uint64_t)req_ptr, sizeof(req)) != 0) return -1;
    if (!req.url || !req.out_buffer || !req.out_size) return -1;

    url_buf = (char *)kmalloc(512);
    if (!url_buf) return -1;

    if (copy_user_string(url_buf, 512, req.url) != 0) { kfree(url_buf); return -1; }

    max_redir = req.max_redirects;
    if (max_redir < 0) max_redir = 0;
    if (max_redir > 20) max_redir = 20;

    khdr = NULL;
    hdr_buf_sz = 0;
    if (req.headers_buf && req.headers_buf_size > 0) {
        hdr_buf_sz = req.headers_buf_size;
        if (hdr_buf_sz > 8192) hdr_buf_sz = 8192;
        if (!user_range_ok((uint64_t)(uintptr_t)req.headers_buf, hdr_buf_sz)) {
            kfree(url_buf);
            return -1;
        }
        khdr = (uint8_t *)kmalloc(hdr_buf_sz);
        if (!khdr) {
            kfree(url_buf);
            return -1;
        }
    }

    kbuf = NULL;
    downloaded = 0;
    status_code = 0;
    hdr_len = 0;
    ret = http_download_alloc(url_buf, &kbuf, &downloaded, &status_code,
                              max_redir, khdr, hdr_buf_sz, &hdr_len);
    kfree(url_buf);

    if (khdr && hdr_len > 0) {
        if (user_range_mapped((uint64_t)(uintptr_t)req.headers_buf, hdr_len))
            memcpy((void *)(uintptr_t)req.headers_buf, khdr, hdr_len);
        kfree(khdr);
    } else if (khdr) {
        kfree(khdr);
    }

    if (req.status_code) {
        uint64_t sa = (uint64_t)(uintptr_t)req.status_code;
        if (user_range_mapped(sa, sizeof(int)))
            *(req.status_code) = status_code;
    }

    if (req.out_headers_len) {
        uint64_t ha = (uint64_t)(uintptr_t)req.out_headers_len;
        if (user_range_mapped(ha, sizeof(uint64_t)))
            *(req.out_headers_len) = hdr_len;
    }

    if (ret < 0 || !kbuf || downloaded == 0) {
        if (kbuf) kfree(kbuf);
        if (req.out_size) {
            uint64_t oa = (uint64_t)(uintptr_t)req.out_size;
            if (user_range_mapped(oa, sizeof(uint64_t)))
                *(req.out_size) = 0;
        }
        if (req.out_buffer) {
            uint64_t ba = (uint64_t)(uintptr_t)req.out_buffer;
            if (user_range_mapped(ba, sizeof(uint64_t)))
                *(req.out_buffer) = 0;
        }
        return ret;
    }

    alloc_size = net_align_up(downloaded, 0x1000u);
    if (alloc_size == 0) alloc_size = 0x1000u;

    if (current_task->mmap_next_addr < HTTP_ALLOC_MMAP_BASE ||
        current_task->mmap_next_addr >= HTTP_ALLOC_MMAP_LIMIT) {
        current_task->mmap_next_addr = HTTP_ALLOC_MMAP_BASE;
    }

    base = net_align_up(current_task->mmap_next_addr, 0x1000u);
    if (base < HTTP_ALLOC_MMAP_BASE) base = HTTP_ALLOC_MMAP_BASE;
    if (alloc_size > HTTP_ALLOC_MMAP_LIMIT - HTTP_ALLOC_MMAP_BASE ||
        base + alloc_size < base ||
        base + alloc_size >= HTTP_ALLOC_MMAP_LIMIT) {
        kfree(kbuf);
        return -1;
    }
    current_task->mmap_next_addr = base + alloc_size;

    if (base + alloc_size >= KERNEL_VMA) {
        kfree(kbuf);
        return -1;
    }

    page_count = 0;
    new_pages = vmm_map_range_in_pml4_tracked(
        current_task->pml4_phys, base, alloc_size, 0x7, &page_count);

    if (!new_pages) {
        kfree(kbuf);
        return -1;
    }

    if (page_count > 0) {
        old_count = current_task->user_pages_count;
        new_count = old_count + page_count;
        expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
        if (expanded) {
            if (current_task->user_pages && old_count > 0) {
                memcpy(expanded, current_task->user_pages, old_count * sizeof(uint64_t));
                kfree(current_task->user_pages);
            }
            memcpy(expanded + old_count, new_pages, page_count * sizeof(uint64_t));
            current_task->user_pages = expanded;
            current_task->user_pages_count = new_count;
        }
        kfree(new_pages);
    }

    memcpy((void *)base, kbuf, downloaded);
    kfree(kbuf);

    if (req.out_buffer) {
        uint64_t ba = (uint64_t)(uintptr_t)req.out_buffer;
        if (user_range_mapped(ba, sizeof(uint64_t)))
            *(req.out_buffer) = base;
    }

    if (req.out_size) {
        uint64_t oa = (uint64_t)(uintptr_t)req.out_size;
        if (user_range_mapped(oa, sizeof(uint64_t)))
            *(req.out_size) = downloaded;
    }

    return 0;
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
    syscall_table[SYSCALL_NET_HTTP_GET_ALLOC] = sys_net_http_get_alloc;
}

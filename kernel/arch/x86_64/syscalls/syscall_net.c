#include "syscall_defs.h"
#include <lebirun/pit.h>

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
    uint8_t ipv6[16];
    uint8_t ipv6_prefix;
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
extern void netif_set_dns(netif_t *netif, ipv4_addr_t dns1, ipv4_addr_t dns2);
extern int dns_server_count(void);
extern ipv4_addr_t dns_server_at(int index);
extern int dns_set_servers(const ipv4_addr_t *servers, int count);
extern int dns6_server_count(void);
extern ipv6_addr_t dns6_server_at(int index);
extern int dns6_set_servers(const ipv6_addr_t *servers, int count);

#include <stdarg.h>

static uint64_t net_align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

static int net_user_range_free(uint64_t base, uint64_t size) {
    uint64_t end;
    uint64_t page;
    uint64_t area_end;
    int i;

    if (!current_task || size == 0) return 0;
    end = base + size;
    if (end < base) return 0;
    if (!((base >= USER_DYNAMIC_BASE && end <= USER_DYNAMIC_LIMIT) ||
          (base >= USER_HIGH_DYNAMIC_BASE &&
           end <= USER_HIGH_DYNAMIC_LIMIT))) return 0;
    for (i = 0; i < current_task->file_map_count; i++) {
        area_end = current_task->file_maps[i].vaddr +
                   current_task->file_maps[i].memsz;
        if (area_end < current_task->file_maps[i].vaddr) return 0;
        if (base < area_end && end > current_task->file_maps[i].vaddr)
            return 0;
    }
    for (page = base; page < end; page += PAGE_SIZE) {
        if (vmm_get_phys_in_pml4(current_task->pml4_phys, page) != 0)
            return 0;
    }
    return 1;
}

static uint64_t net_find_user_mapping(uint64_t cursor, uint64_t size) {
    uint64_t base;

    if (size == 0 || size > USER_HIGH_DYNAMIC_LIMIT - USER_HIGH_DYNAMIC_BASE)
        return 0;
    if (cursor >= USER_HIGH_DYNAMIC_BASE && cursor <= USER_HIGH_DYNAMIC_LIMIT) {
        cursor &= ~(PAGE_SIZE - 1u);
        while (cursor >= USER_HIGH_DYNAMIC_BASE + size) {
            base = cursor - size;
            if (net_user_range_free(base, size)) return base;
            cursor -= PAGE_SIZE;
        }
        return 0;
    }
    if (cursor <= USER_DYNAMIC_BASE || cursor > USER_DYNAMIC_LIMIT)
        cursor = USER_DYNAMIC_LIMIT;
    cursor &= ~(PAGE_SIZE - 1u);
    while (cursor >= USER_DYNAMIC_BASE + size) {
        base = cursor - size;
        if (net_user_range_free(base, size)) return base;
        cursor -= PAGE_SIZE;
    }
    cursor = USER_HIGH_DYNAMIC_LIMIT & ~(PAGE_SIZE - 1u);
    while (cursor >= USER_HIGH_DYNAMIC_BASE + size) {
        base = cursor - size;
        if (net_user_range_free(base, size)) return base;
        cursor -= PAGE_SIZE;
    }
    return 0;
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

#define user_range_mapped(addr, size) \
    syscall_user_range_present((addr), (size), 0, 1)

static int copy_user_string(char *dst, uint64_t dst_size, const char *src_user) {
    uint64_t addr;
    uint64_t pd;
    uint64_t i;
    uint64_t cur;
    char c;

    if (!dst || dst_size == 0) return -1;
    dst[0] = '\0';
    if (!src_user) return -1;
    addr = (uint64_t)(uintptr_t)src_user;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return -1;
    pd = get_user_pd();
    if (!pd) return -1;

    i = 0;
    while (i + 1 < dst_size) {
        cur = addr + i;
        if (cur >= KERNEL_VMA || cur < addr) return -1;
        if (vmm_get_phys_in_pml4(pd, cur & ~0xFFFu) == 0) return -1;
        c = *(const char *)cur;
        dst[i++] = c;
        if (c == '\0') return 0;
    }
    dst[dst_size - 1] = '\0';
    return -1;
}

static int copy_user_string_alloc(char **out, const char *src_user) {
    uint64_t addr;
    uint64_t pd;
    uint64_t length;
    uint64_t current;
    uint64_t page_end;
    char *copy;

    if (!out || !src_user) return -1;
    *out = NULL;
    addr = (uint64_t)(uintptr_t)src_user;
    if (addr < 0x1000 || addr >= KERNEL_VMA) return -1;
    pd = get_user_pd();
    if (!pd) return -1;
    length = 0;
    for (;;) {
        current = addr + length;
        if (current < addr || current >= KERNEL_VMA) return -1;
        if (vmm_get_phys_in_pml4(pd, current & ~0xFFFu) == 0) return -1;
        page_end = (current | 0xFFFu) + 1;
        while (current < page_end && current < KERNEL_VMA) {
            if (*(const char *)(uintptr_t)current == '\0') {
                if (length == SIZE_MAX) return -1;
                copy = (char *)kmalloc((size_t)length + 1);
                if (!copy) return -1;
                memcpy(copy, src_user, (size_t)length + 1);
                *out = copy;
                return 0;
            }
            current++;
            length++;
        }
    }
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

static int sys_net_ifconfig(int raw_ip, const char *raw_mask, int raw_gateway, int fields) {
    netif_t *netif;
    ipv4_addr_t ip;
    ipv4_addr_t netmask;
    ipv4_addr_t gateway;

    if (!fields) return -1;
    net_ensure_hw();
    netif = netif_get_default();
    if (!netif) {
        klog("net: no network interface found\n");
        return -1;
    }
    ip = (fields & 1) ? u32_to_ipv4((uint64_t)(uint32_t)raw_ip) : netif->ipv4;
    netmask = (fields & 2) ? u32_to_ipv4((uint64_t)(uint32_t)(uintptr_t)raw_mask) : netif->netmask;
    gateway = (fields & 4) ? u32_to_ipv4((uint64_t)(uint32_t)raw_gateway) : netif->gateway;
    dhcp_stop(netif);
    netif_set_ipv4(netif, ip, netmask, gateway);
    netif->dhcp_configured = 0;
    return 0;
}

static int sys_net_ping(int ip_packed, const char *unused2, int count) {
    ipv4_addr_t target;

    (void)unused2;
    net_ensure_hw();
    target = u32_to_ipv4((uint64_t)ip_packed);
    if (count <= 0) count = 4;
    return ping(target, count, 3000);
}

static int sys_net_arp(int unused, const char *unused2, int unused3) {
    (void)unused; (void)unused2; (void)unused3;
    net_ensure_hw();
    arp_print_cache();
    klog("arp: cache printed\n");
    return 0;
}

static int sys_net_dns(int unused, const char *hostname, uint64_t result_ptr) {
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
            if (copy_to_user((void *)(uintptr_t)result_ptr, &out,
                             sizeof(out)) != 0) return -1;
        }
    } else {
        klog("DNS: Failed to resolve %s\n", hostbuf);
    }
    return ret;
}

static int sys_net_dhcp(int cmd, const char *unused2, int unused3) {
    int con_id;
    int i;
    uint64_t gen;
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
        if (netif->loopback) {
            klog_con(con_id, "No network interface available\n");
            return -1;
        }
        klog_con(con_id, "DHCP: Starting...\n");
        dhcp_start(netif);
        return 0;
    } else if (cmd == 2) {
        if (netif->loopback) {
            klog_con(con_id, "No network interface available\n");
            return -1;
        }
        if (dhcp_is_bound(netif)) {
            klog_con(con_id, "DHCP: Already configured (%u.%u.%u.%u)\n",
                     netif->ipv4.octets[0], netif->ipv4.octets[1],
                     netif->ipv4.octets[2], netif->ipv4.octets[3]);
            return 0;
        }
        for (i = 0; i < 10; i++) {
            gen = descriptor_ready_generation();
            netif_poll_all();
            dhcp_tick();
            if (netif->link_up) break;
            if (task_has_pending_signals()) return -EINTR;
            descriptor_ready_wait(gen, pit_ms_to_ticks(10));
        }
        if (!netif->link_up) {
            klog_con(con_id, "DHCP: No link detected\n");
            return -1;
        }
        klog_con(con_id, "DHCP: Link up, starting DHCP...\n");
        dhcp_start(netif);
        for (i = 0; i < 500; i++) {
            gen = descriptor_ready_generation();
            netif_poll_all();
            dhcp_tick();
            if (dhcp_is_bound(netif)) break;
            if (task_has_pending_signals()) return -EINTR;
            descriptor_ready_wait(gen, pit_ms_to_ticks(10));
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
            return 0;
        }
        klog_con(con_id, "DHCP: Timed out\n");
        return -1;
    }
    return -1;
}

static int sys_net_dns_set(uint64_t addrs_ptr, const char *unused, int count) {
    netif_t *netif;
    uint32_t *raw;
    ipv4_addr_t *servers;
    uint64_t need;
    ipv4_addr_t second;
    int i;
    int ret;

    (void)unused;
    if (!addrs_ptr || count <= 0) return -1;
    if ((uint64_t)count > SIZE_MAX / sizeof(uint32_t)) return -1;
    need = (uint64_t)count * sizeof(uint32_t);
    if (!user_range_mapped((uint64_t)addrs_ptr, need)) return -1;
    raw = (uint32_t *)kmalloc(need);
    if (!raw) return -1;
    if (copy_from_user(raw, (const void *)(uintptr_t)addrs_ptr,
                       (size_t)need) != 0) {
        kfree(raw);
        return -1;
    }
    if (raw[0] == 0) {
        kfree(raw);
        return -1;
    }
    if ((uint64_t)count > SIZE_MAX / sizeof(*servers)) {
        kfree(raw);
        return -1;
    }
    servers = (ipv4_addr_t *)kmalloc((uint64_t)count * sizeof(*servers));
    if (!servers) {
        kfree(raw);
        return -1;
    }
    for (i = 0; i < count; i++) servers[i] = u32_to_ipv4(raw[i]);
    kfree(raw);
    net_ensure_hw();
    netif = netif_get_default();
    if (!netif) {
        kfree(servers);
        return -1;
    }
    ret = dns_set_servers(servers, count);
    if (ret == 0) {
        second = count > 1 ? dns_server_at(1) : u32_to_ipv4(0);
        netif_set_dns(netif, dns_server_at(0), second);
    }
    kfree(servers);
    return ret;
}

static int sys_net_dns_get(uint64_t buf_ptr, const char *count_ptr, int max_entries) {
    int count;
    int i;
    uint64_t need;
    uint32_t *out;
    uint32_t addr;

    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    count = dns_server_count();
    if (count > max_entries) count = max_entries;
    if (count > 0) {
        if ((uint64_t)count > SIZE_MAX / sizeof(uint32_t)) return -1;
        need = (uint64_t)count * sizeof(uint32_t);
        if (!user_range_mapped((uint64_t)buf_ptr, need)) return -1;
    }
    if (!user_range_mapped((uint64_t)(uintptr_t)count_ptr, sizeof(int))) return -1;
    out = (uint32_t *)(uintptr_t)buf_ptr;
    for (i = 0; i < count; i++) {
        addr = (uint32_t)ipv4_to_u32(dns_server_at(i));
        if (copy_to_user(&out[i], &addr, sizeof(addr)) != 0) return -1;
    }
    if (copy_to_user((void *)(uintptr_t)count_ptr, &count, sizeof(count)) != 0) return -1;
    return 0;
}

static int sys_net_dns6_set(uint64_t addrs_ptr, const char *unused, int count) {
    netif_t *netif;
    ipv6_addr_t *servers;
    uint64_t need;
    int j;
    int zero;
    int ret;

    (void)unused;
    if (!addrs_ptr || count <= 0) return -1;
    if ((uint64_t)count > SIZE_MAX / sizeof(*servers)) return -1;
    need = (uint64_t)count * sizeof(*servers);
    if (!user_range_mapped((uint64_t)addrs_ptr, need)) return -1;
    servers = (ipv6_addr_t *)kmalloc(need);
    if (!servers) return -1;
    if (copy_from_user(servers, (const void *)(uintptr_t)addrs_ptr,
                       (size_t)need) != 0) {
        kfree(servers);
        return -1;
    }
    zero = 1;
    for (j = 0; j < 16; j++) {
        if (servers[0].octets[j]) {
            zero = 0;
            break;
        }
    }
    if (zero) {
        kfree(servers);
        return -1;
    }
    net_ensure_hw();
    netif = netif_get_default();
    if (!netif) {
        kfree(servers);
        return -1;
    }
    ret = dns6_set_servers(servers, count);
    kfree(servers);
    return ret;
}

static int sys_net_dns6_get(uint64_t buf_ptr, const char *count_ptr, int max_entries) {
    int count;
    int i;
    uint64_t need;
    ipv6_addr_t *out;
    ipv6_addr_t addr;

    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    count = dns6_server_count();
    if (count > max_entries) count = max_entries;
    if (count > 0) {
        if ((uint64_t)count > SIZE_MAX / sizeof(*out)) return -1;
        need = (uint64_t)count * sizeof(*out);
        if (!user_range_mapped((uint64_t)buf_ptr, need)) return -1;
    }
    if (!user_range_mapped((uint64_t)(uintptr_t)count_ptr, sizeof(int))) return -1;
    out = (ipv6_addr_t *)(uintptr_t)buf_ptr;
    for (i = 0; i < count; i++) {
        addr = dns6_server_at(i);
        if (copy_to_user(&out[i], &addr, sizeof(addr)) != 0) return -1;
    }
    if (copy_to_user((void *)(uintptr_t)count_ptr, &count, sizeof(count)) != 0) return -1;
    return 0;
}

static int sys_net_getinfo(uint64_t buf_ptr, const char *unused2, int unused3) {
    netif_t *netif;
    netinfo_user_t info;
    int i;

    (void)unused2; (void)unused3;
    if (!buf_ptr) return -1;
    net_ensure_hw();
    
    netif = netif_get_default();
    if (!netif) return -1;
    
    memset(&info, 0, sizeof(info));

    for (i = 0; i < 15 && netif->name[i]; i++) {
        info.name[i] = netif->name[i];
    }
    info.name[15] = '\0';

    for (i = 0; i < 6; i++) {
        info.mac[i] = netif->mac.addr[i];
    }

    info.ipv4 = ipv4_to_u32(netif->ipv4);
    info.netmask = ipv4_to_u32(netif->netmask);
    info.gateway = ipv4_to_u32(netif->gateway);
    info.dns = ipv4_to_u32(netif->dns_server);
    info.mtu = netif->mtu;
    info.link_up = netif->link_up;
    info.dhcp_configured = netif->dhcp_configured;
    for (i = 0; i < 16; i++) info.ipv6[i] = netif->ipv6.octets[i];
    info.ipv6_prefix = netif->ipv6_prefix;

    if (copy_to_user((void *)(uintptr_t)buf_ptr, &info,
                     sizeof(info)) != 0) return -1;

    return 0;
}

static int sys_net_arp_get(uint64_t buf_ptr, const char *count_ptr,
                           int max_entries) {
    uint64_t *ips;
    uint8_t *macs;
    int count;
    uint64_t need;
    arp_user_entry_t *entries;
    int i;
    int j;

    if (!buf_ptr || !count_ptr || max_entries <= 0) return -1;
    net_ensure_hw();
    if ((uint64_t)max_entries > SIZE_MAX / sizeof(*ips) ||
        (uint64_t)max_entries > SIZE_MAX / 6) return -1;
    ips = (uint64_t *)kmalloc((uint64_t)max_entries * sizeof(*ips));
    if (!ips) return -1;
    macs = (uint8_t *)kmalloc((uint64_t)max_entries * 6);
    if (!macs) {
        kfree(ips);
        return -1;
    }
    count = arp_get_cache(ips, macs, max_entries);
    
    if (count > 0) {
        need = (uint64_t)count * (uint64_t)sizeof(arp_user_entry_t);
        if (!user_range_mapped((uint64_t)buf_ptr, need)) {
            kfree(macs);
            kfree(ips);
            return -1;
        }
    }
    if (!user_range_mapped((uint64_t)(uintptr_t)count_ptr, sizeof(int))) {
        kfree(macs);
        kfree(ips);
        return -1;
    }

    entries = (arp_user_entry_t *)(uintptr_t)buf_ptr;
    for (i = 0; i < count; i++) {
        arp_user_entry_t entry;
        entry.ip = ips[i];
        for (j = 0; j < 6; j++) {
            entry.mac[j] = macs[i * 6 + j];
        }
        if (copy_to_user(&entries[i], &entry, sizeof(entry)) != 0) {
            kfree(macs);
            kfree(ips);
            return -1;
        }
    }

    if (copy_to_user((void *)(uintptr_t)count_ptr, &count,
                     sizeof(count)) != 0) {
        kfree(macs);
        kfree(ips);
        return -1;
    }
    kfree(macs);
    kfree(ips);
    return 0;
}

static int sys_net_ping_one(int ip_packed, const char *seq_ptr, int timeout_ms) {
    uint16_t seq;
    ipv4_addr_t target;

    net_ensure_hw();
    target = u32_to_ipv4((uint64_t)ip_packed);
    seq = (uint16_t)(int)(size_t)seq_ptr;
    if (timeout_ms <= 0) timeout_ms = 3000;
    return ping_one(target, seq, (uint64_t)timeout_ms);
}

static int sys_net_dns_resolve(uint64_t hostname_ptr,
                               const char *result_ptr, int unused) {
    const char *hostname;
    char hostbuf[256];
    int ret;
    ipv4_addr_t resolved;

    (void)unused;
    hostname = (const char *)(uintptr_t)hostname_ptr;
    if (!hostname || !result_ptr) return -1;
    net_ensure_hw();
    if (copy_user_string(hostbuf, sizeof(hostbuf), hostname) != 0) return -1;
    if (!user_range_mapped((uint64_t)(uintptr_t)result_ptr, sizeof(uint64_t))) return -1;
    
    ret = dns_resolve(hostbuf, &resolved);
    if (ret == 0) {
        uint64_t out = ipv4_to_u32(resolved);
        if (copy_to_user((void *)(uintptr_t)result_ptr, &out,
                         sizeof(out)) != 0) return -1;
    }
    return ret;
}

static int __attribute__((noinline, noclone)) net_http_copy_body(
    uint64_t address, const uint8_t *body, uint64_t length) {
    if (copy_to_user((void *)(uintptr_t)address, body, length) != 0)
        return -1;
    return 0;
}

static int __attribute__((noinline, noclone)) net_http_copy_headers(
    uint8_t *destination, uint64_t capacity, uint8_t *headers,
    uint64_t *length, int result) {
    uint64_t copied;

    if (headers && *length > 0 && capacity > 0) {
        copied = *length < capacity ? *length : capacity;
        if (copy_to_user(destination, headers, copied) != 0)
            result = -1;
        *length = copied;
    } else if (headers) {
        *length = 0;
    }
    if (headers) kfree(headers);
    return result;
}

static void net_http_store_size(uint64_t *destination, uint64_t value) {
    if (destination)
        copy_to_user((void *)(uintptr_t)destination, &value,
                     sizeof(value));
}

static void net_http_store_status(int *destination, int value) {
    if (destination)
        copy_to_user((void *)(uintptr_t)destination, &value,
                     sizeof(value));
}

static int sys_net_http_get(uint64_t req_ptr, const char *unused1,
                            int unused2) {
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

    if (copy_from_user(&req, (const void *)(uintptr_t)req_ptr,
                       sizeof(req)) != 0) return -1;
    if (!req.url || !req.buffer || req.buffer_size == 0) return -1;

    url_buf = NULL;
    if (copy_user_string_alloc(&url_buf, req.url) != 0) return -1;

    user_buf_addr = (uint64_t)(uintptr_t)req.buffer;
    if (!user_range_ok(user_buf_addr, req.buffer_size)) { kfree(url_buf); return -1; }

    max_redir = req.max_redirects;
    if (max_redir < 0) max_redir = 0;

    khdr = NULL;
    hdr_buf_sz = 0;
    if (req.headers_buf && req.headers_buf_size > 0) {
        hdr_buf_sz = req.headers_buf_size;
        if (!user_range_ok((uint64_t)(uintptr_t)req.headers_buf, hdr_buf_sz)) {
            kfree(url_buf);
            return -1;
        }
    }

    kbuf = NULL;
    downloaded = 0;
    status_code = 0;
    hdr_len = 0;
#if CONFIG_DRIVER_NET
    ret = http_download_alloc(url_buf, &kbuf, &downloaded, &status_code,
                              max_redir, &khdr, &hdr_len);
#else
    ret = -ENOSYS;
#endif

    if (ret == 0 && kbuf && downloaded > 0) {
        copy_len = downloaded < req.buffer_size ? downloaded : req.buffer_size;
        if (net_http_copy_body(user_buf_addr, kbuf, copy_len) < 0) {
            kfree(kbuf);
            if (khdr) kfree(khdr);
            kfree(url_buf);
            return -1;
        }
        downloaded = copy_len;
    }
    if (kbuf) kfree(kbuf);
    kfree(url_buf);

    ret = net_http_copy_headers(req.headers_buf, hdr_buf_sz, khdr,
                                &hdr_len, ret);

    net_http_store_size(req.out_size, downloaded);

    net_http_store_status(req.status_code, status_code);

    net_http_store_size(req.out_headers_len, hdr_len);

    return ret;
}

static int sys_net_http_post(uint64_t req_ptr, const char *unused1,
                             int unused2) {
    char *url_buf;
    char *ct_buf;
    uint64_t user_buf_addr;
    uint8_t *kbuf;
    uint64_t downloaded;
    uint64_t copy_len;
    int status;
    int ret;
    http_post_request_user_t req;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;
    net_ensure_hw();

    if (copy_from_user(&req, (const void *)(uintptr_t)req_ptr,
                       sizeof(req)) != 0) return -1;
    if (!req.url || !req.buffer || req.buffer_size == 0) return -1;

    url_buf = NULL;
    ct_buf = NULL;
    if (copy_user_string_alloc(&url_buf, req.url) != 0) return -1;
    if (req.content_type) {
        if (copy_user_string_alloc(&ct_buf, req.content_type) != 0) { kfree(url_buf); return -1; }
    }

    user_buf_addr = (uint64_t)(uintptr_t)req.buffer;
    if (!user_range_ok(user_buf_addr, req.buffer_size)) { kfree(url_buf); kfree(ct_buf); return -1; }

    if (req.post_body && req.post_body_len > 0) {
        if (!user_range_mapped((uint64_t)(uintptr_t)req.post_body, req.post_body_len)) { kfree(url_buf); kfree(ct_buf); return -1; }
    }

    kbuf = NULL;
    downloaded = 0;
    status = 0;
#if CONFIG_DRIVER_NET
    ret = http_post_download_alloc(
        url_buf, ct_buf,
        (const uint8_t *)(uintptr_t)req.post_body,
        req.post_body ? req.post_body_len : 0,
        &kbuf, &downloaded, &status);
#else
    ret = -ENOSYS;
#endif

    copy_len = downloaded < req.buffer_size ? downloaded : req.buffer_size;
    if (ret == 0 && kbuf && copy_len > 0) {
        if (net_http_copy_body(user_buf_addr, kbuf, copy_len) < 0) {
            kfree(kbuf);
            kfree(url_buf); kfree(ct_buf);
            return -1;
        }
    }
    if (kbuf) kfree(kbuf);
    downloaded = copy_len;
    kfree(url_buf); kfree(ct_buf);

    net_http_store_size(req.out_size, downloaded);

    net_http_store_status(req.status_code, status);

    return ret;
}

static int sys_net_http_get_alloc(uint64_t req_ptr, const char *unused1,
                                  int unused2) {
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
    uint64_t i;
    http_get_alloc_req_t req;

    (void)unused1; (void)unused2;
    if (!req_ptr) return -1;
    if (!current_task) return -1;
    net_ensure_hw();

    if (copy_from_user(&req, (const void *)(uintptr_t)req_ptr,
                       sizeof(req)) != 0) return -1;
    if (!req.url || !req.out_buffer || !req.out_size) return -1;
    if (!user_range_mapped((uint64_t)(uintptr_t)req.out_buffer,
                           sizeof(uint64_t)) ||
        !user_range_mapped((uint64_t)(uintptr_t)req.out_size,
                           sizeof(uint64_t))) return -1;

    url_buf = NULL;
    if (copy_user_string_alloc(&url_buf, req.url) != 0) return -1;

    max_redir = req.max_redirects;
    if (max_redir < 0) max_redir = 0;

    khdr = NULL;
    hdr_buf_sz = 0;
    if (req.headers_buf && req.headers_buf_size > 0) {
        hdr_buf_sz = req.headers_buf_size;
        if (!user_range_ok((uint64_t)(uintptr_t)req.headers_buf, hdr_buf_sz)) {
            kfree(url_buf);
            return -1;
        }
    }

    kbuf = NULL;
    downloaded = 0;
    status_code = 0;
    hdr_len = 0;
#if CONFIG_DRIVER_NET
    ret = http_download_alloc(url_buf, &kbuf, &downloaded, &status_code,
                              max_redir, &khdr, &hdr_len);
#else
    ret = -ENOSYS;
#endif
    kfree(url_buf);

    ret = net_http_copy_headers(req.headers_buf, hdr_buf_sz, khdr,
                                &hdr_len, ret);

    net_http_store_status(req.status_code, status_code);

    net_http_store_size(req.out_headers_len, hdr_len);

    if (ret < 0 || !kbuf || downloaded == 0) {
        if (kbuf) kfree(kbuf);
        net_http_store_size(req.out_size, 0);
        net_http_store_size(req.out_buffer, 0);
        return ret;
    }

    if (downloaded > UINT64_MAX - 0xFFFu) {
        kfree(kbuf);
        return -1;
    }
    alloc_size = net_align_up(downloaded, 0x1000u);
    if (alloc_size == 0) alloc_size = 0x1000u;

    base = net_find_user_mapping(current_task->mmap_next_addr, alloc_size);
    if (!base) {
        kfree(kbuf);
        return -1;
    }

    if (task_add_vm_area(current_task, NULL, base, alloc_size, 0, 0,
                         0x7, TASK_VMA_PRIVATE | TASK_VMA_ANONYMOUS) != 0) {
        kfree(kbuf);
        return -1;
    }

    page_count = 0;
    new_pages = vmm_map_range_in_pml4_tracked(
        current_task->pml4_phys, base, alloc_size, 0x7, &page_count);

    if (!new_pages) {
        task_unmap_vm_areas(current_task, base, alloc_size);
        kfree(kbuf);
        return -1;
    }

    if (page_count > 0) {
        old_count = current_task->user_pages_count;
        if (page_count > UINT64_MAX - old_count ||
            old_count + page_count > UINT64_MAX / sizeof(uint64_t)) {
            for (i = 0; i < page_count; i++) {
                vmm_unmap_page_in_pml4(current_task->pml4_phys,
                                       base + i * PAGE_SIZE);
                pfa_free(new_pages[i]);
            }
            vmm_prune_user_range(current_task->pml4_phys, base, alloc_size);
            task_unmap_vm_areas(current_task, base, alloc_size);
            kfree(new_pages);
            kfree(kbuf);
            return -1;
        }
        new_count = old_count + page_count;
        expanded = (uint64_t *)kmalloc(new_count * sizeof(uint64_t));
        if (!expanded) {
            for (i = 0; i < page_count; i++) {
                vmm_unmap_page_in_pml4(current_task->pml4_phys,
                                       base + i * PAGE_SIZE);
                pfa_free(new_pages[i]);
            }
            vmm_prune_user_range(current_task->pml4_phys, base, alloc_size);
            task_unmap_vm_areas(current_task, base, alloc_size);
            kfree(new_pages);
            kfree(kbuf);
            return -1;
        }
        if (current_task->user_pages && old_count > 0) {
            memcpy(expanded, current_task->user_pages,
                   old_count * sizeof(uint64_t));
            kfree(current_task->user_pages);
        }
        memcpy(expanded + old_count, new_pages,
               page_count * sizeof(uint64_t));
        current_task->user_pages = expanded;
        current_task->user_pages_count = new_count;
        kfree(new_pages);
    }

    current_task->mmap_next_addr = base;

    memcpy((void *)base, kbuf, downloaded);
    kfree(kbuf);

    net_http_store_size(req.out_buffer, base);

    net_http_store_size(req.out_size, downloaded);

    return 0;
}

void syscalls_net_init(void) {
    syscall_table_set(SYSCALL_NET_IFCONFIG, (void *)(sys_net_ifconfig));
    syscall_table_set(SYSCALL_NET_PING, (void *)(sys_net_ping));
    syscall_table_set(SYSCALL_NET_ARP, (void *)(sys_net_arp));
    syscall_table_set(SYSCALL_NET_DNS, (void *)(sys_net_dns));
    syscall_table_set(SYSCALL_NET_DHCP, (void *)(sys_net_dhcp));
    syscall_table_set(SYSCALL_NET_DNS_SET, (void *)(sys_net_dns_set));
    syscall_table_set(SYSCALL_NET_DNS_GET, (void *)(sys_net_dns_get));
    syscall_table_set(SYSCALL_NET_DNS6_SET, (void *)(sys_net_dns6_set));
    syscall_table_set(SYSCALL_NET_DNS6_GET, (void *)(sys_net_dns6_get));
    syscall_table_set(SYSCALL_NET_GETINFO, (void *)(sys_net_getinfo));
    syscall_table_set(SYSCALL_NET_ARP_GET, (void *)(sys_net_arp_get));
    syscall_table_set(SYSCALL_NET_PING_ONE, (void *)(sys_net_ping_one));
    syscall_table_set(SYSCALL_NET_DNS_RESOLVE, (void *)(sys_net_dns_resolve));
    syscall_table_set(SYSCALL_NET_HTTP_GET, (void *)(sys_net_http_get));
    syscall_table_set(SYSCALL_NET_HTTP_POST, (void *)(sys_net_http_post));
    syscall_table_set(SYSCALL_NET_HTTP_GET_ALLOC, (void *)(sys_net_http_get_alloc));
}

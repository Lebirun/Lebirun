#include <lebirun/drivers/net/netif.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <string.h>

static netif_t *netif_list = NULL;
static netif_t *netif_default = NULL;

void KERNEL_INIT netif_init(void) {
    netif_list = NULL;
    netif_default = NULL;
}

netif_t *netif_alloc(void) {
    netif_t *netif = (netif_t *)kmalloc(sizeof(netif_t));
    if (!netif) return NULL;

    memset(netif, 0, sizeof(netif_t));
    netif->mtu = ETH_MTU;
    return netif;
}

void netif_register(netif_t *netif) {
    if (!netif) return;

    netif->next = netif_list;
    netif_list = netif;
}

netif_t *netif_get_default(void) {
    return netif_default;
}

netif_t *netif_find(const char *name) {
    netif_t *netif = netif_list;
    while (netif) {
        int match = 1;
        for (int i = 0; i < 16 && name[i]; i++) {
            if (netif->name[i] != name[i]) {
                match = 0;
                break;
            }
        }
        if (match) return netif;
        netif = netif->next;
    }
    return NULL;
}

void netif_set_default(netif_t *netif) {
    netif_default = netif;
}

void netif_set_ipv4(netif_t *netif, ipv4_addr_t ip, ipv4_addr_t netmask, ipv4_addr_t gateway) {
    if (!netif) return;

    netif->ipv4 = ip;
    netif->netmask = netmask;
    netif->gateway = gateway;
}

void netif_set_dns(netif_t *netif, ipv4_addr_t dns1, ipv4_addr_t dns2) {
    if (!netif) return;

    netif->dns_server = dns1;
    netif->dns_server2 = dns2;
}

void netif_set_ipv6(netif_t *netif, ipv6_addr_t ip, uint8_t prefix, ipv6_addr_t gateway) {
    if (!netif) return;

    netif->ipv6 = ip;
    netif->ipv6_prefix = prefix;
    netif->ipv6_gateway = gateway;
}

int netif_send(netif_t *netif, uint8_t *data, uint64_t len) {
    if (!netif || !netif->send) return -1;
    return netif->send(netif, data, len);
}

void netif_poll_all(void) {
    netif_t *netif = netif_list;
    while (netif) {
        if (netif->poll) {
            netif->poll(netif);
        }
        netif = netif->next;
    }
}

void netif_print_info(netif_t *netif) {
    if (!netif) return;

    printf("Interface: %s\n", netif->name);
    printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           netif->mac.addr[0], netif->mac.addr[1], netif->mac.addr[2],
           netif->mac.addr[3], netif->mac.addr[4], netif->mac.addr[5]);
    printf("  IPv4: %u.%u.%u.%u\n",
           netif->ipv4.octets[0], netif->ipv4.octets[1],
           netif->ipv4.octets[2], netif->ipv4.octets[3]);
    printf("  Netmask: %u.%u.%u.%u\n",
           netif->netmask.octets[0], netif->netmask.octets[1],
           netif->netmask.octets[2], netif->netmask.octets[3]);
    printf("  Gateway: %u.%u.%u.%u\n",
           netif->gateway.octets[0], netif->gateway.octets[1],
           netif->gateway.octets[2], netif->gateway.octets[3]);
    printf("  DNS: %u.%u.%u.%u\n",
           netif->dns_server.octets[0], netif->dns_server.octets[1],
           netif->dns_server.octets[2], netif->dns_server.octets[3]);
    printf("  MTU: %u\n", netif->mtu);
    printf("  Link: %s\n", netif->link_up ? "UP" : "DOWN");
    printf("  DHCP: %s\n", netif->dhcp_configured ? "Yes" : "No");
}

#include <kernel/drivers/net/net.h>
#include <kernel/drivers/net/e1000/e1000.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

const mac_addr_t MAC_BROADCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
const mac_addr_t MAC_ZERO = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
const ipv6_addr_t IPV6_ZERO = {{0}};

static uint32_t net_ticks = 0;

uint32_t net_get_ticks(void) {
    return net_ticks;
}

void net_tick(void) {
    net_ticks++;
    netif_poll_all();
    tcp_tick();
    dhcp_tick();
}

void net_poll(void) {
    netif_poll_all();
}

void net_init(void) {
    printf("NET: Initializing network stack...\n");

    netif_init();
    arp_init();
    ipv4_init();
    ipv6_init();
    udp_init();
    tcp_init();
    dns_init();

    if (e1000_init() < 0) {
        printf("NET: No network interface available\n");
        return;
    }

    netif_t *netif = netif_get_default();
    if (netif) {
        dhcp_init(netif);
        dhcp_start(netif);
    }

    printf("NET: Network stack initialized\n");
}

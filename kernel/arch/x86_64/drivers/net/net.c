#include <lebirun/drivers/net/net.h>
#if CONFIG_DRIVER_NET_E1000
#include <lebirun/drivers/net/e1000/e1000.h>
#endif
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/task.h>
#include <string.h>

const mac_addr_t MAC_BROADCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
const mac_addr_t MAC_ZERO = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
const ipv6_addr_t IPV6_ZERO = {{0}};

static volatile uint64_t net_ticks = 0;
volatile int net_work_pending = 0;

uint64_t net_get_ticks(void) {
    return net_ticks;
}

void net_tick(void) {
    net_ticks++;
    net_work_pending = 1;
}

void net_poll(void) {
    netif_poll_all();
}

static int net_hw_initialized;
static int net_has_interface;

void net_ensure_hw(void) {
    netif_t *netif;
    int i;

    if (!net_has_interface) return;
    if (net_hw_initialized) return;
    net_hw_initialized = 1;

    netif = netif_get_default();
    if (!netif) return;

    for (i = 0; i < 10; i++) {
        sleep_ms(10);
        netif_poll_all();
        if (netif->link_up) break;
    }
}

static void net_worker_thread(void) {
    while (1) {
        if (net_work_pending) {
            net_work_pending = 0;
            netif_poll_all();
            tcp_tick();
            dhcp_tick();
        }
        if (dhcp_is_negotiating()) {
            netif_poll_all();
            yield();
        } else {
            sleep_ms(1);
        }
    }
}

void net_init(void) {
    task_t *nt;
    netif_t *netif;

    printf("NET: Initializing network stack...\n");

    netif_init();
    arp_init();
    ipv4_init();
    ipv6_init();
    udp_init();
    tcp_init();
    dns_init();

#if CONFIG_DRIVER_NET_E1000
    if (e1000_init() < 0) {
        printf("NET: No network interface available\n");
    } else {
        net_has_interface = 1;
        netif = netif_get_default();
        if (netif)
            dhcp_init(netif);
    }
#else
    (void)netif;
    printf("NET: E1000 driver disabled\n");
#endif

    nt = create_kernel_task(net_worker_thread, TASK_READY);
    if (nt) {
        strcpy(nt->name, "net_worker");
        lock_scheduler();
        add_task_to_runqueue(nt);
        unlock_scheduler();
    }

    printf("NET: Network stack initialized\n");
}

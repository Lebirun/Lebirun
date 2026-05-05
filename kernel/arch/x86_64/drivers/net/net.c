#include <lebirun/drivers/net/net.h>
#if CONFIG_DRIVER_NET_E1000
#include <lebirun/drivers/net/e1000/e1000.h>
#endif
#include <lebirun/mem_map.h>
#include <lebirun/spinlock.h>
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
static int net_worker_started;
static spinlock_t net_hw_lock = {0};

static void net_start_worker(void);

void net_ensure_hw(void) {
    netif_t *netif;
    int i;

    if (net_hw_initialized) return;
    while (!spin_trylock(&net_hw_lock)) {
        __asm__ volatile("sti");
        schedule();
    }
    if (net_hw_initialized) {
        spin_unlock(&net_hw_lock);
        return;
    }

#if CONFIG_DRIVER_NET_E1000
    if (e1000_init() < 0) {
        printf("NET: No network interface available\n");
        net_hw_initialized = 1;
        spin_unlock(&net_hw_lock);
        return;
    }
    net_has_interface = 1;
    netif = netif_get_default();
    if (netif)
        dhcp_init(netif);
#else
    printf("NET: E1000 driver disabled\n");
    net_hw_initialized = 1;
    spin_unlock(&net_hw_lock);
    return;
#endif

    net_hw_initialized = 1;
    spin_unlock(&net_hw_lock);

    net_start_worker();

    netif = netif_get_default();
    if (!netif) return;

    for (i = 0; i < 10; i++) {
        sleep_ms(10);
        netif_poll_all();
        if (netif->link_up) break;
    }
}

static void net_worker_thread(void) {
    uint64_t last_poll;

    last_poll = 0;
    while (1) {
        if (net_work_pending || net_get_ticks() - last_poll >= 10) {
            net_work_pending = 0;
            netif_poll_all();
            tcp_tick();
            dhcp_tick();
            last_poll = net_get_ticks();
        }
        if (dhcp_is_negotiating()) {
            netif_poll_all();
            yield();
        } else {
            sleep_ms(1);
        }
    }
}

static void net_start_worker(void) {
    task_t *nt;

    if (net_worker_started) return;
    net_worker_started = 1;
    nt = create_kernel_task(net_worker_thread, TASK_READY);
    if (nt) {
        strcpy(nt->name, "net_worker");
        lock_scheduler();
        add_task_to_runqueue(nt);
        unlock_scheduler();
    }
}

void net_init(void) {
    printf("NET: Initializing network stack...\n");

    net_hw_initialized = 0;
    net_has_interface = 0;
    net_worker_started = 0;

    netif_init();
    arp_init();
    ipv4_init();
    ipv6_init();
    udp_init();
    tcp_init();
    dns_init();

    printf("NET: Network stack initialized\n");
}

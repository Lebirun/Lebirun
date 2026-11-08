#include <lebirun/drivers/net/net.h>
#if CONFIG_DRIVER_NET_VIRTIO
#include <lebirun/drivers/net/virtio_net/virtio_net.h>
#endif
#if CONFIG_DRIVER_NET_E1000
#include <lebirun/drivers/net/e1000/e1000.h>
#endif
#include <lebirun/mem_map.h>
#include <lebirun/spinlock.h>
#include <lebirun/tty.h>
#include <lebirun/task.h>
#include <lebirun/common.h>
#include <lebirun/pit.h>
#include <lebirun/pci.h>
#include <string.h>

const mac_addr_t MAC_BROADCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
const mac_addr_t MAC_ZERO = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
const ipv6_addr_t IPV6_ZERO = {{0}};

static volatile uint64_t net_ticks = 0;

uint64_t net_get_ticks(void) {
    return net_ticks;
}

void net_tick(void) {
    net_ticks = pit_get_ticks();
}

void net_poll(void) {
    netif_poll_all();
}

static uint32_t net_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr;
    uint32_t val;

    addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)0xCFC));
    return val;
}

static net_driver_t *net_drivers;
static int net_hw_initialized;
static int net_hw_up;
static spinlock_t net_hw_lock = {0};

static void net_hw_lock_acquire(void) {
    while (!spin_trylock(&net_hw_lock)) {
        __asm__ volatile("sti");
        schedule();
    }
}

int net_register_driver(net_driver_t *drv) {
    net_driver_t **p;
    const uint16_t *d;
    uint32_t i;
    uint32_t bdf;
    uint32_t id;

    for (i = 0; pci_get(i, &bdf) == 0; i++) {
        id = net_pci_read32(PCI_BUS(bdf), PCI_SLOT(bdf), PCI_FUNC(bdf), 0);
        if ((id & 0xFFFF) != drv->vendor) continue;
        for (d = drv->devices; *d && *d != id >> 16; d++);
        if (*d) goto found;
    }
    return -1;
found:
    drv->bus = PCI_BUS(bdf);
    drv->slot = PCI_SLOT(bdf);
    drv->func = PCI_FUNC(bdf);
    drv->next = NULL;
    net_hw_lock_acquire();
    for (p = &net_drivers; *p; p = &(*p)->next);
    *p = drv;
    if (!net_hw_up) net_hw_initialized = 0;
    spin_unlock(&net_hw_lock);
    return 0;
}

void net_ensure_hw(void) {
    net_driver_t *drv;
    netif_t *netif;
    int i;

    if (net_hw_initialized) return;
    net_hw_lock_acquire();
    if (net_hw_initialized) {
        spin_unlock(&net_hw_lock);
        return;
    }

    for (drv = net_drivers; drv && !net_hw_up; drv = drv->next)
        net_hw_up = drv->init_at(drv->bus, drv->slot, drv->func) == 0;
    if (!net_hw_up) {
        if (!net_drivers) printf("NET: No network interface available\n");
        net_hw_initialized = 1;
        spin_unlock(&net_hw_lock);
        return;
    }
    netif = netif_get_default();
    if (netif)
        dhcp_init(netif);

    net_hw_initialized = 1;
    spin_unlock(&net_hw_lock);

    netif = netif_get_default();
    if (!netif) return;

    for (i = 0; i < 10; i++) {
        sleep_ms(10);
        netif_poll_all();
        if (netif->link_up) break;
    }
}

void KERNEL_INIT net_init(void) {
    KERNEL_INIT_LOG("NET: Initializing network stack...\n");

    net_hw_initialized = 0;

    netif_init();
    netif_loopback_init();
    arp_init();
    ipv4_init();
    ipv6_init();
    udp_init();
    tcp_init();
    dns_init();
#if CONFIG_DRIVER_NET_VIRTIO
    net_register_driver(&virtio_net_driver);
#endif
#if CONFIG_DRIVER_NET_E1000
    net_register_driver(&e1000_driver);
#endif

    KERNEL_INIT_LOG("NET: Network stack initialized\n");
}

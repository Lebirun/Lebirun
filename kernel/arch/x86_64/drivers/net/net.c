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
#include <string.h>

const mac_addr_t MAC_BROADCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
const mac_addr_t MAC_ZERO = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
const ipv6_addr_t IPV6_ZERO = {{0}};

static volatile uint64_t net_ticks = 0;

uint64_t net_get_ticks(void) {
    return net_ticks;
}

void net_tick(void) {
    net_ticks++;
}

void net_poll(void) {
    netif_poll_all();
}

#if CONFIG_DRIVER_NET_VIRTIO || CONFIG_DRIVER_NET_E1000
static uint32_t net_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr;
    uint32_t val;

    addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)0xCFC));
    return val;
}

#define NET_PCI_NONE 0
#define NET_PCI_E1000 1
#define NET_PCI_VIRTIO 2

static int net_pci_find(uint8_t *bus, uint8_t *slot, uint8_t *func) {
    uint16_t b;
    uint8_t s;
    uint8_t f;
    uint32_t id;
    uint16_t vendor;
    uint16_t device;

    for (b = 0; b < 256; b++) {
        for (s = 0; s < 32; s++) {
            for (f = 0; f < 8; f++) {
                id = net_pci_read32((uint8_t)b, s, f, 0);
                vendor = id & 0xFFFF;
                device = (id >> 16) & 0xFFFF;
#if CONFIG_DRIVER_NET_VIRTIO
                if (vendor == VIRTIO_NET_VENDOR_ID &&
                    (device == VIRTIO_NET_DEV_MODERN || device == VIRTIO_NET_DEV_LEGACY)) {
                    *bus = (uint8_t)b;
                    *slot = s;
                    *func = f;
                    return NET_PCI_VIRTIO;
                }
#endif
#if CONFIG_DRIVER_NET_E1000
                if (vendor == E1000_VENDOR_ID &&
                    (device == E1000_DEVICE_82540EM ||
                     device == E1000_DEVICE_82545EM ||
                     device == E1000_DEVICE_82574L)) {
                    *bus = (uint8_t)b;
                    *slot = s;
                    *func = f;
                    return NET_PCI_E1000;
                }
#endif
                if (f == 0 && id == 0xFFFFFFFFU) break;
            }
        }
    }
    return NET_PCI_NONE;
}
#endif

static int net_hw_initialized;
static spinlock_t net_hw_lock = {0};

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

    {
        int ok = 0;
#if CONFIG_DRIVER_NET_VIRTIO || CONFIG_DRIVER_NET_E1000
        {
            int found;
            uint8_t bus;
            uint8_t slot;
            uint8_t func;

            found = net_pci_find(&bus, &slot, &func);
            if (found == NET_PCI_VIRTIO) {
#if CONFIG_DRIVER_NET_VIRTIO
                ok = virtio_net_init_at(bus, slot, func) == 0;
#else
                printf("NET: Virtio-Net device found but driver disabled\n");
#endif
            } else if (found == NET_PCI_E1000) {
#if CONFIG_DRIVER_NET_E1000
                ok = e1000_init_at(bus, slot, func) == 0;
#else
                printf("NET: E1000 device found but driver disabled\n");
#endif
            } else {
                printf("NET: No network interface available\n");
            }
        }
#else
        printf("NET: No network driver enabled\n");
#endif
        if (!ok) {
            net_hw_initialized = 1;
            spin_unlock(&net_hw_lock);
            return;
        }
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

    KERNEL_INIT_LOG("NET: Network stack initialized\n");
}

#include <lebirun/drivers/net/virtio_net/virtio_net.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/ethernet.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/idt.h>
#include <lebirun/task.h>
#include <lebirun/tty.h>
#include <string.h>

#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR 3
#define VIRTIO_PCI_CAP_DEVICE 4

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_MRG_RXBUF 15
#define VIRTIO_F_VERSION_1 32

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_NET_HDR_LEN 12
#define VIRTIO_NET_RX_POSTED 64
#define VIRTIO_NET_RX_STRIDE 2048

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

typedef struct {
    volatile uint32_t device_feature_select;
    volatile uint32_t device_feature;
    volatile uint32_t driver_feature_select;
    volatile uint32_t driver_feature;
    volatile uint16_t config_msix_vector;
    volatile uint16_t num_queues;
    volatile uint8_t device_status;
    volatile uint8_t config_generation;
    volatile uint16_t queue_select;
    volatile uint16_t queue_size;
    volatile uint16_t queue_msix_vector;
    volatile uint16_t queue_enable;
    volatile uint16_t queue_notify_off;
    volatile uint64_t queue_desc;
    volatile uint64_t queue_driver;
    volatile uint64_t queue_device;
} virtio_common_t;

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct {
    virtq_desc_t *desc;
    volatile uint16_t *avail_idx;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_idx;
    volatile virtq_used_elem_t *used_ring;
    uint16_t size;
    uint16_t index;
    uint16_t notify_off;
    uint64_t mem_phys;
    uint64_t mem_pages;
    uint16_t last_used;
} virtio_queue_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    virtio_common_t *common;
    volatile uint8_t *notify_base;
    uint32_t notify_mult;
    volatile uint8_t *devcfg;
    volatile uint8_t *isr;
    uint8_t irq;
    virtio_queue_t rx;
    virtio_queue_t tx;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_phys;
    uint64_t rx_pages;
    uint16_t rx_posted;
    uint8_t *tx_buf;
    uint64_t tx_buf_phys;
    uint16_t *tx_pairs;
    uint16_t tx_pair_top;
    mac_addr_t mac;
    uint8_t link_up;
    netif_t *netif;
    volatile int tx_lock;
    volatile int poll_lock;
} virtio_net_dev_t;

static virtio_net_dev_t *g_dev;

extern int pt_ensure_phys_mapped(uint64_t phys_addr);

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr;
    uint32_t val;

    addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)PCI_CONFIG_DATA));
    return val;
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr;

    addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"((uint16_t)PCI_CONFIG_DATA));
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint8_t)(pci_read32(bus, slot, func, off) >> ((off & 3) * 8));
}

static uint64_t pci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar) {
    uint32_t low;
    uint32_t high;
    uint64_t addr;

    if (bar >= 6) return 0;
    low = pci_read32(bus, slot, func, (uint8_t)(0x10 + bar * 4));
    if (low & 1) return 0;
    addr = (uint64_t)(low & 0xFFFFFFF0U);
    if ((low & 6) == 4 && bar < 5) {
        high = pci_read32(bus, slot, func, (uint8_t)(0x14 + bar * 4));
        addr |= (uint64_t)high << 32;
    }
    return addr;
}

static int map_phys(uint64_t phys, uint64_t pages) {
    uint64_t i;

    for (i = 0; i < pages; i++) {
        if (pt_ensure_phys_mapped(phys + i * PAGE_SIZE) < 0) return -1;
    }
    return 0;
}

static void vnet_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

static void vnet_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

static int vnet_try_lock(volatile int *lock) {
    return __sync_lock_test_and_set(lock, 1) == 0;
}

static void vnet_notify(virtio_net_dev_t *dev, virtio_queue_t *q) {
    volatile uint16_t *addr;

    addr = (volatile uint16_t *)(dev->notify_base + (uint64_t)q->notify_off * dev->notify_mult);
    *addr = q->index;
    __asm__ __volatile__("mfence" ::: "memory");
}

static int find_device(virtio_net_dev_t *dev) {
    uint16_t bus;
    uint8_t slot;
    uint8_t func;
    uint32_t id;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                id = pci_read32((uint8_t)bus, slot, func, 0);
                if ((id & 0xFFFF) == VIRTIO_NET_VENDOR_ID &&
                    ((id >> 16) == VIRTIO_NET_DEV_MODERN || (id >> 16) == VIRTIO_NET_DEV_LEGACY)) {
                    dev->bus = (uint8_t)bus;
                    dev->slot = slot;
                    dev->func = func;
                    dev->irq = (uint8_t)(pci_read32((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                    return 0;
                }
                if (func == 0 && id == 0xFFFFFFFFU) break;
            }
        }
    }
    return -1;
}

static volatile uint8_t *map_bar_off(virtio_net_dev_t *dev, uint8_t bar, uint32_t off, uint32_t len) {
    uint64_t phys;
    uint64_t first;
    uint64_t last;
    uint64_t page;
    uint64_t base;

    phys = pci_bar(dev->bus, dev->slot, dev->func, bar);
    if (!phys || !len) return NULL;
    first = off & ~0xFFFULL;
    last = ((uint64_t)off + len + 0xFFFULL) & ~0xFFFULL;
    base = KERNEL_VMA + 0x3E000000ULL + (uint64_t)bar * 0x200000ULL;
    for (page = first; page < last; page += PAGE_SIZE) {
        vmm_map_page(base + page, phys + page, VMM_PTE_PRESENT | VMM_PTE_WRITE | VMM_PTE_PCD);
    }
    return (volatile uint8_t *)(base + off);
}

static void pci_clear_control(virtio_net_dev_t *dev, uint8_t cap, uint16_t mask) {
    uint8_t off;
    uint32_t shift;
    uint32_t val;
    uint32_t ctrl;

    off = (uint8_t)(((uint16_t)cap + 2) & ~3U);
    shift = (((uint16_t)cap + 2) & 3U) * 8U;
    val = pci_read32(dev->bus, dev->slot, dev->func, off);
    ctrl = (val >> shift) & 0xFFFFU;
    ctrl &= ~(uint32_t)mask;
    val = (val & ~(0xFFFFU << shift)) | (ctrl << shift);
    pci_write32(dev->bus, dev->slot, dev->func, off, val);
}

static int map_caps(virtio_net_dev_t *dev) {
    uint8_t ptr;
    uint8_t id;
    uint8_t next;
    uint8_t type;
    uint8_t bar;
    uint32_t off;
    uint32_t len;
    uint32_t cmd;
    uint32_t guard;
    volatile uint8_t *mapped;

    cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x00000006U;
    cmd &= ~0x00000400U;
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
    ptr = pci_read8(dev->bus, dev->slot, dev->func, 0x34) & 0xFC;
    guard = 0;
    while (ptr >= 0x40 && guard++ < 48) {
        id = pci_read8(dev->bus, dev->slot, dev->func, ptr);
        next = pci_read8(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 1)) & 0xFC;
        if (id == 0x11) {
            pci_clear_control(dev, ptr, 0xC000U);
        } else if (id == 0x05) {
            pci_clear_control(dev, ptr, 0x0001U);
        }
        if (id == 0x09) {
            type = pci_read8(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 3));
            bar = pci_read8(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 4));
            off = pci_read32(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 8));
            len = pci_read32(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 12));
            mapped = map_bar_off(dev, bar, off, len);
            if (type == VIRTIO_PCI_CAP_COMMON) {
                dev->common = (virtio_common_t *)mapped;
            } else if (type == VIRTIO_PCI_CAP_NOTIFY) {
                dev->notify_base = mapped;
                dev->notify_mult = pci_read32(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 16));
            } else if (type == VIRTIO_PCI_CAP_ISR) {
                dev->isr = mapped;
            } else if (type == VIRTIO_PCI_CAP_DEVICE) {
                dev->devcfg = mapped;
            }
        }
        if (!next || next == ptr) break;
        ptr = next;
    }
    if (!dev->common || !dev->notify_base || !dev->notify_mult || !dev->devcfg) return -1;
    return 0;
}

static int negotiate(virtio_net_dev_t *dev) {
    uint32_t lo;
    uint32_t hi;

    dev->common->device_status = 0;
    dev->common->device_status = VIRTIO_STATUS_ACK;
    dev->common->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    dev->common->device_feature_select = 0;
    lo = dev->common->device_feature;
    dev->common->device_feature_select = 1;
    hi = dev->common->device_feature;
    dev->common->driver_feature_select = 0;
    dev->common->driver_feature = lo & ((1U << VIRTIO_NET_F_MAC) | (1U << VIRTIO_NET_F_MRG_RXBUF));
    dev->common->driver_feature_select = 1;
    dev->common->driver_feature = hi & (1U << (VIRTIO_F_VERSION_1 - 32));
    dev->common->config_msix_vector = 0xFFFF;
    dev->common->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    if (!(dev->common->device_status & VIRTIO_STATUS_FEATURES_OK)) return -1;
    return 0;
}

static int setup_queue(virtio_net_dev_t *dev, virtio_queue_t *q, uint16_t idx) {
    uint64_t desc_size;
    uint64_t avail_off;
    uint64_t used_off;
    uint64_t total;
    uint8_t *base;

    dev->common->queue_select = idx;
    __asm__ __volatile__("mfence" ::: "memory");
    q->size = dev->common->queue_size;
    if (q->size < 2) return -1;
    desc_size = (uint64_t)q->size * sizeof(virtq_desc_t);
    avail_off = (desc_size + 1) & ~1ULL;
    used_off = (avail_off + 6 + (uint64_t)q->size * 2 + 3) & ~3ULL;
    total = used_off + 6 + (uint64_t)q->size * sizeof(virtq_used_elem_t);
    q->mem_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    q->mem_phys = pfa_alloc_contiguous(q->mem_pages);
    if (!q->mem_phys || map_phys(q->mem_phys, q->mem_pages) < 0) return -1;
    base = (uint8_t *)(q->mem_phys + KERNEL_VMA);
    memset(base, 0, q->mem_pages * PAGE_SIZE);
    q->desc = (virtq_desc_t *)base;
    q->avail_idx = (volatile uint16_t *)(base + avail_off + 2);
    q->avail_ring = (volatile uint16_t *)(base + avail_off + 4);
    q->used_idx = (volatile uint16_t *)(base + used_off + 2);
    q->used_ring = (volatile virtq_used_elem_t *)(base + used_off + 4);
    q->last_used = 0;
    q->index = idx;
    q->notify_off = dev->common->queue_notify_off;
    dev->common->queue_msix_vector = 0xFFFF;
    dev->common->queue_desc = q->mem_phys;
    dev->common->queue_driver = q->mem_phys + avail_off;
    dev->common->queue_device = q->mem_phys + used_off;
    dev->common->queue_enable = 1;
    return 0;
}

static void release_dev(virtio_net_dev_t *dev) {
    if (!dev) return;
    if (dev->common) dev->common->device_status = 0;
    if (dev->rx.mem_phys) pfa_free_contiguous(dev->rx.mem_phys, dev->rx.mem_pages);
    if (dev->tx.mem_phys) pfa_free_contiguous(dev->tx.mem_phys, dev->tx.mem_pages);
    if (dev->rx_bufs_phys) pfa_free_contiguous(dev->rx_bufs_phys, dev->rx_pages);
    if (dev->tx_buf_phys) pfa_free(dev->tx_buf_phys);
    if (dev->tx_pairs) kfree(dev->tx_pairs);
    if (dev->netif) kfree(dev->netif);
    if (g_dev == dev) g_dev = NULL;
    kfree(dev);
}

static void rx_post(virtio_net_dev_t *dev, uint16_t d) {
    virtio_queue_t *q;
    uint16_t idx;

    q = &dev->rx;
    q->desc[d].addr = dev->rx_bufs_phys + (uint64_t)d * VIRTIO_NET_RX_STRIDE;
    q->desc[d].len = VIRTIO_NET_RX_STRIDE;
    q->desc[d].flags = VIRTQ_DESC_F_WRITE;
    q->desc[d].next = 0;
    idx = *q->avail_idx;
    q->avail_ring[idx % q->size] = d;
    __asm__ __volatile__("mfence" ::: "memory");
    *q->avail_idx = (uint16_t)(idx + 1);
}

static void tx_reap(virtio_net_dev_t *dev) {
    virtio_queue_t *q;
    uint16_t head;

    q = &dev->tx;
    while (dev->tx.last_used != *q->used_idx) {
        head = q->used_ring[dev->tx.last_used % q->size].id;
        dev->tx_pairs[dev->tx_pair_top++] = (uint16_t)(head / 2);
        dev->tx.last_used++;
    }
}

int virtio_net_send(netif_t *netif, uint8_t *data, uint64_t len) {
    virtio_net_dev_t *dev;
    virtio_queue_t *q;
    uint16_t pair;
    uint16_t d0;
    uint16_t d1;
    uint16_t idx;

    dev = (virtio_net_dev_t *)netif->driver_data;
    if (!dev || len > ETH_FRAME_MAX) return -1;
    q = &dev->tx;
    vnet_lock(&dev->tx_lock);
    tx_reap(dev);
    if (!dev->tx_pair_top) {
        vnet_unlock(&dev->tx_lock);
        return -1;
    }
    pair = dev->tx_pairs[--dev->tx_pair_top];
    d0 = (uint16_t)(pair * 2);
    d1 = (uint16_t)(d0 + 1);
    memset(dev->tx_buf, 0, VIRTIO_NET_HDR_LEN);
    memcpy(dev->tx_buf + VIRTIO_NET_HDR_LEN, data, len);
    q->desc[d0].addr = dev->tx_buf_phys;
    q->desc[d0].len = VIRTIO_NET_HDR_LEN;
    q->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[d0].next = d1;
    q->desc[d1].addr = dev->tx_buf_phys + VIRTIO_NET_HDR_LEN;
    q->desc[d1].len = (uint32_t)len;
    q->desc[d1].flags = 0;
    q->desc[d1].next = 0;
    idx = *q->avail_idx;
    q->avail_ring[idx % q->size] = d0;
    __asm__ __volatile__("mfence" ::: "memory");
    *q->avail_idx = (uint16_t)(idx + 1);
    __asm__ __volatile__("mfence" ::: "memory");
    vnet_notify(dev, q);
    vnet_unlock(&dev->tx_lock);
    return 0;
}

int virtio_net_poll(netif_t *netif) {
    virtio_net_dev_t *dev;
    virtio_queue_t *q;
    virtq_used_elem_t e;
    uint8_t *buf;
    int count;
    int reposted;

    dev = (virtio_net_dev_t *)netif->driver_data;
    if (!dev) return 0;
    if (!vnet_try_lock(&dev->poll_lock)) return 0;
    vnet_lock(&dev->tx_lock);
    tx_reap(dev);
    vnet_unlock(&dev->tx_lock);
    q = &dev->rx;
    count = 0;
    reposted = 0;
    while (dev->rx.last_used != *q->used_idx) {
        e = q->used_ring[dev->rx.last_used % q->size];
        dev->rx.last_used++;
        if (e.id < dev->rx_posted && e.len > VIRTIO_NET_HDR_LEN && e.len <= VIRTIO_NET_RX_STRIDE) {
            buf = dev->rx_bufs + (uint64_t)e.id * VIRTIO_NET_RX_STRIDE;
            eth_receive(netif, buf + VIRTIO_NET_HDR_LEN, (uint64_t)e.len - VIRTIO_NET_HDR_LEN);
            count++;
        }
        if (e.id < dev->rx_posted) {
            rx_post(dev, (uint16_t)e.id);
            reposted = 1;
        }
    }
    if (reposted) vnet_notify(dev, q);
    __asm__ __volatile__("mfence" ::: "memory");
    vnet_unlock(&dev->poll_lock);
    return count;
}

void virtio_net_irq_handler(void *regs) {
    virtio_net_dev_t *dev;
    uint8_t isr;

    (void)regs;
    dev = g_dev;
    if (!dev || !dev->isr) return;
    isr = *dev->isr;
    if (!isr) return;
    if (isr & 1)
        descriptor_ready_notify_irq();
}

static int virtio_net_bring_up(virtio_net_dev_t *dev) {
    netif_t *netif;
    char name[16];
    uint64_t n;
    uint16_t i;
    uint16_t pairs;

    if (map_caps(dev) < 0 || negotiate(dev) < 0) {
        kfree(dev);
        return -1;
    }
    memcpy(&dev->mac, (const void *)dev->devcfg, ETH_ALEN);
    dev->link_up = 1;
    if (setup_queue(dev, &dev->rx, 0) < 0 || setup_queue(dev, &dev->tx, 1) < 0) {
        release_dev(dev);
        return -1;
    }
    dev->rx_posted = dev->rx.size < VIRTIO_NET_RX_POSTED ? dev->rx.size : VIRTIO_NET_RX_POSTED;
    n = (uint64_t)dev->rx_posted * VIRTIO_NET_RX_STRIDE;
    dev->rx_pages = (n + PAGE_SIZE - 1) / PAGE_SIZE;
    dev->rx_bufs_phys = pfa_alloc_contiguous(dev->rx_pages);
    if (!dev->rx_bufs_phys || map_phys(dev->rx_bufs_phys, dev->rx_pages) < 0) {
        release_dev(dev);
        return -1;
    }
    dev->rx_bufs = (uint8_t *)(dev->rx_bufs_phys + KERNEL_VMA);
    memset(dev->rx_bufs, 0, dev->rx_pages * PAGE_SIZE);
    dev->tx_buf_phys = pfa_alloc();
    if (!dev->tx_buf_phys || map_phys(dev->tx_buf_phys, 1) < 0) {
        release_dev(dev);
        return -1;
    }
    dev->tx_buf = (uint8_t *)(dev->tx_buf_phys + KERNEL_VMA);
    memset(dev->tx_buf, 0, PAGE_SIZE);
    pairs = (uint16_t)(dev->tx.size / 2);
    dev->tx_pairs = (uint16_t *)kmalloc((uint64_t)pairs * sizeof(uint16_t));
    if (!dev->tx_pairs) {
        release_dev(dev);
        return -1;
    }
    for (i = 0; i < pairs; i++) dev->tx_pairs[i] = (uint16_t)(pairs - 1 - i);
    dev->tx_pair_top = pairs;
    dev->tx.last_used = 0;
    dev->rx.last_used = *dev->rx.used_idx;
    __asm__ __volatile__("mfence" ::: "memory");
    dev->common->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                                 VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
    for (i = 0; i < dev->rx_posted; i++) rx_post(dev, i);
    __asm__ __volatile__("mfence" ::: "memory");
    vnet_notify(dev, &dev->rx);
    netif = netif_alloc();
    if (!netif) {
        release_dev(dev);
        return -1;
    }
    memset(name, 0, sizeof(name));
    for (i = 0; i < 16; i++) {
        name[0] = 'e';
        name[1] = 't';
        name[2] = 'h';
        name[3] = (char)('0' + i / 10);
        name[4] = (char)('0' + i % 10);
        name[5] = '\0';
        if (i < 10) {
            name[3] = (char)('0' + i);
            name[4] = '\0';
        }
        if (!netif_find(name)) break;
    }
    if (i == 16) {
        kfree(netif);
        release_dev(dev);
        return -1;
    }
    memcpy(netif->name, name, sizeof(name));
    memcpy(&netif->mac, &dev->mac, sizeof(mac_addr_t));
    netif->mtu = ETH_MTU;
    netif->link_up = dev->link_up;
    netif->send = virtio_net_send;
    netif->poll = virtio_net_poll;
    netif->driver_data = dev;
    dev->netif = netif;
    netif_register(netif);
    netif_set_default(netif);
    g_dev = dev;
    if (dev->isr && dev->irq > 0 && dev->irq < 16) {
        irq_register_handler(dev->irq, (irq_handler_t)virtio_net_irq_handler);
        irq_unmask(dev->irq);
        printf("VIRTIO-NET: Interrupts enabled (IRQ %u)\n", dev->irq);
    }
    printf("VIRTIO-NET: %s MAC %02X:%02X:%02X:%02X:%02X:%02X link %s\n", netif->name,
           dev->mac.addr[0], dev->mac.addr[1], dev->mac.addr[2],
           dev->mac.addr[3], dev->mac.addr[4], dev->mac.addr[5],
           dev->link_up ? "UP" : "DOWN");
    return 0;
}

int virtio_net_init(void) {
    virtio_net_dev_t *dev;

    if (g_dev) return 0;
    dev = (virtio_net_dev_t *)kmalloc(sizeof(virtio_net_dev_t));
    if (!dev) return -1;
    memset(dev, 0, sizeof(virtio_net_dev_t));
    if (find_device(dev) < 0) {
        kfree(dev);
        return -1;
    }
    return virtio_net_bring_up(dev);
}

int virtio_net_init_at(uint8_t bus, uint8_t slot, uint8_t func) {
    virtio_net_dev_t *dev;
    uint32_t id;

    if (g_dev) return 0;
    dev = (virtio_net_dev_t *)kmalloc(sizeof(virtio_net_dev_t));
    if (!dev) return -1;
    memset(dev, 0, sizeof(virtio_net_dev_t));
    id = pci_read32(bus, slot, func, 0);
    if ((id & 0xFFFF) != VIRTIO_NET_VENDOR_ID ||
        ((id >> 16) != VIRTIO_NET_DEV_MODERN && (id >> 16) != VIRTIO_NET_DEV_LEGACY)) {
        kfree(dev);
        return -1;
    }
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->irq = (uint8_t)(pci_read32(bus, slot, func, 0x3C) & 0xFF);
    return virtio_net_bring_up(dev);
}

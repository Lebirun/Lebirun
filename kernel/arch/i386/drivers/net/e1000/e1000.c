#include <kernel/drivers/net/e1000/e1000.h>
#include <kernel/drivers/net/net.h>
#include <kernel/drivers/net/ethernet.h>
#include <kernel/io.h>
#include <kernel/mem_map.h>
#include <kernel/idt.h>
#include <kernel/tty.h>
#include <string.h>

static e1000_device_t g_e1000_dev;

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

static inline uint32_t e1000_read(e1000_device_t *dev, uint32_t reg) {
    if (dev->bar_type == 0) {
        return *((volatile uint32_t *)(dev->bar0_virt + reg));
    } else {
        outl(dev->io_base, reg);
        return inl(dev->io_base + 4);
    }
}

static inline void e1000_write(e1000_device_t *dev, uint32_t reg, uint32_t value) {
    if (dev->bar_type == 0) {
        *((volatile uint32_t *)(dev->bar0_virt + reg)) = value;
    } else {
        outl(dev->io_base, reg);
        outl(dev->io_base + 4, value);
    }
}

static uint16_t e1000_read_eeprom(e1000_device_t *dev, uint8_t addr) {
    uint32_t tmp;
    e1000_write(dev, E1000_EERD, (1) | ((uint32_t)(addr) << 8));
    while (!((tmp = e1000_read(dev, E1000_EERD)) & (1 << 4)));
    return (uint16_t)((tmp >> 16) & 0xFFFF);
}

void e1000_read_mac(e1000_device_t *dev) {
    uint32_t tmp;
    tmp = e1000_read(dev, E1000_RAL);
    if (tmp != 0 && tmp != 0xFFFFFFFF) {
        dev->mac.addr[0] = tmp & 0xFF;
        dev->mac.addr[1] = (tmp >> 8) & 0xFF;
        dev->mac.addr[2] = (tmp >> 16) & 0xFF;
        dev->mac.addr[3] = (tmp >> 24) & 0xFF;
        tmp = e1000_read(dev, E1000_RAH);
        dev->mac.addr[4] = tmp & 0xFF;
        dev->mac.addr[5] = (tmp >> 8) & 0xFF;
        return;
    }

    uint16_t word;
    word = e1000_read_eeprom(dev, 0);
    dev->mac.addr[0] = word & 0xFF;
    dev->mac.addr[1] = (word >> 8) & 0xFF;
    word = e1000_read_eeprom(dev, 1);
    dev->mac.addr[2] = word & 0xFF;
    dev->mac.addr[3] = (word >> 8) & 0xFF;
    word = e1000_read_eeprom(dev, 2);
    dev->mac.addr[4] = word & 0xFF;
    dev->mac.addr[5] = (word >> 8) & 0xFF;
}

static void e1000_reset(e1000_device_t *dev) {
    uint32_t ctrl = e1000_read(dev, E1000_CTRL);
    e1000_write(dev, E1000_CTRL, ctrl | E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++);
    while (e1000_read(dev, E1000_CTRL) & E1000_CTRL_RST);
}

static void e1000_linkup(e1000_device_t *dev) {
    uint32_t ctrl = e1000_read(dev, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_ILOS | E1000_CTRL_PHY_RST);
    e1000_write(dev, E1000_CTRL, ctrl);
}

static int e1000_init_rx(e1000_device_t *dev) {
    uint32_t rx_ring_phys = pfa_alloc();
    if (!rx_ring_phys) return -1;

    uint32_t rx_ring_virt = 0xFD000000;
    vmm_map_page(rx_ring_virt, rx_ring_phys, 0x003);
    memset((void *)rx_ring_virt, 0, PAGE_SIZE);

    dev->rx_descs = (e1000_rx_desc_t *)rx_ring_virt;
    dev->rx_descs_phys = rx_ring_phys;

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint32_t buf_phys = pfa_alloc();
        if (!buf_phys) return -1;

        uint32_t buf_virt = 0xFD100000 + i * PAGE_SIZE;
        vmm_map_page(buf_virt, buf_phys, 0x003);
        memset((void *)buf_virt, 0, PAGE_SIZE);

        dev->rx_buffers[i] = (uint8_t *)buf_virt;
        dev->rx_buffers_phys[i] = buf_phys;

        dev->rx_descs[i].buffer_addr = buf_phys;
        dev->rx_descs[i].status = 0;
    }

    e1000_write(dev, E1000_RDBAL, rx_ring_phys);
    e1000_write(dev, E1000_RDBAH, 0);
    e1000_write(dev, E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write(dev, E1000_RDH, 0);
    e1000_write(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);

    dev->rx_cur = 0;

    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE |
                    E1000_RCTL_MPE | E1000_RCTL_LBM_NONE |
                    E1000_RCTL_RDMTS_HALF | E1000_RCTL_BAM |
                    E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048;
    e1000_write(dev, E1000_RCTL, rctl);

    return 0;
}

static int e1000_init_tx(e1000_device_t *dev) {
    uint32_t tx_ring_phys = pfa_alloc();
    if (!tx_ring_phys) return -1;

    uint32_t tx_ring_virt = 0xFD200000;
    vmm_map_page(tx_ring_virt, tx_ring_phys, 0x003);
    memset((void *)tx_ring_virt, 0, PAGE_SIZE);

    dev->tx_descs = (e1000_tx_desc_t *)tx_ring_virt;
    dev->tx_descs_phys = tx_ring_phys;

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint32_t buf_phys = pfa_alloc();
        if (!buf_phys) return -1;

        uint32_t buf_virt = 0xFD300000 + i * PAGE_SIZE;
        vmm_map_page(buf_virt, buf_phys, 0x003);
        memset((void *)buf_virt, 0, PAGE_SIZE);

        dev->tx_buffers[i] = (uint8_t *)buf_virt;
        dev->tx_buffers_phys[i] = buf_phys;

        dev->tx_descs[i].buffer_addr = buf_phys;
        dev->tx_descs[i].status = E1000_TXD_STAT_DD;
        dev->tx_descs[i].cmd = 0;
    }

    e1000_write(dev, E1000_TDBAL, tx_ring_phys);
    e1000_write(dev, E1000_TDBAH, 0);
    e1000_write(dev, E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write(dev, E1000_TDH, 0);
    e1000_write(dev, E1000_TDT, 0);

    dev->tx_cur = 0;

    e1000_write(dev, E1000_TIPG, E1000_TIPG_IPGT | E1000_TIPG_IPGR1 | E1000_TIPG_IPGR2);

    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (15 << E1000_TCTL_CT_SHIFT) |
                    (64 << E1000_TCTL_COLD_SHIFT) |
                    E1000_TCTL_RTLC;
    e1000_write(dev, E1000_TCTL, tctl);

    return 0;
}

void e1000_enable_interrupts(e1000_device_t *dev) {
    e1000_write(dev, E1000_IMS, E1000_IMS_LSC | E1000_IMS_RXT0 |
                                E1000_IMS_RXDMT0 | E1000_IMS_RXO);
    e1000_read(dev, E1000_ICR);
}

void e1000_disable_interrupts(e1000_device_t *dev) {
    e1000_write(dev, E1000_IMC, 0xFFFFFFFF);
    e1000_read(dev, E1000_ICR);
}

int e1000_send(netif_t *netif, uint8_t *data, uint32_t len) {
    e1000_device_t *dev = (e1000_device_t *)netif->driver_data;
    if (!dev || !dev->initialized) return -1;
    if (len > ETH_FRAME_MAX) return -1;

    uint32_t cur = dev->tx_cur;

    while (!(dev->tx_descs[cur].status & E1000_TXD_STAT_DD)) {
        for (volatile int i = 0; i < 1000; i++);
    }

    memcpy(dev->tx_buffers[cur], data, len);

    dev->tx_descs[cur].length = len;
    dev->tx_descs[cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    dev->tx_descs[cur].status = 0;

    dev->tx_cur = (cur + 1) % E1000_NUM_TX_DESC;
    e1000_write(dev, E1000_TDT, dev->tx_cur);

    dev->packets_tx++;
    dev->bytes_tx += len;

    return 0;
}

int e1000_poll(netif_t *netif) {
    e1000_device_t *dev = (e1000_device_t *)netif->driver_data;
    if (!dev || !dev->initialized) return 0;

    int count = 0;

    while (dev->rx_descs[dev->rx_cur].status & E1000_RXD_STAT_DD) {
        uint16_t len = dev->rx_descs[dev->rx_cur].length;
        uint8_t *buf = dev->rx_buffers[dev->rx_cur];

        if (dev->rx_descs[dev->rx_cur].errors) {
            dev->errors_rx++;
        } else {
            dev->packets_rx++;
            dev->bytes_rx += len;
            eth_receive(netif, buf, len);
        }

        dev->rx_descs[dev->rx_cur].status = 0;

        uint32_t old_cur = dev->rx_cur;
        dev->rx_cur = (dev->rx_cur + 1) % E1000_NUM_RX_DESC;
        e1000_write(dev, E1000_RDT, old_cur);

        count++;
    }

    return count;
}

void e1000_irq_handler(void *regs) {
    (void)regs;
    e1000_device_t *dev = &g_e1000_dev;
    if (!dev->initialized) return;

    uint32_t icr = e1000_read(dev, E1000_ICR);

    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000_read(dev, E1000_STATUS);
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        if (dev->netif) {
            dev->netif->link_up = dev->link_up;
        }
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
        if (dev->netif) {
            e1000_poll(dev->netif);
        }
    }
}

int e1000_probe(void) {
    printf("E1000: Probing PCI bus...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;

                if (vendor != E1000_VENDOR_ID) continue;

                if (device == E1000_DEVICE_82540EM ||
                    device == E1000_DEVICE_82545EM ||
                    device == E1000_DEVICE_82574L) {

                    printf("E1000: Found device at PCI %u:%u.%u (VID: 0x%04X, DID: 0x%04X)\n",
                           bus, slot, func, vendor, device);

                    g_e1000_dev.pci_bus = bus;
                    g_e1000_dev.pci_slot = slot;
                    g_e1000_dev.pci_func = func;
                    g_e1000_dev.device_id = device;

                    uint32_t bar0 = pci_read_config(bus, slot, func, 0x10);
                    g_e1000_dev.bar_type = bar0 & 1;

                    if (g_e1000_dev.bar_type == 0) {
                        g_e1000_dev.bar0 = bar0 & 0xFFFFFFF0;
                    } else {
                        g_e1000_dev.io_base = bar0 & 0xFFFFFFFC;
                    }

                    uint32_t cmd = pci_read_config(bus, slot, func, 0x04);
                    cmd |= (1 << 1) | (1 << 2);
                    pci_write_config(bus, slot, func, 0x04, cmd);

                    g_e1000_dev.irq = pci_read_config(bus, slot, func, 0x3C) & 0xFF;

                    return 0;
                }
            }
        }
    }

    printf("E1000: No supported device found\n");
    return -1;
}

e1000_device_t *e1000_get_device(void) {
    return g_e1000_dev.initialized ? &g_e1000_dev : NULL;
}

void e1000_print_status(e1000_device_t *dev) {
    printf("=== E1000 Status ===\n");
    printf("PCI: %u:%u.%u\n", dev->pci_bus, dev->pci_slot, dev->pci_func);
    printf("Device ID: 0x%04X\n", dev->device_id);
    printf("BAR0: 0x%08X (type %u)\n", dev->bar0, dev->bar_type);
    printf("IRQ: %u\n", dev->irq);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           dev->mac.addr[0], dev->mac.addr[1], dev->mac.addr[2],
           dev->mac.addr[3], dev->mac.addr[4], dev->mac.addr[5]);
    printf("Link: %s\n", dev->link_up ? "UP" : "DOWN");
    printf("RX: %u packets, %u bytes, %u errors\n",
           dev->packets_rx, dev->bytes_rx, dev->errors_rx);
    printf("TX: %u packets, %u bytes, %u errors\n",
           dev->packets_tx, dev->bytes_tx, dev->errors_tx);
}

int e1000_init(void) {
    printf("E1000: Initializing driver...\n");

    memset(&g_e1000_dev, 0, sizeof(g_e1000_dev));

    if (e1000_probe() < 0) {
        return -1;
    }

    if (g_e1000_dev.bar_type == 0) {
        uint32_t bar0_phys = g_e1000_dev.bar0;
        uint32_t bar0_virt = 0xFC000000;

        for (uint32_t off = 0; off < 0x20000; off += PAGE_SIZE) {
            vmm_map_page(bar0_virt + off, bar0_phys + off, 0x003);
        }

        g_e1000_dev.bar0_virt = bar0_virt;
    }

    e1000_reset(&g_e1000_dev);

    for (int i = 0; i < 128; i++) {
        e1000_write(&g_e1000_dev, E1000_MTA + i * 4, 0);
    }

    e1000_read_mac(&g_e1000_dev);
    printf("E1000: MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
           g_e1000_dev.mac.addr[0], g_e1000_dev.mac.addr[1],
           g_e1000_dev.mac.addr[2], g_e1000_dev.mac.addr[3],
           g_e1000_dev.mac.addr[4], g_e1000_dev.mac.addr[5]);

    e1000_linkup(&g_e1000_dev);

    if (e1000_init_rx(&g_e1000_dev) < 0) {
        printf("E1000: Failed to initialize RX\n");
        return -1;
    }

    if (e1000_init_tx(&g_e1000_dev) < 0) {
        printf("E1000: Failed to initialize TX\n");
        return -1;
    }

    uint32_t status = e1000_read(&g_e1000_dev, E1000_STATUS);
    g_e1000_dev.link_up = (status & E1000_STATUS_LU) ? 1 : 0;
    printf("E1000: Link is %s\n", g_e1000_dev.link_up ? "UP" : "DOWN");

    netif_t *netif = netif_alloc();
    if (!netif) {
        printf("E1000: Failed to allocate network interface\n");
        return -1;
    }

    memcpy(netif->name, "eth0", 5);
    memcpy(&netif->mac, &g_e1000_dev.mac, sizeof(mac_addr_t));
    netif->mtu = ETH_MTU;
    netif->link_up = g_e1000_dev.link_up;
    netif->send = e1000_send;
    netif->poll = e1000_poll;
    netif->driver_data = &g_e1000_dev;
    g_e1000_dev.netif = netif;

    netif_register(netif);
    netif_set_default(netif);

    if (g_e1000_dev.irq > 0 && g_e1000_dev.irq < 16) {
        irq_register_handler(g_e1000_dev.irq, (irq_handler_t)e1000_irq_handler);
        irq_unmask(g_e1000_dev.irq);
        e1000_enable_interrupts(&g_e1000_dev);
        printf("E1000: Interrupts enabled (IRQ %u)\n", g_e1000_dev.irq);
    }

    g_e1000_dev.initialized = 1;
    printf("E1000: Initialization complete\n");

    return 0;
}

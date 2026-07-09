#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/io.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/tty.h>
#include <lebirun/idt.h>
#include <lebirun/pit.h>
#include <string.h>
#include <stddef.h>

static ahci_controller_t g_ahci_controller;

static uint64_t ahci_required_port_capacity(uint64_t ports_impl) {
    uint64_t i;
    uint64_t capacity;

    capacity = 0;
    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ports_impl & (1 << i)) capacity = i + 1;
    }
    if (capacity == 0) capacity = 1;
    return capacity;
}

static ahci_port_t *ahci_port_slot(uint64_t index) {
    if (!g_ahci_controller.ports) return NULL;
    if (index >= g_ahci_controller.ports_capacity) return NULL;
    return &g_ahci_controller.ports[index];
}

static inline uint64_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint64_t address = (uint64_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outb(PCI_CONFIG_ADDRESS, address & 0xFF);
    outb(PCI_CONFIG_ADDRESS + 1, (address >> 8) & 0xFF);
    outb(PCI_CONFIG_ADDRESS + 2, (address >> 16) & 0xFF);
    outb(PCI_CONFIG_ADDRESS + 3, (address >> 24) & 0xFF);
    
    uint64_t data = 0;
    data = inb(PCI_CONFIG_DATA);
    data |= (uint64_t)inb(PCI_CONFIG_DATA + 1) << 8;
    data |= (uint64_t)inb(PCI_CONFIG_DATA + 2) << 16;
    data |= (uint64_t)inb(PCI_CONFIG_DATA + 3) << 24;
    return data;
}

static inline void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint64_t value) {
    uint64_t address = (uint64_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outb(PCI_CONFIG_ADDRESS, address & 0xFF);
    outb(PCI_CONFIG_ADDRESS + 1, (address >> 8) & 0xFF);
    outb(PCI_CONFIG_ADDRESS + 2, (address >> 16) & 0xFF);
    outb(PCI_CONFIG_ADDRESS + 3, (address >> 24) & 0xFF);
    
    outb(PCI_CONFIG_DATA, value & 0xFF);
    outb(PCI_CONFIG_DATA + 1, (value >> 8) & 0xFF);
    outb(PCI_CONFIG_DATA + 2, (value >> 16) & 0xFF);
    outb(PCI_CONFIG_DATA + 3, (value >> 24) & 0xFF);
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint64_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint64_t address = (uint64_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint64_t value) {
    uint64_t address = (uint64_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static inline uint32_t mmio_read32(uint64_t addr) {
    return *((volatile uint32_t *)addr);
}

static inline void mmio_write32(uint64_t addr, uint32_t value) {
    *((volatile uint32_t *)addr) = value;
}

static inline uint64_t ahci_hba_read(uint64_t base, uint64_t reg) {
    return mmio_read32(base + reg);
}

static inline void ahci_hba_write(uint64_t base, uint64_t reg, uint64_t value) {
    mmio_write32(base + reg, value);
}

static inline uint64_t ahci_port_read(ahci_port_t *port, uint64_t reg) {
    return mmio_read32(port->port_base + reg);
}

static inline void ahci_port_write(ahci_port_t *port, uint64_t reg, uint64_t value) {
    mmio_write32(port->port_base + reg, value);
}

static void ahci_delay(uint64_t ms) {
    volatile uint64_t count = ms * 10000;
    while (count--) {
        __asm__ volatile("nop");
    }
}

static void ahci_wait_delay(void) {
    int i;

    for (i = 0; i < 64; i++) {
        __asm__ volatile("pause");
    }
}

static ahci_dev_type_t ahci_check_type(ahci_port_t *port) {
    uint64_t ssts = ahci_port_read(port, AHCI_PxSSTS);
    uint8_t ipm = (ssts >> AHCI_PxSSTS_IPM_SHIFT) & 0x0F;
    uint8_t det = ssts & AHCI_PxSSTS_DET_MASK;

    if (det != AHCI_PxSSTS_DET_READY)
        return AHCI_DEV_NULL;
    if (ipm != AHCI_PxSSTS_IPM_ACTIVE)
        return AHCI_DEV_NULL;

    uint64_t sig = ahci_port_read(port, AHCI_PxSIG);
    switch (sig) {
        case SATA_SIG_ATAPI:
            return AHCI_DEV_SATAPI;
        case SATA_SIG_SEMB:
            return AHCI_DEV_SEMB;
        case SATA_SIG_PM:
            return AHCI_DEV_PM;
        default:
            return AHCI_DEV_SATA;
    }
}

static void ahci_stop_cmd(ahci_port_t *port) {
    uint64_t cmd = ahci_port_read(port, AHCI_PxCMD);
    
    cmd &= ~AHCI_PxCMD_ST;
    ahci_port_write(port, AHCI_PxCMD, cmd);
    
    int timeout = 500;
    while (timeout-- > 0) {
        cmd = ahci_port_read(port, AHCI_PxCMD);
        if (!(cmd & AHCI_PxCMD_CR))
            break;
        ahci_wait_delay();
    }
    
    cmd = ahci_port_read(port, AHCI_PxCMD);
    cmd &= ~AHCI_PxCMD_FRE;
    ahci_port_write(port, AHCI_PxCMD, cmd);
    
    timeout = 500;
    while (timeout-- > 0) {
        cmd = ahci_port_read(port, AHCI_PxCMD);
        if (!(cmd & AHCI_PxCMD_FR))
            break;
        ahci_delay(1);
    }
}

static void ahci_start_cmd(ahci_port_t *port) {
    uint64_t cmd = ahci_port_read(port, AHCI_PxCMD);
    
    int timeout = 500;
    while (timeout-- > 0) {
        if (!(cmd & AHCI_PxCMD_CR))
            break;
        ahci_delay(1);
        cmd = ahci_port_read(port, AHCI_PxCMD);
    }
    
    cmd |= AHCI_PxCMD_FRE;
    ahci_port_write(port, AHCI_PxCMD, cmd);
    
    cmd |= AHCI_PxCMD_ST;
    ahci_port_write(port, AHCI_PxCMD, cmd);
}

static int ahci_find_slot(ahci_port_t *port) {
    uint64_t slots = ahci_port_read(port, AHCI_PxSACT) | ahci_port_read(port, AHCI_PxCI);
    for (uint64_t i = 0; i < g_ahci_controller.num_cmd_slots; i++) {
        if ((slots & (1 << i)) == 0)
            return i;
    }
    return -1;
}

static int ahci_wait_cmd(ahci_port_t *port, int slot, uint64_t timeout_ms) {
    uint64_t start_ticks;
    uint64_t timeout_ticks;
    uint64_t max_spins;
    uint64_t spins;
    uint64_t ci;
    uint64_t is;

    start_ticks = tick_count;
    timeout_ticks = 0;
    if (pit_freq > 0) {
        timeout_ticks = (timeout_ms * pit_freq + 999) / 1000;
        if (timeout_ticks == 0) timeout_ticks = 1;
    }
    max_spins = timeout_ms * 10000;
    if (max_spins < 10000) max_spins = 10000;
    spins = 0;

    for (;;) {
        ci = ahci_port_read(port, AHCI_PxCI);
        if ((ci & (1 << slot)) == 0)
            return 0;
        
        is = ahci_port_read(port, AHCI_PxIS);
        if (is & (1 << 30)) { 
            printf("AHCI: Task file error on port %u\n", port->port_num);
            return -1;
        }

        if (timeout_ticks > 0 && tick_count - start_ticks >= timeout_ticks) {
            break;
        }
        if (++spins >= max_spins) {
            break;
        }
        
        ahci_wait_delay();
    }
    
    printf("AHCI: Command timeout on port %u\n", port->port_num);
    return -1;
}

int ahci_probe(void) {
    uint16_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t max_func;
    uint64_t vendor_device;
    uint16_t vendor;
    uint16_t device_id;
    uint64_t class_info;
    uint8_t base_class;
    uint8_t sub_class;
    uint8_t prog_if;
    uint64_t header_type;
    uint64_t cmd;

    printf("AHCI: Probing PCI bus for AHCI controllers...\n");

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            vendor_device = pci_read_config(bus, slot, 0, 0x00);
            vendor = vendor_device & 0xFFFF;

            if (vendor == 0xFFFF)
                continue;

            header_type = pci_read_config(bus, slot, 0, 0x0C);
            max_func = ((header_type >> 16) & 0x80) ? 8 : 1;

            for (func = 0; func < max_func; func++) {
                if (func > 0) {
                    vendor_device = pci_read_config(bus, slot, func, 0x00);
                    vendor = vendor_device & 0xFFFF;

                    if (vendor == 0xFFFF)
                        continue;
                }

                class_info = pci_read_config(bus, slot, func, 0x08);
                base_class = (class_info >> 24) & 0xFF;
                sub_class = (class_info >> 16) & 0xFF;
                prog_if = (class_info >> 8) & 0xFF;

                if (base_class == PCI_CLASS_STORAGE &&
                    sub_class == PCI_SUBCLASS_SATA &&
                    prog_if == PCI_PROGIF_AHCI) {

                    device_id = (vendor_device >> 16) & 0xFFFF;
                    printf("AHCI: Found controller at PCI %u:%u.%u (VID: 0x%04X, DID: 0x%04X)\n",
                           bus, slot, func, vendor, device_id);

                    g_ahci_controller.pci_bus = bus;
                    g_ahci_controller.pci_slot = slot;
                    g_ahci_controller.pci_func = func;

                    g_ahci_controller.abar = pci_read_config(bus, slot, func, 0x24) & 0xFFFFFFF0;
                    printf("AHCI: ABAR = 0x%016lX\n", g_ahci_controller.abar);

                    cmd = pci_read_config(bus, slot, func, 0x04);
                    cmd |= (1 << 1) | (1 << 2);
                    pci_write_config(bus, slot, func, 0x04, cmd);

                    return 0;
                }
            }
        }
    }

    printf("AHCI: No AHCI controller found\n");
    return -1;
}

int ahci_port_init(ahci_port_t *port) {
    uint64_t page_phys;
    uint64_t page_virt;
    uint64_t cmd_list_off;
    uint64_t fis_off;
    uint64_t cmd_table_off;
    int i;

    printf("AHCI: Initializing port %u...\n", port->port_num);

    ahci_stop_cmd(port);
    printf("AHCI: port %u stop_cmd done\n", port->port_num);

    page_phys = pfa_alloc();
    if (!page_phys) {
        printf("AHCI: Failed to allocate page for port %u\n", port->port_num);
        return -1;
    }

    page_virt = page_phys + KERNEL_VMA;
    vmm_map_page(page_virt, page_phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);

    cmd_list_off = 0;
    fis_off = AHCI_CMD_SLOTS * sizeof(hba_cmd_header_t);
    fis_off = (fis_off + 255) & ~255ULL;
    cmd_table_off = fis_off + sizeof(hba_fis_t);
    cmd_table_off = (cmd_table_off + 127) & ~127ULL;

    printf("AHCI: port %u layout: cmd_list@0x%lX fis@0x%lX tbl@0x%lX\n",
           port->port_num, cmd_list_off, fis_off, cmd_table_off);

    port->cmd_list = (hba_cmd_header_t *)(page_virt + cmd_list_off);
    port->fis = (hba_fis_t *)(page_virt + fis_off);
    port->cmd_table = (hba_cmd_table_t *)(page_virt + cmd_table_off);
    port->cmd_list_phys = page_phys + cmd_list_off;
    port->fis_phys = page_phys + fis_off;
    port->cmd_table_phys = page_phys + cmd_table_off;

    ahci_port_write(port, AHCI_PxCLB, port->cmd_list_phys);
    ahci_port_write(port, AHCI_PxCLBU, 0);

    ahci_port_write(port, AHCI_PxFB, port->fis_phys);
    ahci_port_write(port, AHCI_PxFBU, 0);
    printf("AHCI: port %u port regs written\n", port->port_num);

    for (i = 0; i < AHCI_CMD_SLOTS; i++) {
        port->cmd_list[i].prdtl = AHCI_PRDT_ENTRIES;
        port->cmd_list[i].ctba = (uint32_t)(port->cmd_table_phys + (i * sizeof(hba_cmd_table_t)));
        port->cmd_list[i].ctbau = (uint32_t)((port->cmd_table_phys + (i * sizeof(hba_cmd_table_t))) >> 32);
    }

    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    ahci_port_write(port, AHCI_PxSERR, 0xFFFFFFFF);

    ahci_start_cmd(port);

    printf("AHCI: Port %u initialized (single page at 0x%016lX)\n",
           port->port_num, page_phys);

    return 0;
}

int ahci_identify(ahci_port_t *port) {
    printf("AHCI: Identifying device on port %u...\n", port->port_num);
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0) {
        printf("AHCI: No free command slot\n");
        return -1;
    }
    
    uint64_t buf_phys = pfa_alloc();
    if (!buf_phys) {
        printf("AHCI: Failed to allocate identify buffer\n");
        return -1;
    }
    
    uint64_t buf_virt = buf_phys + KERNEL_VMA;
    memset((void *)buf_virt, 0, PAGE_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = (uint32_t)buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_table->prdt[0].dbc = 511;
    cmd_table->prdt[0].i = 1;
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_IDENTIFY;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    if (ahci_wait_cmd(port, slot, 1000) < 0) {
        pfa_free(buf_phys);
        return -1;
    }
    
    uint16_t *identify = (uint16_t *)buf_virt;
    
    port->sector_count = (uint64_t)identify[100] |
                        ((uint64_t)identify[101] << 16) |
                        ((uint64_t)identify[102] << 32) |
                        ((uint64_t)identify[103] << 48);
    
    if (port->sector_count == 0) {
        port->sector_count = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    }
    
    for (int i = 0; i < 20; i++) {
        port->model[i * 2] = (identify[27 + i] >> 8) & 0xFF;
        port->model[i * 2 + 1] = identify[27 + i] & 0xFF;
    }
    port->model[40] = '\0';
    
    for (int i = 39; i >= 0 && port->model[i] == ' '; i--) {
        port->model[i] = '\0';
    }
    
    for (int i = 0; i < 10; i++) {
        port->serial[i * 2] = (identify[10 + i] >> 8) & 0xFF;
        port->serial[i * 2 + 1] = identify[10 + i] & 0xFF;
    }
    port->serial[20] = '\0';
    
    for (int i = 19; i >= 0 && port->serial[i] == ' '; i--) {
        port->serial[i] = '\0';
    }
    
    uint64_t size_mb = (port->sector_count * AHCI_SECTOR_SIZE) / (1024 * 1024);
    printf("AHCI: Port %u: Model=\"%s\" Serial=\"%s\" Size=%llu MB\n",
           port->port_num, port->model, port->serial, (unsigned long long)size_mb);
    
    pfa_free(buf_phys);
    
    return 0;
}

int ahci_read_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, void *buffer) {
    uint64_t buf_pages;
    uint64_t buf_phys;
    uint64_t buf_virt;
    int slot;
    int result;
    hba_cmd_header_t *cmd_header;
    hba_cmd_table_t *cmd_table;
    fis_reg_h2d_t *fis;

    if (!port->present || port->type != AHCI_DEV_SATA) {
        return -1;
    }
    
    if (count == 0 || count > 128) {
        return -1;
    }

    mutex_lock(&port->io_lock);
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    slot = ahci_find_slot(port);
    if (slot < 0) {
        printf("AHCI: No free command slot for read\n");
        mutex_unlock(&port->io_lock);
        return -1;
    }
    
    buf_pages = (count * AHCI_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!buf_phys) {
        printf("AHCI: Failed to allocate DMA buffer\n");
        mutex_unlock(&port->io_lock);
        return -1;
    }
    buf_virt = buf_phys + KERNEL_VMA;
    
    cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = (uint32_t)buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_table->prdt[0].dbc = (count * AHCI_SECTOR_SIZE) - 1;
    cmd_table->prdt[0].i = 1;
    
    fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_READ_DMA_EX;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 1 << 6;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = count & 0xFF;
    fis->counth = (count >> 8) & 0xFF;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    result = ahci_wait_cmd(port, slot, 5000);
    
    if (result == 0) {
        memcpy(buffer, (void *)buf_virt, count * AHCI_SECTOR_SIZE);
    }

    pfa_free_contiguous(buf_phys, buf_pages);
    mutex_unlock(&port->io_lock);
    
    return result;
}

int ahci_write_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, const void *buffer) {
    uint64_t buf_pages;
    uint64_t buf_phys;
    uint64_t buf_virt;
    int slot;
    int result;
    hba_cmd_header_t *cmd_header;
    hba_cmd_table_t *cmd_table;
    fis_reg_h2d_t *fis;

    if (!port->present || port->type != AHCI_DEV_SATA) {
        return -1;
    }
    
    if (count == 0 || count > 128) {
        return -1;
    }

    mutex_lock(&port->io_lock);
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    slot = ahci_find_slot(port);
    if (slot < 0) {
        printf("AHCI: No free command slot for write\n");
        mutex_unlock(&port->io_lock);
        return -1;
    }
    
    buf_pages = (count * AHCI_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!buf_phys) {
        printf("AHCI: Failed to allocate DMA buffer\n");
        mutex_unlock(&port->io_lock);
        return -1;
    }
    buf_virt = buf_phys + KERNEL_VMA;
    
    memcpy((void *)buf_virt, buffer, count * AHCI_SECTOR_SIZE);

    cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 1;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = (uint32_t)buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_table->prdt[0].dbc = (count * AHCI_SECTOR_SIZE) - 1;
    cmd_table->prdt[0].i = 1;
    
    fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_WRITE_DMA_EX;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 1 << 6;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = count & 0xFF;
    fis->counth = (count >> 8) & 0xFF;

    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    result = ahci_wait_cmd(port, slot, 5000);
    
    pfa_free_contiguous(buf_phys, buf_pages);
    mutex_unlock(&port->io_lock);
    
    return result;
}

int ahci_atapi_read(ahci_port_t *port, uint64_t lba, uint32_t count, void *buffer) {
    uint64_t buf_pages;
    uint64_t buf_phys;
    uint64_t buf_virt;
    uint64_t byte_count;
    int slot;
    int result;
    hba_cmd_header_t *cmd_header;
    hba_cmd_table_t *cmd_table;
    fis_reg_h2d_t *fis;
    uint8_t *acmd;

    if (!port->present || port->type != AHCI_DEV_SATAPI)
        return -1;

    if (count == 0 || count > 32)
        return -1;

    mutex_lock(&port->io_lock);

    byte_count = (uint64_t)count * ATAPI_SECTOR_SIZE;

    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);

    slot = ahci_find_slot(port);
    if (slot < 0) {
        mutex_unlock(&port->io_lock);
        return -1;
    }

    buf_pages = (byte_count + PAGE_SIZE - 1) / PAGE_SIZE;
    buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!buf_phys) {
        mutex_unlock(&port->io_lock);
        return -1;
    }
    buf_virt = buf_phys + KERNEL_VMA;

    cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->a = 1;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;

    cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));

    cmd_table->prdt[0].dba = (uint32_t)buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_table->prdt[0].dbc = byte_count - 1;
    cmd_table->prdt[0].i = 1;

    fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_PACKET;
    fis->featurel = 1;
    fis->lba1 = (uint8_t)(byte_count & 0xFF);
    fis->lba2 = (uint8_t)((byte_count >> 8) & 0xFF);

    acmd = cmd_table->acmd;
    memset(acmd, 0, 16);
    acmd[0] = SCSI_READ12;
    acmd[2] = (uint8_t)((lba >> 24) & 0xFF);
    acmd[3] = (uint8_t)((lba >> 16) & 0xFF);
    acmd[4] = (uint8_t)((lba >> 8) & 0xFF);
    acmd[5] = (uint8_t)(lba & 0xFF);
    acmd[6] = (uint8_t)((count >> 24) & 0xFF);
    acmd[7] = (uint8_t)((count >> 16) & 0xFF);
    acmd[8] = (uint8_t)((count >> 8) & 0xFF);
    acmd[9] = (uint8_t)(count & 0xFF);

    ahci_port_write(port, AHCI_PxCI, 1 << slot);

    result = ahci_wait_cmd(port, slot, 10000);

    if (result == 0)
        memcpy(buffer, (void *)buf_virt, byte_count);

    pfa_free_contiguous(buf_phys, buf_pages);
    mutex_unlock(&port->io_lock);

    return result;
}

ahci_port_t *ahci_find_cdrom(void) {
    uint64_t i;

    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_t *port = ahci_get_port(i);
        if (port && port->present && port->type == AHCI_DEV_SATAPI)
            return port;
    }
    return NULL;
}

int ahci_flush(ahci_port_t *port) {
    int slot;
    int result;
    hba_cmd_header_t *cmd_header;
    hba_cmd_table_t *cmd_table;
    fis_reg_h2d_t *fis;

    if (!port->present || port->type != AHCI_DEV_SATA) {
        return -1;
    }

    mutex_lock(&port->io_lock);
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    slot = ahci_find_slot(port);
    if (slot < 0) {
        mutex_unlock(&port->io_lock);
        return -1;
    }
    
    cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 0; 
    
    cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_FLUSH_EXT;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    result = ahci_wait_cmd(port, slot, 30000);
    mutex_unlock(&port->io_lock);
    return result;
}

ahci_controller_t *ahci_get_controller(void) {
    return &g_ahci_controller;
}

ahci_port_t *ahci_get_port(uint64_t index) {
    ahci_port_t *port;

    port = ahci_port_slot(index);
    if (!port) return NULL;
    if (!port->present) {
        return NULL;
    }
    
    return port;
}

void ahci_debug_info(void) {
    if (!g_ahci_controller.initialized) {
        printf("AHCI: Controller not initialized\n");
        return;
    }
    
    printf("PCI: %u:%u.%u\n", g_ahci_controller.pci_bus, 
           g_ahci_controller.pci_slot, g_ahci_controller.pci_func);
    printf("ABAR: 0x%016lX (virt: 0x%016lX)\n", g_ahci_controller.abar, g_ahci_controller.abar_virt);
    printf("Version: %u.%u\n", (g_ahci_controller.version >> 16) & 0xFFFF,
           g_ahci_controller.version & 0xFFFF);
    printf("Ports Implemented: 0x%08lX (%u ports)\n", g_ahci_controller.ports_impl,
           g_ahci_controller.num_ports);
    printf("Command Slots: %u\n", g_ahci_controller.num_cmd_slots);
    
    for (uint64_t i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_t *port = ahci_port_slot(i);
        if (!port)
            continue;
        if (!port->present)
            continue;
        
        const char *type_str = "Unknown";
        switch (port->type) {
            case AHCI_DEV_SATA: type_str = "SATA"; break;
            case AHCI_DEV_SATAPI: type_str = "SATAPI"; break;
            case AHCI_DEV_SEMB: type_str = "SEMB"; break;
            case AHCI_DEV_PM: type_str = "PM"; break;
            default: break;
        }
        
        if (port->type == AHCI_DEV_SATA) {
            printf("Port %llu: %s - %s (%llu MB)\n", (unsigned long long)i, type_str, port->model,
                   (unsigned long long)(port->sector_count * AHCI_SECTOR_SIZE / (1024 * 1024)));
        } else {
            printf("Port %llu: %s\n", (unsigned long long)i, type_str);
        }
    }
}

int ahci_init(void) {
    static int probed = 0;

    if (g_ahci_controller.initialized)
        return 0;

    if (probed)
        return -1;
    probed = 1;

    printf("AHCI: Initializing AHCI driver...\n");
    
    memset(&g_ahci_controller, 0, sizeof(g_ahci_controller));
    
    if (ahci_probe() < 0) {
        return -1;
    }
    
    uint64_t abar_phys = g_ahci_controller.abar;
    uint64_t abar_virt = (KERNEL_VMA + 0x37100000ULL);
    uint64_t abar_size;
    uint64_t cap;
    uint64_t ghc;
    uint64_t ports_impl;
    uint64_t ports_capacity;
    uint64_t offset;
    uint64_t i;
    ahci_port_t *port;
    const char *type_str;
    
    abar_size = AHCI_PORT_BASE + AHCI_MAX_PORTS * AHCI_PORT_SIZE;
    abar_size = (abar_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (offset = 0; offset < abar_size; offset += PAGE_SIZE) {
        vmm_map_page(abar_virt + offset, abar_phys + offset, 0x003);
    }
    
    g_ahci_controller.abar_virt = abar_virt;
    
    cap = ahci_hba_read(abar_virt, AHCI_CAP);
    g_ahci_controller.num_cmd_slots = ((cap >> 8) & 0x1F) + 1;
    g_ahci_controller.ports_impl = ahci_hba_read(abar_virt, AHCI_PI);
    g_ahci_controller.version = ahci_hba_read(abar_virt, AHCI_VS);
    ports_capacity = ahci_required_port_capacity(g_ahci_controller.ports_impl);
    g_ahci_controller.ports = (ahci_port_t *)kmalloc(ports_capacity * sizeof(ahci_port_t));
    if (!g_ahci_controller.ports) {
        printf("AHCI: Failed to allocate port table\n");
        return -1;
    }
    memset(g_ahci_controller.ports, 0, ports_capacity * sizeof(ahci_port_t));
    g_ahci_controller.ports_capacity = ports_capacity;
    
    printf("AHCI: CAP=0x%08lX, PI=0x%08lX, VS=0x%08lX\n", cap, g_ahci_controller.ports_impl, g_ahci_controller.version);
    printf("AHCI: Version %u.%u, %u command slots\n",
           (g_ahci_controller.version >> 16) & 0xFFFF,
           g_ahci_controller.version & 0xFFFF,
           g_ahci_controller.num_cmd_slots);
    
    ghc = ahci_hba_read(abar_virt, AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    ahci_hba_write(abar_virt, AHCI_GHC, ghc);
    
    ports_impl = g_ahci_controller.ports_impl;
    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(ports_impl & (1 << i)))
            continue;
        
        port = ahci_port_slot(i);
        if (!port)
            continue;
        port->port_num = i;
        port->hba_base = abar_virt;
        port->port_base = abar_virt + AHCI_PORT_BASE + (i * AHCI_PORT_SIZE);
        mutex_init(&port->io_lock);
        
        port->type = ahci_check_type(port);
        if (port->type == AHCI_DEV_NULL) {
            continue;
        }
        
        port->present = true;
        g_ahci_controller.num_ports++;
        
        type_str = "Unknown";
        switch (port->type) {
            case AHCI_DEV_SATA: type_str = "SATA drive"; break;
            case AHCI_DEV_SATAPI: type_str = "SATAPI drive"; break;
            case AHCI_DEV_SEMB: type_str = "Enclosure Management Bridge"; break;
            case AHCI_DEV_PM: type_str = "Port Multiplier"; break;
            default: break;
        }
        printf("AHCI: Port %u: %s detected\n", i, type_str);
        
        if (ahci_port_init(port) == 0) {
            if (port->type == AHCI_DEV_SATA) {
                ahci_identify(port);
            }
        }
    }
    
    g_ahci_controller.initialized = true;
    
    printf("AHCI: Initialization complete. %u ports active.\n", g_ahci_controller.num_ports);
    
    ahci_debug_info();
    
    return 0;
}

int ahci_test_rw(void) {
    ahci_port_t *port = ahci_get_port(0);
    if (!port) {
        printf("AHCI test: No SATA drive on port 0\n");
        return -1;
    }
    
    printf("AHCI test: Testing read/write on port 0...\n");
    
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    
    for (int i = 0; i < 512; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }
    
    uint64_t test_lba = 100;
    
    uint8_t backup[512];
    if (ahci_read_sectors(port, test_lba, 1, backup) < 0) {
        printf("AHCI test: Failed to backup sector %llu\n", (unsigned long long)test_lba);
        return -1;
    }
    
    if (ahci_write_sectors(port, test_lba, 1, write_buf) < 0) {
        printf("AHCI test: Write failed\n");
        return -1;
    }
    
    if (ahci_flush(port) < 0) {
        printf("AHCI test: Flush failed\n");
    }
    
    memset(read_buf, 0, 512);
    if (ahci_read_sectors(port, test_lba, 1, read_buf) < 0) {
        printf("AHCI test: Read failed\n");
        return -1;
    }
    
    int errors = 0;
    for (int i = 0; i < 512; i++) {
        if (read_buf[i] != write_buf[i]) {
            errors++;
        }
    }
    
    ahci_write_sectors(port, test_lba, 1, backup);
    ahci_flush(port);
    
    if (errors == 0) {
        printf("AHCI test: SUCCESS - Read/write verified correctly\n");
        return 0;
    } else {
        printf("AHCI test: FAILED - %d byte mismatches\n", errors);
        return -1;
    }
}

const char *ahci_error_string(ahci_error_t err) {
    switch (err) {
        case AHCI_ERR_NONE: return "No error";
        case AHCI_ERR_TIMEOUT: return "Command timeout";
        case AHCI_ERR_TASKFILE: return "Task file error";
        case AHCI_ERR_INTERFACE: return "Interface error";
        case AHCI_ERR_DMA: return "DMA error";
        case AHCI_ERR_DEVICE: return "Device error";
        case AHCI_ERR_RESET_FAIL: return "Port reset failed";
        default: return "Unknown error";
    }
}

ahci_error_t ahci_get_last_error(ahci_port_t *port) {
    return port->last_error;
}

static void ahci_port_clear_error(ahci_port_t *port) {
    ahci_port_write(port, AHCI_PxSERR, 0xFFFFFFFF);
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    port->last_error = AHCI_ERR_NONE;
}

int ahci_port_reset(ahci_port_t *port) {
    printf("AHCI: Resetting port %u...\n", port->port_num);
    
    ahci_stop_cmd(port);
    
    uint64_t sctl = ahci_port_read(port, AHCI_PxSCTL);
    sctl = (sctl & ~0x0F) | 0x01;
    ahci_port_write(port, AHCI_PxSCTL, sctl);
    
    ahci_delay(2);
    
    sctl = ahci_port_read(port, AHCI_PxSCTL);
    sctl &= ~0x0F;
    ahci_port_write(port, AHCI_PxSCTL, sctl);
    
    int timeout = 500;
    while (timeout-- > 0) {
        uint64_t ssts = ahci_port_read(port, AHCI_PxSSTS);
        uint8_t det = ssts & AHCI_PxSSTS_DET_MASK;
        if (det == AHCI_PxSSTS_DET_READY)
            break;
        ahci_delay(1);
    }
    
    if (timeout <= 0) {
        printf("AHCI: Port %u reset timeout (no device ready)\n", port->port_num);
        port->last_error = AHCI_ERR_RESET_FAIL;
        return -1;
    }
    
    ahci_port_clear_error(port);
    
    ahci_start_cmd(port);
    
    printf("AHCI: Port %u reset complete\n", port->port_num);
    return 0;
}

int ahci_port_recover(ahci_port_t *port) {
    printf("AHCI: Attempting recovery on port %u (error: %s)\n", 
           port->port_num, ahci_error_string(port->last_error));
    
    ahci_stop_cmd(port);
    
    for (uint64_t i = 0; i < AHCI_CMD_SLOTS; i++) {
        if (port->requests[i].state == AHCI_CMD_STATE_ACTIVE) {
            port->requests[i].state = AHCI_CMD_STATE_ERROR;
            port->requests[i].result = -1;
            port->requests[i].completed = true;
            if (port->requests[i].callback) {
                port->requests[i].callback(port, i, -1, port->requests[i].callback_ctx);
            }
        }
    }
    
    port->cmd_issued = 0;
    port->cmd_running = 0;
    
    ahci_port_clear_error(port);
    
    if (ahci_port_reset(port) < 0) {
        return -1;
    }
    
    port->error_count++;
    printf("AHCI: Port %u recovery complete (total errors: %u)\n", 
           port->port_num, port->error_count);
    
    return 0;
}

static int ahci_check_port_error(ahci_port_t *port) {
    uint64_t is = ahci_port_read(port, AHCI_PxIS);
    uint64_t tfd = ahci_port_read(port, AHCI_PxTFD);
    
    if (is & AHCI_PxIS_TFES) {
        port->last_error = AHCI_ERR_TASKFILE;
        port->last_tfd = tfd;
        port->last_serr = ahci_port_read(port, AHCI_PxSERR);
        printf("AHCI: Port %u task file error (TFD=0x%08lX, SERR=0x%08lX)\n",
               port->port_num, tfd, port->last_serr);
        return -1;
    }
    
    if (is & (AHCI_PxIS_HBFS | AHCI_PxIS_HBDS)) {
        port->last_error = AHCI_ERR_DMA;
        port->last_serr = ahci_port_read(port, AHCI_PxSERR);
        printf("AHCI: Port %u DMA error (IS=0x%08lX)\n", port->port_num, is);
        return -1;
    }
    
    if (is & AHCI_PxIS_IFS) {
        port->last_error = AHCI_ERR_INTERFACE;
        port->last_serr = ahci_port_read(port, AHCI_PxSERR);
        printf("AHCI: Port %u interface error (SERR=0x%08lX)\n", 
               port->port_num, port->last_serr);
        return -1;
    }
    
    return 0;
}

static int ahci_find_free_slot(ahci_port_t *port) {
    uint64_t slots = port->cmd_issued | ahci_port_read(port, AHCI_PxCI) | 
                     ahci_port_read(port, AHCI_PxSACT);
    for (uint64_t i = 0; i < g_ahci_controller.num_cmd_slots; i++) {
        if ((slots & (1 << i)) == 0 && port->requests[i].state == AHCI_CMD_STATE_FREE)
            return i;
    }
    return -1;
}

int ahci_read_async(ahci_port_t *port, uint64_t lba, uint64_t count,
                    void *buffer, ahci_callback_t callback, void *ctx) {
    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    
    if (count == 0 || count > 128)
        return -1;
    
    int slot = ahci_find_free_slot(port);
    if (slot < 0)
        return -1;
    
    ahci_cmd_request_t *req = &port->requests[slot];
    req->state = AHCI_CMD_STATE_PENDING;
    req->command = ATA_CMD_READ_DMA_EX;
    req->lba = lba;
    req->count = count;
    req->buffer = buffer;
    req->callback = callback;
    req->callback_ctx = ctx;
    req->result = 0;
    req->completed = false;
    
    uint64_t buf_pages = (count * AHCI_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    req->buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!req->buf_phys) {
        req->state = AHCI_CMD_STATE_FREE;
        return -1;
    }
    req->buf_pages = buf_pages;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = (uint32_t)req->buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(req->buf_phys >> 32);
    cmd_table->prdt[0].dbc = (count * AHCI_SECTOR_SIZE) - 1;
    cmd_table->prdt[0].i = 1;
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_READ_DMA_EX;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 1 << 6;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = count & 0xFF;
    fis->counth = (count >> 8) & 0xFF;
    
    req->state = AHCI_CMD_STATE_ACTIVE;
    port->cmd_issued |= (1 << slot);
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    return slot;
}

int ahci_write_async(ahci_port_t *port, uint64_t lba, uint64_t count,
                     const void *buffer, ahci_callback_t callback, void *ctx) {
    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    
    if (count == 0 || count > 128)
        return -1;
    
    int slot = ahci_find_free_slot(port);
    if (slot < 0)
        return -1;
    
    ahci_cmd_request_t *req = &port->requests[slot];
    req->state = AHCI_CMD_STATE_PENDING;
    req->command = ATA_CMD_WRITE_DMA_EX;
    req->lba = lba;
    req->count = count;
    req->buffer = (void *)buffer;
    req->callback = callback;
    req->callback_ctx = ctx;
    req->result = 0;
    req->completed = false;
    
    uint64_t buf_pages = (count * AHCI_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    req->buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!req->buf_phys) {
        req->state = AHCI_CMD_STATE_FREE;
        return -1;
    }
    req->buf_pages = buf_pages;
    
    uint64_t buf_virt = req->buf_phys + KERNEL_VMA;
    
    memcpy((void *)buf_virt, buffer, count * AHCI_SECTOR_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 1;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = (uint32_t)req->buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(req->buf_phys >> 32);
    cmd_table->prdt[0].dbc = (count * AHCI_SECTOR_SIZE) - 1;
    cmd_table->prdt[0].i = 1;
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_WRITE_DMA_EX;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 1 << 6;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = count & 0xFF;
    fis->counth = (count >> 8) & 0xFF;
    
    req->state = AHCI_CMD_STATE_ACTIVE;
    port->cmd_issued |= (1 << slot);
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    return slot;
}

void ahci_poll_completion(ahci_port_t *port) {
    uint64_t ci = ahci_port_read(port, AHCI_PxCI);
    uint64_t completed = port->cmd_issued & ~ci;
    
    if (ahci_check_port_error(port) < 0) {
        ahci_port_recover(port);
        return;
    }
    
    for (uint64_t i = 0; i < g_ahci_controller.num_cmd_slots; i++) {
        if (completed & (1 << i)) {
            ahci_cmd_request_t *req = &port->requests[i];
            if (req->state == AHCI_CMD_STATE_ACTIVE) {
                uint64_t buf_virt = req->buf_phys + KERNEL_VMA;
                
                if (req->command == ATA_CMD_READ_DMA_EX) {
                    memcpy(req->buffer, (void *)buf_virt, req->count * AHCI_SECTOR_SIZE);
                }
                
                pfa_free_contiguous(req->buf_phys, req->buf_pages);
                
                req->state = AHCI_CMD_STATE_COMPLETE;
                req->result = 0;
                req->completed = true;
                port->cmd_issued &= ~(1 << i);
                
                if (req->callback) {
                    req->callback(port, i, 0, req->callback_ctx);
                }
                
                req->state = AHCI_CMD_STATE_FREE;
            }
        }
    }
}

static void ahci_port_irq_handler(ahci_port_t *port) {
    uint64_t is = ahci_port_read(port, AHCI_PxIS);
    ahci_port_write(port, AHCI_PxIS, is);
    
    if (is & AHCI_PxIS_FATAL) {
        ahci_check_port_error(port);
        ahci_port_recover(port);
        return;
    }
    
    if (is & (AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS | AHCI_PxIS_SDBS)) {
        ahci_poll_completion(port);
    }
}

void ahci_irq_handler(void *regs) {
    (void)regs;
    
    if (!g_ahci_controller.initialized)
        return;
    
    uint64_t is = ahci_hba_read(g_ahci_controller.abar_virt, AHCI_IS);
    
    for (uint64_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (is & (1 << i)) {
            ahci_port_t *port = ahci_port_slot(i);
            if (port && port->present) {
                ahci_port_irq_handler(port);
            }
        }
    }
    
    ahci_hba_write(g_ahci_controller.abar_virt, AHCI_IS, is);
}

void ahci_enable_interrupts(void) {
    if (!g_ahci_controller.initialized)
        return;
    
    uint64_t irq_line = pci_read_config(g_ahci_controller.pci_bus,
                                         g_ahci_controller.pci_slot,
                                         g_ahci_controller.pci_func, 0x3C) & 0xFF;
    
    if (irq_line == 0 || irq_line == 0xFF) {
        printf("AHCI: No valid IRQ line configured\n");
        return;
    }
    
    g_ahci_controller.irq = irq_line;
    
    irq_register_handler(irq_line, (irq_handler_t)ahci_irq_handler);
    irq_unmask(irq_line);
    
    uint64_t ghc = ahci_hba_read(g_ahci_controller.abar_virt, AHCI_GHC);
    ghc |= AHCI_GHC_IE;
    ahci_hba_write(g_ahci_controller.abar_virt, AHCI_GHC, ghc);
    
    for (uint64_t i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_t *port = ahci_port_slot(i);
        if (!port)
            continue;
        if (port->present) {
            uint64_t ie = AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS |
                          AHCI_PxIS_SDBS | AHCI_PxIS_FATAL;
            ahci_port_write(port, AHCI_PxIE, ie);
            port->use_irq = true;
        }
    }
    
    g_ahci_controller.irq_enabled = true;
    printf("AHCI: Interrupts enabled (IRQ %u)\n", irq_line);
}

void ahci_disable_interrupts(void) {
    if (!g_ahci_controller.initialized)
        return;
    
    uint64_t ghc = ahci_hba_read(g_ahci_controller.abar_virt, AHCI_GHC);
    ghc &= ~AHCI_GHC_IE;
    ahci_hba_write(g_ahci_controller.abar_virt, AHCI_GHC, ghc);
    
    for (uint64_t i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_t *port = ahci_port_slot(i);
        if (!port)
            continue;
        if (port->present) {
            ahci_port_write(port, AHCI_PxIE, 0);
            port->use_irq = false;
        }
    }
    
    if (g_ahci_controller.irq_enabled) {
        irq_mask(g_ahci_controller.irq);
        irq_unregister_handler(g_ahci_controller.irq);
    }
    
    g_ahci_controller.irq_enabled = false;
    printf("AHCI: Interrupts disabled\n");
}

int ahci_smart_enable(ahci_port_t *port) {
    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0)
        return -1;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 0;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_SMART;
    fis->featurel = ATA_SMART_ENABLE;
    fis->lba1 = ATA_SMART_LBA_MID;
    fis->lba2 = ATA_SMART_LBA_HI;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    return ahci_wait_cmd(port, slot, 1000);
}

int ahci_smart_disable(ahci_port_t *port) {
    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0)
        return -1;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 0;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_SMART;
    fis->featurel = ATA_SMART_DISABLE;
    fis->lba1 = ATA_SMART_LBA_MID;
    fis->lba2 = ATA_SMART_LBA_HI;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    return ahci_wait_cmd(port, slot, 1000);
}

int ahci_smart_get_status(ahci_port_t *port) {
    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0)
        return -1;
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 0;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_SMART;
    fis->featurel = ATA_SMART_STATUS;
    fis->lba1 = ATA_SMART_LBA_MID;
    fis->lba2 = ATA_SMART_LBA_HI;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    if (ahci_wait_cmd(port, slot, 1000) < 0)
        return -1;
    
    fis_reg_d2h_t *rfis = &port->fis->rfis;
    if (rfis->lba1 == 0x4F && rfis->lba2 == 0xC2) {
        return 0;
    } else if (rfis->lba1 == 0xF4 && rfis->lba2 == 0x2C) {
        return 1;
    }
    
    return -1;
}

int ahci_smart_read_data(ahci_port_t *port, smart_data_t *data) {
    if (!port->present || port->type != AHCI_DEV_SATA || !data)
        return -1;
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0)
        return -1;
    
    uint64_t buf_phys = pfa_alloc();
    if (!buf_phys)
        return -1;
    
    uint64_t buf_virt = buf_phys + KERNEL_VMA;
    memset((void *)buf_virt, 0, PAGE_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = (uint32_t)buf_phys;
    cmd_table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmd_table->prdt[0].dbc = 511;
    cmd_table->prdt[0].i = 1;
    
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_SMART;
    fis->featurel = ATA_SMART_READ_DATA;
    fis->lba1 = ATA_SMART_LBA_MID;
    fis->lba2 = ATA_SMART_LBA_HI;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    int result = ahci_wait_cmd(port, slot, 1000);
    
    if (result == 0) {
        memcpy(data, (void *)buf_virt, sizeof(smart_data_t));
    }
    
    pfa_free(buf_phys);
    
    return result;
}

void ahci_smart_print(smart_data_t *data) {
    if (!data)
        return;
    
    printf("Version: %u\n", data->version);
    printf("Offline status: 0x%02X\n", data->offline_status);
    printf("Self-test status: 0x%02X\n", data->self_test_status);
    
    printf("\nAttributes:\n");
    printf("ID   Flags  Cur  Worst  Raw\n");
    for (int i = 0; i < 30; i++) {
        smart_attr_t *attr = &data->attrs[i];
        if (attr->attr_id == 0)
            continue;
        
        uint64_t raw_val = 0;
        for (int j = 0; j < 6; j++) {
            raw_val |= ((uint64_t)attr->raw[j]) << (j * 8);
        }
        
        printf("%3u  0x%04X %3u  %3u    %llu\n",
               attr->attr_id, attr->flags, attr->current, attr->worst,
               (unsigned long long)raw_val);
    }
}

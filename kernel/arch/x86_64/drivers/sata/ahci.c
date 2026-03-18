#include <kernel/drivers/sata/ahci.h>
#include <kernel/io.h>
#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <string.h>
#include <stddef.h>

static ahci_controller_t g_ahci_controller;

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
        ahci_delay(1);
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
    uint64_t timeout = timeout_ms;
    
    while (timeout--) {
        uint64_t ci = ahci_port_read(port, AHCI_PxCI);
        if ((ci & (1 << slot)) == 0)
            return 0;
        
        uint64_t is = ahci_port_read(port, AHCI_PxIS);
        if (is & (1 << 30)) { 
            printf("AHCI: Task file error on port %u\n", port->port_num);
            return -1;
        }
        
        ahci_delay(1);
    }
    
    printf("AHCI: Command timeout on port %u\n", port->port_num);
    return -1;
}

int ahci_probe(void) {
    printf("AHCI: Probing PCI bus for AHCI controllers...\n");
    
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint64_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                uint16_t vendor = vendor_device & 0xFFFF;
                
                if (vendor == 0xFFFF)
                    continue;
                
                uint64_t class_info = pci_read_config(bus, slot, func, 0x08);
                uint8_t base_class = (class_info >> 24) & 0xFF;
                uint8_t sub_class = (class_info >> 16) & 0xFF;
                uint8_t prog_if = (class_info >> 8) & 0xFF;
                
                if (base_class == PCI_CLASS_STORAGE && 
                    sub_class == PCI_SUBCLASS_SATA &&
                    prog_if == PCI_PROGIF_AHCI) {
                    
                    uint16_t device_id = (vendor_device >> 16) & 0xFFFF;
                    printf("AHCI: Found controller at PCI %u:%u.%u (VID: 0x%04X, DID: 0x%04X)\n",
                           bus, slot, func, vendor, device_id);
                    
                    g_ahci_controller.pci_bus = bus;
                    g_ahci_controller.pci_slot = slot;
                    g_ahci_controller.pci_func = func;
                    
                    g_ahci_controller.abar = pci_read_config(bus, slot, func, 0x24) & 0xFFFFFFF0;
                    printf("AHCI: ABAR = 0x%016lX\n", g_ahci_controller.abar);
                    
                    uint64_t cmd = pci_read_config(bus, slot, func, 0x04);
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
    uint64_t cmd_list_phys;
    uint64_t fis_phys;
    uint64_t cmd_table_phys;
    uint64_t cmd_list_virt;
    uint64_t fis_virt;
    uint64_t cmd_table_virt;
    int i;

    printf("AHCI: Initializing port %u...\n", port->port_num);

    ahci_stop_cmd(port);
    printf("AHCI: port %u stop_cmd done\n", port->port_num);

    cmd_list_phys = pfa_alloc();
    if (!cmd_list_phys) {
        printf("AHCI: Failed to allocate command list for port %u\n", port->port_num);
        return -1;
    }

    fis_phys = pfa_alloc();
    if (!fis_phys) {
        printf("AHCI: Failed to allocate FIS for port %u\n", port->port_num);
        pfa_free(cmd_list_phys);
        return -1;
    }

    cmd_table_phys = pfa_alloc();
    if (!cmd_table_phys) {
        printf("AHCI: Failed to allocate command table for port %u\n", port->port_num);
        pfa_free(cmd_list_phys);
        pfa_free(fis_phys);
        return -1;
    }
    printf("AHCI: port %u alloc done (cmd=0x%lX fis=0x%lX tbl=0x%lX)\n",
           port->port_num, cmd_list_phys, fis_phys, cmd_table_phys);

    cmd_list_virt = (KERNEL_VMA + 0x38000000ULL) + (port->port_num * 3 * PAGE_SIZE);
    fis_virt = cmd_list_virt + PAGE_SIZE;
    cmd_table_virt = fis_virt + PAGE_SIZE;

    vmm_map_page(cmd_list_virt, cmd_list_phys, 0x003);
    printf("AHCI: port %u map cmd_list done\n", port->port_num);
    vmm_map_page(fis_virt, fis_phys, 0x003);
    printf("AHCI: port %u map fis done\n", port->port_num);
    vmm_map_page(cmd_table_virt, cmd_table_phys, 0x003);
    printf("AHCI: port %u map cmd_table done\n", port->port_num);

    memset((void *)cmd_list_virt, 0, PAGE_SIZE);
    memset((void *)fis_virt, 0, PAGE_SIZE);
    memset((void *)cmd_table_virt, 0, PAGE_SIZE);
    printf("AHCI: port %u memset done\n", port->port_num);

    port->cmd_list = (hba_cmd_header_t *)cmd_list_virt;
    port->fis = (hba_fis_t *)fis_virt;
    port->cmd_table = (hba_cmd_table_t *)cmd_table_virt;
    port->cmd_list_phys = cmd_list_phys;
    port->fis_phys = fis_phys;
    port->cmd_table_phys = cmd_table_phys;

    ahci_port_write(port, AHCI_PxCLB, cmd_list_phys);
    ahci_port_write(port, AHCI_PxCLBU, 0);

    ahci_port_write(port, AHCI_PxFB, fis_phys);
    ahci_port_write(port, AHCI_PxFBU, 0);
    printf("AHCI: port %u port regs written\n", port->port_num);

    for (i = 0; i < AHCI_CMD_SLOTS; i++) {
        port->cmd_list[i].prdtl = AHCI_PRDT_ENTRIES;
        port->cmd_list[i].ctba = cmd_table_phys + (i * sizeof(hba_cmd_table_t));
        port->cmd_list[i].ctbau = 0;
    }

    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    ahci_port_write(port, AHCI_PxSERR, 0xFFFFFFFF);

    ahci_start_cmd(port);

    printf("AHCI: Port %u initialized (CLB=0x%016lX, FB=0x%016lX, CTBA=0x%016lX)\n",
           port->port_num, cmd_list_phys, fis_phys, cmd_table_phys);

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
    
    uint64_t buf_virt = (KERNEL_VMA + 0x39000000ULL) + (port->port_num * PAGE_SIZE);
    vmm_map_page(buf_virt, buf_phys, 0x003);
    memset((void *)buf_virt, 0, PAGE_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = buf_phys;
    cmd_table->prdt[0].dbau = 0;
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
        vmm_unmap_page(buf_virt);
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
    vmm_unmap_page(buf_virt);
    
    return 0;
}

int ahci_read_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, void *buffer) {
    if (!port->present || port->type != AHCI_DEV_SATA) {
        return -1;
    }
    
    if (count == 0 || count > 128) {
        return -1;
    }
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0) {
        printf("AHCI: No free command slot for read\n");
        return -1;
    }
    
    uint64_t buf_pages = (count * AHCI_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!buf_phys) {
        printf("AHCI: Failed to allocate DMA buffer\n");
        return -1;
    }
    
    uint64_t buf_virt = (KERNEL_VMA + 0x3A000000ULL);
    for (uint64_t i = 0; i < buf_pages; i++) {
        vmm_map_page(buf_virt + i * PAGE_SIZE, buf_phys + i * PAGE_SIZE, 0x003);
    }
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = buf_phys;
    cmd_table->prdt[0].dbau = 0;
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
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    int result = ahci_wait_cmd(port, slot, 5000);
    
    if (result == 0) {
        memcpy(buffer, (void *)buf_virt, count * AHCI_SECTOR_SIZE);
    }
    
    for (uint64_t i = 0; i < buf_pages; i++) {
        vmm_unmap_page(buf_virt + i * PAGE_SIZE);
    }
    pfa_free_contiguous(buf_phys, buf_pages);
    
    return result;
}

int ahci_write_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, const void *buffer) {
    if (!port->present || port->type != AHCI_DEV_SATA) {
        return -1;
    }
    
    if (count == 0 || count > 128) {
        return -1;
    }
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0) {
        printf("AHCI: No free command slot for write\n");
        return -1;
    }
    
    uint64_t buf_pages = (count * AHCI_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t buf_phys = pfa_alloc_contiguous(buf_pages);
    if (!buf_phys) {
        printf("AHCI: Failed to allocate DMA buffer\n");
        return -1;
    }
    
    uint64_t buf_virt = (KERNEL_VMA + 0x3A000000ULL);
    for (uint64_t i = 0; i < buf_pages; i++) {
        vmm_map_page(buf_virt + i * PAGE_SIZE, buf_phys + i * PAGE_SIZE, 0x003);
    }
    
    memcpy((void *)buf_virt, buffer, count * AHCI_SECTOR_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 1;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = buf_phys;
    cmd_table->prdt[0].dbau = 0;
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
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    int result = ahci_wait_cmd(port, slot, 5000);
    
    for (uint64_t i = 0; i < buf_pages; i++) {
        vmm_unmap_page(buf_virt + i * PAGE_SIZE);
    }
    pfa_free_contiguous(buf_phys, buf_pages);
    
    return result;
}

int ahci_flush(ahci_port_t *port) {
    if (!port->present || port->type != AHCI_DEV_SATA) {
        return -1;
    }
    
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    
    int slot = ahci_find_slot(port);
    if (slot < 0) {
        return -1;
    }
    
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
    fis->command = ATA_CMD_FLUSH_EXT;
    fis->device = 0;
    
    ahci_port_write(port, AHCI_PxCI, 1 << slot);
    
    return ahci_wait_cmd(port, slot, 30000);
}

ahci_controller_t *ahci_get_controller(void) {
    return &g_ahci_controller;
}

ahci_port_t *ahci_get_port(uint64_t index) {
    if (index >= AHCI_MAX_PORTS) {
        return NULL;
    }
    
    ahci_port_t *port = &g_ahci_controller.ports[index];
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
        ahci_port_t *port = &g_ahci_controller.ports[i];
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
        
        printf("Port %u: %s", i, type_str);
        if (port->type == AHCI_DEV_SATA) {
            printf(" - %s (%llu MB)", port->model, 
                   (unsigned long long)(port->sector_count * AHCI_SECTOR_SIZE / (1024 * 1024)));
        }
        printf("\n");
    }
}

int ahci_init(void) {
    if (g_ahci_controller.initialized)
        return 0;

    printf("AHCI: Initializing AHCI driver...\n");
    
    memset(&g_ahci_controller, 0, sizeof(g_ahci_controller));
    
    if (ahci_probe() < 0) {
        return -1;
    }
    
    uint64_t abar_phys = g_ahci_controller.abar;
    uint64_t abar_virt = (KERNEL_VMA + 0x37100000ULL);
    
    for (uint64_t offset = 0; offset < 0x2000; offset += PAGE_SIZE) {
        vmm_map_page(abar_virt + offset, abar_phys + offset, 0x003);
    }
    
    g_ahci_controller.abar_virt = abar_virt;
    
    uint64_t cap = ahci_hba_read(abar_virt, AHCI_CAP);
    g_ahci_controller.num_cmd_slots = ((cap >> 8) & 0x1F) + 1;
    g_ahci_controller.ports_impl = ahci_hba_read(abar_virt, AHCI_PI);
    g_ahci_controller.version = ahci_hba_read(abar_virt, AHCI_VS);
    
    printf("AHCI: CAP=0x%08lX, PI=0x%08lX, VS=0x%08lX\n", cap, g_ahci_controller.ports_impl, g_ahci_controller.version);
    printf("AHCI: Version %u.%u, %u command slots\n",
           (g_ahci_controller.version >> 16) & 0xFFFF,
           g_ahci_controller.version & 0xFFFF,
           g_ahci_controller.num_cmd_slots);
    
    uint64_t ghc = ahci_hba_read(abar_virt, AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    ahci_hba_write(abar_virt, AHCI_GHC, ghc);
    
    uint64_t ports_impl = g_ahci_controller.ports_impl;
    for (uint64_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(ports_impl & (1 << i)))
            continue;
        
        ahci_port_t *port = &g_ahci_controller.ports[i];
        port->port_num = i;
        port->hba_base = abar_virt;
        port->port_base = abar_virt + AHCI_PORT_BASE + (i * AHCI_PORT_SIZE);
        
        port->type = ahci_check_type(port);
        if (port->type == AHCI_DEV_NULL) {
            continue;
        }
        
        port->present = true;
        g_ahci_controller.num_ports++;
        
        const char *type_str = "Unknown";
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
    
    uint64_t buf_virt = (KERNEL_VMA + 0x3A000000ULL) + (slot * 0x10000);
    for (uint64_t i = 0; i < buf_pages; i++) {
        vmm_map_page(buf_virt + i * PAGE_SIZE, req->buf_phys + i * PAGE_SIZE, 0x003);
    }
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = req->buf_phys;
    cmd_table->prdt[0].dbau = 0;
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
    
    uint64_t buf_virt = (KERNEL_VMA + 0x3A000000ULL) + (slot * 0x10000);
    for (uint64_t i = 0; i < buf_pages; i++) {
        vmm_map_page(buf_virt + i * PAGE_SIZE, req->buf_phys + i * PAGE_SIZE, 0x003);
    }
    
    memcpy((void *)buf_virt, buffer, count * AHCI_SECTOR_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 1;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = req->buf_phys;
    cmd_table->prdt[0].dbau = 0;
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
                uint64_t buf_virt = (KERNEL_VMA + 0x3A000000ULL) + (i * 0x10000);
                
                if (req->command == ATA_CMD_READ_DMA_EX) {
                    memcpy(req->buffer, (void *)buf_virt, req->count * AHCI_SECTOR_SIZE);
                }
                
                for (uint64_t j = 0; j < req->buf_pages; j++) {
                    vmm_unmap_page(buf_virt + j * PAGE_SIZE);
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
            ahci_port_t *port = &g_ahci_controller.ports[i];
            if (port->present) {
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
        ahci_port_t *port = &g_ahci_controller.ports[i];
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
        ahci_port_t *port = &g_ahci_controller.ports[i];
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
    
    uint64_t buf_virt = (KERNEL_VMA + 0x3B000000ULL);
    vmm_map_page(buf_virt, buf_phys, 0x003);
    memset((void *)buf_virt, 0, PAGE_SIZE);
    
    hba_cmd_header_t *cmd_header = &port->cmd_list[slot];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->p = 0;
    cmd_header->c = 1;
    cmd_header->prdtl = 1;
    
    hba_cmd_table_t *cmd_table = port->cmd_table + slot;
    memset(cmd_table, 0, sizeof(hba_cmd_table_t));
    
    cmd_table->prdt[0].dba = buf_phys;
    cmd_table->prdt[0].dbau = 0;
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
    
    vmm_unmap_page(buf_virt);
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

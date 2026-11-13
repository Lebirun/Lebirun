#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/io.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/tty.h>
#include <lebirun/idt.h>
#include <lebirun/pit.h>
#include <lebirun/lke.h>
#include <lebirun/pci.h>
#include <lebirun/timekeeping.h>
#include <string.h>
#include <stddef.h>

static ahci_controller_t g_ahci_controller;
static uint32_t ahci_ports_allocated;

static ahci_port_t *ahci_port_slot(uint64_t index) {
    uint64_t i;
    uint64_t slot;

    if (!g_ahci_controller.ports) return NULL;
    if (index >= AHCI_PORT_COUNT) return NULL;
    if (!(ahci_ports_allocated & (1ULL << index))) return NULL;
    slot = 0;
    for (i = 0; i < index; i++) {
        if (ahci_ports_allocated & (1ULL << i)) slot++;
    }
    if (slot >= g_ahci_controller.ports_capacity) return NULL;
    return &g_ahci_controller.ports[slot];
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint64_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
    uint64_t address = (uint64_t)((bus << 16) | (slot << 11) |
                      (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void KERNEL_INIT pci_write_config(uint8_t bus, uint8_t slot,
                                         uint8_t func, uint8_t offset,
                                         uint64_t value) {
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

static int ahci_interrupts_enabled(void) {
    uint64_t flags;

    __asm__ volatile("pushf; pop %0" : "=r"(flags) : : "memory");
    return (flags & (1u << 9)) != 0;
}

static ahci_dev_type_t KERNEL_INIT ahci_check_type(uint64_t port_base) {
    uint64_t ssts;
    uint64_t sig;
    uint8_t ipm;
    uint8_t det;

    ssts = ahci_hba_read(port_base, AHCI_PxSSTS);
    ipm = (ssts >> AHCI_PxSSTS_IPM_SHIFT) & 0x0F;
    det = ssts & AHCI_PxSSTS_DET_MASK;

    if (det != AHCI_PxSSTS_DET_READY)
        return AHCI_DEV_NULL;
    if (ipm != AHCI_PxSSTS_IPM_ACTIVE)
        return AHCI_DEV_NULL;

    sig = ahci_hba_read(port_base, AHCI_PxSIG);
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

#define AHCI_LBA48_MAX 0x0000FFFFFFFFFFFFULL
#define AHCI_TABLE_HDR sizeof(hba_cmd_table_t)
#define AHCI_TABLE_PAGE_CAP ((PAGE_SIZE - AHCI_TABLE_HDR) / sizeof(hba_prdt_entry_t))
#define AHCI_PRD_MAX 0x400000ULL
#define AHCI_RW_MAX_SECTORS 65536ULL

typedef struct {
    hba_cmd_table_t *table;
    uint64_t table_phys;
    uint32_t cap;
    uint32_t prdtl;
    int reserve;
    uint64_t bounce_phys;
    uint64_t bounce_pages;
    uint64_t lba;
    uint32_t count;
    uint8_t write;
    uint8_t rw;
    uint8_t atapi;
} ahci_cmd_t;

static void ahci_yield(uint64_t spins) {
    extern int scheduler_initialized;

    if (spins >= 16384 && (spins & 4095) == 0 && scheduler_initialized &&
        current_task && ahci_interrupts_enabled())
        schedule();
    else if (spins < 4096)
        __asm__ volatile("pause");
    else if ((spins & 255) == 0)
        ahci_wait_delay();
}

static uint64_t ahci_lock(ahci_port_t *port) {
    uint64_t flags;

    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&port->lock);
    return flags;
}

static void ahci_unlock(ahci_port_t *port, uint64_t flags) {
    spin_unlock(&port->lock);
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
}

static uint32_t ahci_span(const void *buffer, uint64_t bytes) {
    uint64_t start;

    if ((uint64_t)(uintptr_t)buffer < KERNEL_VMA)
        return (uint32_t)((bytes + AHCI_PRD_MAX - 1) / AHCI_PRD_MAX);
    start = (uint64_t)(uintptr_t)buffer & (PAGE_SIZE - 1);
    return (uint32_t)((start + bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

static int ahci_table_get(ahci_port_t *port, ahci_cmd_t *cmd, uint32_t entries) {
    uint64_t size;
    uint64_t align;
    uint64_t spins;

    memset(cmd, 0, sizeof(*cmd));
    if (entries == 0) entries = 1;
    if (entries <= port->reserve_cap &&
        !__atomic_exchange_n(&port->reserve_busy, 1, __ATOMIC_ACQUIRE)) {
        cmd->table = port->reserve;
        cmd->table_phys = port->reserve_phys;
        cmd->cap = port->reserve_cap;
        cmd->reserve = 1;
        return 0;
    }
    if (entries <= AHCI_TABLE_PAGE_CAP) {
        size = AHCI_TABLE_HDR + (uint64_t)entries * sizeof(hba_prdt_entry_t);
        for (align = 128; align < size; align <<= 1);
        cmd->table = (hba_cmd_table_t *)kmalloc_aligned(size, align);
        if (cmd->table) {
            cmd->table_phys = blkdev_phys(cmd->table);
            if (cmd->table_phys) {
                cmd->cap = entries;
                return 0;
            }
            kfree_aligned(cmd->table);
            cmd->table = NULL;
        }
    }
    if (entries > port->reserve_cap) return -1;
    for (spins = 0;
         __atomic_exchange_n(&port->reserve_busy, 1, __ATOMIC_ACQUIRE);
         spins++)
        ahci_yield(spins + 16384);
    cmd->table = port->reserve;
    cmd->table_phys = port->reserve_phys;
    cmd->cap = port->reserve_cap;
    cmd->reserve = 1;
    return 0;
}

static void ahci_table_put(ahci_port_t *port, ahci_cmd_t *cmd) {
    if (cmd->bounce_phys) pfa_free_contiguous(cmd->bounce_phys, cmd->bounce_pages);
    if (cmd->reserve)
        __atomic_store_n(&port->reserve_busy, 0, __ATOMIC_RELEASE);
    else if (cmd->table)
        kfree_aligned(cmd->table);
    cmd->table = NULL;
    cmd->bounce_phys = 0;
}

static int ahci_prd_add(ahci_cmd_t *cmd, uint64_t phys, uint64_t bytes) {
    hba_prdt_entry_t *prd;

    while (bytes) {
        prd = cmd->prdtl ? &cmd->table->prdt[cmd->prdtl - 1] : NULL;
        if (prd && ((uint64_t)prd->dbau << 32 | prd->dba) + prd->dbc + 1 == phys &&
            prd->dbc + 1 + bytes <= AHCI_PRD_MAX) {
            prd->dbc += (uint32_t)bytes;
            return 0;
        }
        if (cmd->prdtl >= cmd->cap) return -1;
        prd = &cmd->table->prdt[cmd->prdtl++];
        memset(prd, 0, sizeof(*prd));
        prd->dba = (uint32_t)phys;
        prd->dbau = (uint32_t)(phys >> 32);
        prd->dbc = (uint32_t)((bytes > AHCI_PRD_MAX ? AHCI_PRD_MAX : bytes) - 1);
        phys += prd->dbc + 1;
        bytes -= prd->dbc + 1;
    }
    return 0;
}

static int ahci_prd_buffer(ahci_cmd_t *cmd, const void *buffer, uint64_t bytes) {
    uint64_t address;
    uint64_t page_bytes;
    uint64_t physical;
    uint64_t offset;

    if ((uint64_t)(uintptr_t)buffer < KERNEL_VMA) return -1;
    for (offset = 0; offset < bytes; offset += page_bytes) {
        address = (uint64_t)(uintptr_t)buffer + offset;
        page_bytes = PAGE_SIZE - (address & (PAGE_SIZE - 1));
        if (page_bytes > bytes - offset) page_bytes = bytes - offset;
        physical = blkdev_phys((const void *)(uintptr_t)address);
        if (!physical || ahci_prd_add(cmd, physical, page_bytes) != 0) return -1;
    }
    return 0;
}

static int ahci_prd_bounce(ahci_cmd_t *cmd, uint64_t bytes) {
    cmd->prdtl = 0;
    cmd->bounce_pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    cmd->bounce_phys = pfa_alloc_contiguous(cmd->bounce_pages);
    if (!cmd->bounce_phys) return -1;
    return ahci_prd_add(cmd, cmd->bounce_phys, bytes);
}

static void ahci_build_fis(ahci_cmd_t *cmd, int ncq, int tag) {
    fis_reg_h2d_t *fis;
    uint8_t *acmd;

    fis = (fis_reg_h2d_t *)cmd->table->cfis;
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    if (cmd->atapi) {
        fis->command = ATA_CMD_PACKET;
        fis->featurel = 1;
        fis->lba1 = 0xFE;
        fis->lba2 = 0xFF;
        acmd = cmd->table->acmd;
        memset(acmd, 0, 16);
        acmd[0] = SCSI_READ12;
        acmd[2] = (uint8_t)(cmd->lba >> 24);
        acmd[3] = (uint8_t)(cmd->lba >> 16);
        acmd[4] = (uint8_t)(cmd->lba >> 8);
        acmd[5] = (uint8_t)cmd->lba;
        acmd[6] = (uint8_t)(cmd->count >> 24);
        acmd[7] = (uint8_t)(cmd->count >> 16);
        acmd[8] = (uint8_t)(cmd->count >> 8);
        acmd[9] = (uint8_t)cmd->count;
        return;
    }
    fis->lba0 = (uint8_t)cmd->lba;
    fis->lba1 = (uint8_t)(cmd->lba >> 8);
    fis->lba2 = (uint8_t)(cmd->lba >> 16);
    fis->lba3 = (uint8_t)(cmd->lba >> 24);
    fis->lba4 = (uint8_t)(cmd->lba >> 32);
    fis->lba5 = (uint8_t)(cmd->lba >> 40);
    fis->device = 1 << 6;
    if (ncq) {
        fis->command = cmd->write ? ATA_CMD_WRITE_FPDMA : ATA_CMD_READ_FPDMA;
        fis->featurel = (uint8_t)cmd->count;
        fis->featureh = (uint8_t)(cmd->count >> 8);
        fis->countl = (uint8_t)(tag << 3);
    } else {
        fis->command = cmd->write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
        fis->countl = (uint8_t)cmd->count;
        fis->counth = (uint8_t)(cmd->count >> 8);
    }
}

static int ahci_tag_claim(ahci_port_t *port, int ncq, uint64_t *gen) {
    uint64_t flags;
    uint64_t spins;
    uint32_t free_tags;
    int waiting;
    int tag;

    waiting = 0;
    for (spins = 0;; spins++) {
        flags = ahci_lock(port);
        free_tags = ~port->busy & port->tag_mask;
        if (!port->resetting && free_tags &&
            (ncq ? !port->nonq_busy && !port->nonq_waiting : !port->busy)) {
            tag = __builtin_ctz(free_tags);
            port->busy |= 1u << tag;
            if (!ncq) {
                port->nonq_busy = true;
                if (waiting) port->nonq_waiting--;
            }
            *gen = port->gen;
            ahci_unlock(port, flags);
            return tag;
        }
        if (!ncq && !waiting) {
            port->nonq_waiting++;
            waiting = 1;
        }
        ahci_unlock(port, flags);
        ahci_yield(spins);
    }
}

static void ahci_tag_release(ahci_port_t *port, int tag, int ncq) {
    uint64_t flags;

    flags = ahci_lock(port);
    port->busy &= ~(1u << tag);
    if (!ncq) port->nonq_busy = false;
    ahci_unlock(port, flags);
}

static void ahci_reset_port(ahci_port_t *port, uint64_t gen) {
    uint64_t flags;
    uint64_t is;
    int timeout;

    flags = ahci_lock(port);
    if (port->gen != gen || port->resetting) {
        ahci_unlock(port, flags);
        return;
    }
    port->resetting = true;
    __atomic_add_fetch(&port->gen, 1, __ATOMIC_ACQ_REL);
    ahci_unlock(port, flags);

    is = ahci_port_read(port, AHCI_PxIS) | port->irq_is;
    port->last_tfd = ahci_port_read(port, AHCI_PxTFD);
    port->last_serr = ahci_port_read(port, AHCI_PxSERR);
    port->last_error = (is & AHCI_PxIS_TFES) ? AHCI_ERR_TASKFILE :
                       (is & (AHCI_PxIS_HBFS | AHCI_PxIS_HBDS)) ? AHCI_ERR_DMA :
                       (is & AHCI_PxIS_IFS) ? AHCI_ERR_INTERFACE : AHCI_ERR_TIMEOUT;
    printf("AHCI: Port %u %s (IS=0x%08lX TFD=0x%08lX SERR=0x%08lX), resetting\n",
           port->port_num, ahci_error_string(port->last_error), is,
           port->last_tfd, port->last_serr);
    port->error_count++;
    ahci_port_reset(port);
    for (timeout = 1000; timeout > 0 &&
         (ahci_port_read(port, AHCI_PxTFD) & 0x88); timeout--)
        ahci_delay(1);
    ahci_port_write(port, AHCI_PxSERR, 0xFFFFFFFF);
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    __atomic_store_n(&port->irq_is, 0, __ATOMIC_RELEASE);

    flags = ahci_lock(port);
    port->resetting = false;
    ahci_unlock(port, flags);
}

typedef struct {
    ahci_port_t *port;
    uint64_t gen;
    uint32_t bit;
    int ncq;
} ahci_wait_t;

static int ahci_wait_done(void *arg) {
    ahci_wait_t *w;
    uint64_t pending;

    w = (ahci_wait_t *)arg;
    pending = ahci_port_read(w->port, AHCI_PxCI);
    if (w->ncq) pending |= ahci_port_read(w->port, AHCI_PxSACT);
    return !(pending & w->bit) || w->port->gen != w->gen ||
           ((ahci_port_read(w->port, AHCI_PxIS) | w->port->irq_is) &
            AHCI_PxIS_FATAL);
}

static int ahci_wait_tag(ahci_port_t *port, int tag, int ncq, uint64_t gen,
                         uint64_t timeout_ms) {
    uint64_t start;
    uint64_t timeout_ticks;
    uint64_t spins;
    uint64_t pending;
    uint64_t is;
    uint64_t deadline;
    ahci_wait_t w;

    w.port = port;
    w.gen = gen;
    w.bit = 1u << tag;
    w.ncq = ncq;
    deadline = timekeeping_monotonic_ns() + timeout_ms * 1000000ULL;
    start = tick_count;
    timeout_ticks = pit_freq ? (timeout_ms * pit_freq + 999) / 1000 : 0;
    for (spins = 0;; spins++) {
        pending = ahci_port_read(port, AHCI_PxCI);
        if (ncq) pending |= ahci_port_read(port, AHCI_PxSACT);
        is = ahci_port_read(port, AHCI_PxIS) | port->irq_is;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&port->gen, __ATOMIC_ACQUIRE) != gen) return -2;
        if (!(pending & (1u << tag))) return 0;
        if ((is & AHCI_PxIS_FATAL) ||
            (timeout_ticks ? tick_count - start >= timeout_ticks :
                             spins >= timeout_ms * 10000)) {
            ahci_reset_port(port, gen);
            return -1;
        }
        if (!port->use_irq ||
            waitq_wait_cond(&port->wq, ahci_wait_done, &w, deadline) < 0)
            ahci_yield(spins);
    }
}

static int ahci_issue(ahci_port_t *port, ahci_cmd_t *cmd, int ncq,
                      uint64_t timeout_ms) {
    hba_cmd_header_t *hdr;
    uint64_t gen;
    int tag;
    int result;

    tag = ahci_tag_claim(port, ncq, &gen);
    if (cmd->rw) ahci_build_fis(cmd, ncq, tag);
    hdr = &port->cmd_list[tag];
    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl = sizeof(fis_reg_h2d_t) / 4;
    hdr->w = cmd->write;
    hdr->a = cmd->atapi;
    hdr->c = !ncq;
    hdr->prdtl = (uint16_t)cmd->prdtl;
    hdr->ctba = (uint32_t)cmd->table_phys;
    hdr->ctbau = (uint32_t)(cmd->table_phys >> 32);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ncq) ahci_port_write(port, AHCI_PxSACT, 1u << tag);
    ahci_port_write(port, AHCI_PxCI, 1u << tag);
    result = ahci_wait_tag(port, tag, ncq, gen, timeout_ms);
    ahci_tag_release(port, tag, ncq);
    return result;
}

static int ahci_run(ahci_port_t *port, ahci_cmd_t *cmd, int ncq,
                    uint64_t timeout_ms) {
    int result;
    int aborted;
    int retried;

    ncq = ncq && port->ncq && cmd->rw && !cmd->atapi;
    retried = 0;
    for (aborted = 0;; aborted++) {
        result = ahci_issue(port, cmd, ncq, timeout_ms);
        if (result == 0) return 0;
        if (result == -2 && aborted < 8) continue;
        if (retried) return -1;
        retried = 1;
        ncq = 0;
    }
}

static int ahci_xfer(ahci_port_t *port, uint64_t lba, uint64_t units,
                     uint32_t unit, void *buffer, int write, int atapi) {
    ahci_cmd_t cmd;
    uint8_t *bytes;
    uint64_t max_units;
    uint64_t n;
    uint64_t size;
    uint32_t entries;
    int result;

    bytes = (uint8_t *)buffer;
    max_units = atapi ? 0xFFFFFFFFULL : AHCI_RW_MAX_SECTORS;
    while (units) {
        n = units < max_units ? units : max_units;
        entries = ahci_span(bytes, n * unit);
        if (entries > AHCI_TABLE_PAGE_CAP) {
            n = (uint64_t)(AHCI_TABLE_PAGE_CAP - 1) * PAGE_SIZE / unit;
            entries = ahci_span(bytes, n * unit);
        }
        if (ahci_table_get(port, &cmd, entries) != 0) {
            n = (uint64_t)(port->reserve_cap - 1) * PAGE_SIZE / unit;
            if (!n) return -1;
            entries = ahci_span(bytes, n * unit);
            if (ahci_table_get(port, &cmd, entries) != 0) return -1;
        }
        size = n * unit;
        if (ahci_prd_buffer(&cmd, bytes, size) != 0) {
            while (ahci_prd_bounce(&cmd, size) != 0) {
                if (cmd.bounce_phys) pfa_free_contiguous(cmd.bounce_phys, cmd.bounce_pages);
                cmd.bounce_phys = 0;
                if (n * unit <= PAGE_SIZE) {
                    ahci_table_put(port, &cmd);
                    return -1;
                }
                n = (n + 1) / 2;
                size = n * unit;
            }
            if (write) memcpy((void *)(uintptr_t)(cmd.bounce_phys + KERNEL_VMA), bytes, size);
        }
        cmd.lba = lba;
        cmd.count = (uint32_t)n;
        cmd.write = (uint8_t)write;
        cmd.atapi = (uint8_t)atapi;
        cmd.rw = 1;
        cmd.table->prdt[cmd.prdtl - 1].i = 1;
        result = ahci_run(port, &cmd, 1, atapi ? 10000 : 5000);
        if (result == 0 && !write && cmd.bounce_phys)
            memcpy(bytes, (void *)(uintptr_t)(cmd.bounce_phys + KERNEL_VMA), size);
        ahci_table_put(port, &cmd);
        if (result != 0) return result;
        bytes += size;
        lba += n;
        units -= n;
    }
    return 0;
}

static int ahci_simple(ahci_port_t *port, uint8_t command, uint8_t feature,
                       void *data, uint64_t timeout_ms) {
    ahci_cmd_t cmd;
    fis_reg_h2d_t *fis;
    int result;

    if (ahci_table_get(port, &cmd, 2) != 0) return -1;
    memset(cmd.table, 0, AHCI_TABLE_HDR);
    if (data) {
        if (ahci_prd_buffer(&cmd, data, 512) != 0) {
            ahci_table_put(port, &cmd);
            return -1;
        }
        cmd.table->prdt[cmd.prdtl - 1].i = 1;
    }
    fis = (fis_reg_h2d_t *)cmd.table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;
    fis->featurel = feature;
    if (command == ATA_CMD_SMART) {
        fis->lba1 = ATA_SMART_LBA_MID;
        fis->lba2 = ATA_SMART_LBA_HI;
    }
    result = ahci_run(port, &cmd, 0, timeout_ms);
    ahci_table_put(port, &cmd);
    return result;
}

int KERNEL_INIT ahci_port_init(ahci_port_t *port) {
    uint64_t page_phys;
    uint64_t page_virt;
    uint64_t reserve_off;

    ahci_stop_cmd(port);

    page_phys = pfa_alloc();
    if (!page_phys) {
        KERNEL_INIT_LOG("AHCI: Failed to allocate page for port %u\n",
                        port->port_num);
        return -1;
    }

    page_virt = page_phys + KERNEL_VMA;
    vmm_map_page(page_virt, page_phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);

    reserve_off = (AHCI_CMD_LIST_SIZE + sizeof(hba_fis_t) + 127) & ~127ULL;
    port->cmd_list = (hba_cmd_header_t *)page_virt;
    port->fis = (hba_fis_t *)(page_virt + AHCI_CMD_LIST_SIZE);
    port->reserve = (hba_cmd_table_t *)(page_virt + reserve_off);
    port->cmd_list_phys = page_phys;
    port->fis_phys = page_phys + AHCI_CMD_LIST_SIZE;
    port->reserve_phys = page_phys + reserve_off;
    port->reserve_cap = (uint32_t)((PAGE_SIZE - reserve_off - AHCI_TABLE_HDR) /
                                   sizeof(hba_prdt_entry_t));
    port->tag_mask = g_ahci_controller.num_cmd_slots >= 32 ? 0xFFFFFFFFu :
                     (1u << g_ahci_controller.num_cmd_slots) - 1;
    spinlock_init(&port->lock);

    ahci_port_write(port, AHCI_PxCLB, (uint32_t)port->cmd_list_phys);
    ahci_port_write(port, AHCI_PxCLBU, (uint32_t)(port->cmd_list_phys >> 32));
    ahci_port_write(port, AHCI_PxFB, (uint32_t)port->fis_phys);
    ahci_port_write(port, AHCI_PxFBU, (uint32_t)(port->fis_phys >> 32));
    ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
    ahci_port_write(port, AHCI_PxSERR, 0xFFFFFFFF);

    ahci_start_cmd(port);

    KERNEL_INIT_LOG("AHCI: Port %u initialized (single page at 0x%016lX)\n",
                    port->port_num, page_phys);

    return 0;
}

int KERNEL_INIT ahci_identify(ahci_port_t *port) {
    uint16_t *identify;
    uint64_t buf_phys;
    uint64_t size_mb;
    uint32_t depth;
    int i;

    KERNEL_INIT_LOG("AHCI: Identifying device on port %u...\n",
                    port->port_num);

    buf_phys = (uint64_t)pmm_alloc_mapped_page();
    if (!buf_phys) {
        KERNEL_INIT_LOG("AHCI: Failed to allocate identify buffer\n");
        return -1;
    }
    identify = (uint16_t *)(buf_phys + KERNEL_VMA);
    memset(identify, 0, PAGE_SIZE);
    if (ahci_simple(port, ATA_CMD_IDENTIFY, 0, identify, 1000) < 0) {
        pfa_free(buf_phys);
        return -1;
    }

    port->sector_count = (uint64_t)identify[100] |
                         ((uint64_t)identify[101] << 16) |
                         ((uint64_t)identify[102] << 32) |
                         ((uint64_t)identify[103] << 48);
    if (port->sector_count == 0)
        port->sector_count = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);

    for (i = 0; i < 20; i++) {
        port->model[i * 2] = (identify[27 + i] >> 8) & 0xFF;
        port->model[i * 2 + 1] = identify[27 + i] & 0xFF;
    }
    port->model[40] = '\0';
    for (i = 39; i >= 0 && port->model[i] == ' '; i--)
        port->model[i] = '\0';
    for (i = 0; i < 10; i++) {
        port->serial[i * 2] = (identify[10 + i] >> 8) & 0xFF;
        port->serial[i * 2 + 1] = identify[10 + i] & 0xFF;
    }
    port->serial[20] = '\0';
    for (i = 19; i >= 0 && port->serial[i] == ' '; i--)
        port->serial[i] = '\0';

    if ((ahci_hba_read(port->hba_base, AHCI_CAP) & (1u << 30)) &&
        (identify[76] & (1u << 8))) {
        depth = (identify[75] & 0x1F) + 1;
        if (depth < 32) port->tag_mask &= (1u << depth) - 1;
        port->ncq = true;
    }

    size_mb = (port->sector_count * AHCI_SECTOR_SIZE) / (1024 * 1024);
    KERNEL_INIT_LOG("AHCI: Port %u: Model=\"%s\" Serial=\"%s\" Size=%llu MB%s\n",
                    port->port_num, port->model, port->serial,
                    (unsigned long long)size_mb, port->ncq ? " NCQ" : "");

    pfa_free(buf_phys);
    return 0;
}

static int ahci_check_rw(ahci_port_t *port, uint64_t lba, uint64_t count,
                         const void *buffer) {
    if (!port || !buffer || count == 0 || !port->present ||
        port->type != AHCI_DEV_SATA) return -1;
    if (lba > AHCI_LBA48_MAX || count - 1 > AHCI_LBA48_MAX - lba) return -1;
    if (count > UINT64_MAX / AHCI_SECTOR_SIZE ||
        (uint64_t)(uintptr_t)buffer > UINT64_MAX - count * AHCI_SECTOR_SIZE)
        return -1;
    return 0;
}

int ahci_read_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, void *buffer) {
    if (ahci_check_rw(port, lba, count, buffer) != 0) return -1;
    return ahci_xfer(port, lba, count, AHCI_SECTOR_SIZE, buffer, 0, 0);
}

int ahci_write_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, const void *buffer) {
    if (ahci_check_rw(port, lba, count, buffer) != 0) return -1;
    return ahci_xfer(port, lba, count, AHCI_SECTOR_SIZE, (void *)buffer, 1, 0);
}

int ahci_write_sectorsv(ahci_port_t *port, uint64_t lba,
                        uint64_t sectors, const void *const *buffers,
                        uint32_t buffer_count, uint64_t bytes_per_buffer) {
    ahci_cmd_t cmd;
    uint64_t entries;
    uint32_t i;
    int result;

    if (!buffers || buffer_count == 0 || bytes_per_buffer == 0 ||
        bytes_per_buffer % AHCI_SECTOR_SIZE ||
        buffer_count > UINT64_MAX / bytes_per_buffer ||
        sectors != buffer_count * bytes_per_buffer / AHCI_SECTOR_SIZE ||
        sectors > AHCI_RW_MAX_SECTORS ||
        ahci_check_rw(port, lba, sectors, buffers) != 0) return -1;
    entries = 0;
    for (i = 0; i < buffer_count; i++) {
        if (!buffers[i]) return -1;
        entries += ahci_span(buffers[i], bytes_per_buffer);
    }
    if (entries > AHCI_TABLE_PAGE_CAP || ahci_table_get(port, &cmd, (uint32_t)entries) != 0)
        goto split;
    for (i = 0; i < buffer_count; i++) {
        if (ahci_prd_buffer(&cmd, buffers[i], bytes_per_buffer) != 0) {
            ahci_table_put(port, &cmd);
            goto split;
        }
    }
    cmd.table->prdt[cmd.prdtl - 1].i = 1;
    cmd.lba = lba;
    cmd.count = (uint32_t)sectors;
    cmd.write = 1;
    cmd.rw = 1;
    result = ahci_run(port, &cmd, 1, 5000);
    ahci_table_put(port, &cmd);
    return result;
split:
    for (i = 0; i < buffer_count; i++) {
        if (ahci_write_sectors(port, lba, bytes_per_buffer / AHCI_SECTOR_SIZE,
                               buffers[i]) != 0) return -1;
        lba += bytes_per_buffer / AHCI_SECTOR_SIZE;
    }
    return 0;
}

int ahci_atapi_read(ahci_port_t *port, uint64_t lba, uint32_t count, void *buffer) {
    if (!port || !buffer || count == 0 || !port->present ||
        port->type != AHCI_DEV_SATAPI || lba > UINT32_MAX ||
        count - 1 > UINT32_MAX - lba)
        return -1;
    return ahci_xfer(port, lba, count, ATAPI_SECTOR_SIZE, buffer, 0, 1);
}

ahci_port_t *ahci_find_cdrom(void) {
    uint64_t i;

    for (i = 0; i < AHCI_PORT_COUNT; i++) {
        ahci_port_t *port = ahci_get_port(i);
        if (port && port->present && port->type == AHCI_DEV_SATAPI)
            return port;
    }
    return NULL;
}

int ahci_flush(ahci_port_t *port) {
    if (!port || !port->present || port->type != AHCI_DEV_SATA) return -1;
    if (ahci_simple(port, ATA_CMD_FLUSH_EXT, 0, NULL, 30000) == 0) return 0;
    return ahci_simple(port, ATA_CMD_FLUSH, 0, NULL, 30000);
}


int KERNEL_INIT ahci_probe(void) {
    uint32_t i;
    uint32_t bdf;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint64_t vendor_device;
    uint64_t class_info;
    uint64_t cmd;

    KERNEL_INIT_LOG("AHCI: Probing PCI bus for AHCI controllers...\n");

    for (i = 0; pci_get(i, &bdf) == 0; i++) {
        bus = PCI_BUS(bdf);
        slot = PCI_SLOT(bdf);
        func = PCI_FUNC(bdf);
        class_info = pci_read_config(bus, slot, func, 0x08);
        if (((class_info >> 24) & 0xFF) != PCI_CLASS_STORAGE ||
            ((class_info >> 16) & 0xFF) != PCI_SUBCLASS_SATA ||
            ((class_info >> 8) & 0xFF) != PCI_PROGIF_AHCI)
            continue;

        vendor_device = pci_read_config(bus, slot, func, 0x00);
        KERNEL_INIT_LOG("AHCI: Found controller at PCI %u:%u.%u (VID: 0x%04X, DID: 0x%04X)\n",
                        bus, slot, func, (unsigned)(vendor_device & 0xFFFF),
                        (unsigned)((vendor_device >> 16) & 0xFFFF));

        g_ahci_controller.pci_bus = bus;
        g_ahci_controller.pci_slot = slot;
        g_ahci_controller.pci_func = func;

        g_ahci_controller.abar = pci_read_config(bus, slot, func, 0x24) & 0xFFFFFFF0;
        KERNEL_INIT_LOG("AHCI: ABAR = 0x%016lX\n",
                        g_ahci_controller.abar);

        cmd = pci_read_config(bus, slot, func, 0x04);
        cmd |= (1 << 1) | (1 << 2);
        pci_write_config(bus, slot, func, 0x04, cmd);

        return 0;
    }

    KERNEL_INIT_LOG("AHCI: No AHCI controller found\n");
    return -1;
}


ahci_controller_t *ahci_get_controller(void) {
    return &g_ahci_controller;
}

uint64_t ahci_get_allocated_pages(void) {
    return g_ahci_controller.num_ports;
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
    
    for (uint64_t i = 0; i < AHCI_PORT_COUNT; i++) {
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

static int ahci_blk_read(blkdev_t *dev, uint64_t lba, uint64_t count,
                         void *buf) {
    return ahci_read_sectors((ahci_port_t *)dev->priv, lba, count, buf);
}

static int ahci_blk_write(blkdev_t *dev, uint64_t lba, uint64_t count,
                          const void *buf) {
    return ahci_write_sectors((ahci_port_t *)dev->priv, lba, count, buf);
}

static int ahci_blk_writev(blkdev_t *dev, uint64_t lba, uint64_t sectors,
                           const void *const *bufs, uint32_t count,
                           uint64_t bytes) {
    return ahci_write_sectorsv((ahci_port_t *)dev->priv, lba, sectors, bufs,
                               count, bytes);
}

static int ahci_blk_flush(blkdev_t *dev) {
    return ahci_flush((ahci_port_t *)dev->priv);
}

static int ahci_blk_atapi_read(blkdev_t *dev, uint64_t lba, uint64_t count,
                               void *buf) {
    if (count > UINT32_MAX) return -1;
    return ahci_atapi_read((ahci_port_t *)dev->priv, lba, (uint32_t)count,
                           buf);
}

static const blkdev_ops_t ahci_disk_ops = {
    ahci_blk_read, ahci_blk_write, ahci_blk_writev, ahci_blk_flush
};

static const blkdev_ops_t ahci_cdrom_ops = {
    ahci_blk_atapi_read, NULL, NULL, NULL
};

static void ahci_msi_init(void);

int KERNEL_INIT ahci_init(void) {
    static int probed KERNEL_INIT_BSS;
    uint64_t abar_phys;
    uint64_t abar_virt;
    uint64_t abar_size;
    uint64_t cap;
    uint64_t ghc;
    uint64_t ports_impl;
    uint64_t ports_capacity;
    uint64_t controller_port_count;
    uint64_t offset;
    uint64_t i;
    ahci_port_t *port;
    ahci_dev_type_t port_types[AHCI_PORT_COUNT];
    const char *type_str;

    if (g_ahci_controller.initialized)
        return 0;

    if (probed)
        return -1;
    probed = 1;

    KERNEL_INIT_LOG("AHCI: Initializing AHCI driver...\n");
    
    memset(&g_ahci_controller, 0, sizeof(g_ahci_controller));
    ahci_ports_allocated = 0;
    
    if (ahci_probe() < 0) {
        return -1;
    }
    
    abar_phys = g_ahci_controller.abar;
    abar_virt = KERNEL_VMA + 0x37100000ULL;
    
    abar_size = PAGE_SIZE;
    for (offset = 0; offset < abar_size; offset += PAGE_SIZE) {
        vmm_map_page(abar_virt + offset, abar_phys + offset, 0x003);
    }
    
    g_ahci_controller.abar_virt = abar_virt;
    
    cap = ahci_hba_read(abar_virt, AHCI_CAP);
    g_ahci_controller.num_cmd_slots = ((cap >> 8) & 0x1F) + 1;
    g_ahci_controller.ports_impl = ahci_hba_read(abar_virt, AHCI_PI);
    g_ahci_controller.version = ahci_hba_read(abar_virt, AHCI_VS);
    controller_port_count = (cap & 0x1F) + 1;
    g_ahci_controller.ports_impl &=
        (1ULL << controller_port_count) - 1;
    abar_size = AHCI_PORT_BASE + controller_port_count * AHCI_PORT_SIZE;
    abar_size = (abar_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (offset = PAGE_SIZE; offset < abar_size; offset += PAGE_SIZE) {
        vmm_map_page(abar_virt + offset, abar_phys + offset, 0x003);
    }
    ghc = ahci_hba_read(abar_virt, AHCI_GHC);
    ahci_hba_write(abar_virt, AHCI_GHC, ghc | AHCI_GHC_AE);
    ports_impl = g_ahci_controller.ports_impl;
    ports_capacity = 0;
    for (i = 0; i < AHCI_PORT_COUNT; i++) {
        if (!(ports_impl & (1ULL << i))) continue;
        port_types[i] = ahci_check_type(abar_virt + AHCI_PORT_BASE +
                                        i * AHCI_PORT_SIZE);
        if (port_types[i] == AHCI_DEV_NULL) continue;
        ahci_ports_allocated |= 1U << i;
        ports_capacity++;
    }
    if (ports_capacity != 0) {
        g_ahci_controller.ports = (ahci_port_t *)kmalloc(
            ports_capacity * sizeof(ahci_port_t));
        if (!g_ahci_controller.ports) {
            KERNEL_INIT_LOG("AHCI: Failed to allocate port table\n");
            ahci_hba_write(abar_virt, AHCI_GHC, ghc);
            for (offset = 0; offset < abar_size; offset += PAGE_SIZE) {
                vmm_unmap_page(abar_virt + offset);
            }
            ahci_ports_allocated = 0;
            g_ahci_controller.abar_virt = 0;
            probed = 0;
            return -1;
        }
        memset(g_ahci_controller.ports, 0,
               ports_capacity * sizeof(ahci_port_t));
    }
    g_ahci_controller.ports_capacity = ports_capacity;

    KERNEL_INIT_LOG("AHCI: CAP=0x%08lX, PI=0x%08lX, VS=0x%08lX\n",
                    cap, g_ahci_controller.ports_impl,
                    g_ahci_controller.version);
    KERNEL_INIT_LOG("AHCI: Version %u.%u, %u command slots\n",
                    (g_ahci_controller.version >> 16) & 0xFFFF,
                    g_ahci_controller.version & 0xFFFF,
                    g_ahci_controller.num_cmd_slots);
    
    for (i = 0; i < AHCI_PORT_COUNT; i++) {
        if (!(ports_impl & (1ULL << i)))
            continue;

        port = ahci_port_slot(i);
        if (!port)
            continue;
        port->port_num = i;
        port->hba_base = abar_virt;
        port->port_base = abar_virt + AHCI_PORT_BASE + (i * AHCI_PORT_SIZE);
        
        port->type = port_types[i];
        
        type_str = "Unknown";
        switch (port->type) {
            case AHCI_DEV_SATA: type_str = "SATA drive"; break;
            case AHCI_DEV_SATAPI: type_str = "SATAPI drive"; break;
            case AHCI_DEV_SEMB: type_str = "Enclosure Management Bridge"; break;
            case AHCI_DEV_PM: type_str = "Port Multiplier"; break;
            default: break;
        }
        KERNEL_INIT_LOG("AHCI: Port %u: %s detected\n", i, type_str);
        
        if (ahci_port_init(port) < 0) {
            port->type = AHCI_DEV_NULL;
            continue;
        }

        port->present = true;
        g_ahci_controller.num_ports++;
        if (port->type == AHCI_DEV_SATA) {
            ahci_identify(port);
        }
        if (port->type == AHCI_DEV_SATA || port->type == AHCI_DEV_SATAPI) {
            int cd = port->type == AHCI_DEV_SATAPI;

            port->blk.ops = cd ? &ahci_cdrom_ops : &ahci_disk_ops;
            port->blk.priv = port;
            port->blk.sectors = port->sector_count;
            port->blk.block_size = cd ? ATAPI_SECTOR_SIZE : AHCI_SECTOR_SIZE;
            port->blk.type = cd ? BLKDEV_CDROM : BLKDEV_DISK;
            blkdev_register(&port->blk);
        }
    }

    if (g_ahci_controller.num_ports == 0) {
        kfree(g_ahci_controller.ports);
        g_ahci_controller.ports = NULL;
        g_ahci_controller.ports_capacity = 0;
        ahci_ports_allocated = 0;
    }
    
    g_ahci_controller.initialized = true;
    if (g_ahci_controller.num_ports) ahci_msi_init();
    
    KERNEL_INIT_LOG("AHCI: Initialization complete. %u ports active.\n",
                    g_ahci_controller.num_ports);
    
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
    ahci_reset_port(port, __atomic_load_n(&port->gen, __ATOMIC_ACQUIRE));
    return port->last_error == AHCI_ERR_RESET_FAIL ? -1 : 0;
}

void ahci_irq_handler(void *regs) {
    ahci_port_t *port;
    uint64_t is;
    uint64_t port_is;
    uint64_t i;

    (void)regs;
    if (!g_ahci_controller.initialized)
        return;

    is = ahci_hba_read(g_ahci_controller.abar_virt, AHCI_IS);
    for (i = 0; i < AHCI_PORT_COUNT; i++) {
        if (!(is & (1ULL << i))) continue;
        port = ahci_port_slot(i);
        if (!port || !port->present) continue;
        port_is = ahci_port_read(port, AHCI_PxIS);
        ahci_port_write(port, AHCI_PxIS, port_is);
        if (port_is & AHCI_PxIS_FATAL)
            __atomic_or_fetch(&port->irq_is, (uint32_t)(port_is & AHCI_PxIS_FATAL),
                              __ATOMIC_ACQ_REL);
        waitq_wake_all(&port->wq);
    }
    ahci_hba_write(g_ahci_controller.abar_virt, AHCI_IS, is);
}

static void ahci_msi_irq(void *arg) {
    (void)arg;
    ahci_irq_handler(NULL);
}

static void KERNEL_INIT ahci_msi_init(void) {
    ahci_port_t *port;
    uint32_t bdf;
    uint64_t i;
    int vector;

    bdf = (uint32_t)g_ahci_controller.pci_bus << 8 |
          (uint32_t)g_ahci_controller.pci_slot << 3 |
          (uint32_t)g_ahci_controller.pci_func;
    vector = irq_alloc_vector(ahci_msi_irq, NULL);
    if (vector < 0) return;
    if (pci_msi_enable(bdf, (uint8_t)vector) != 0) {
        irq_free_vector(vector);
        return;
    }
    for (i = 0; i < AHCI_PORT_COUNT; i++) {
        port = ahci_port_slot(i);
        if (!port || !port->present) continue;
        ahci_port_write(port, AHCI_PxIS, 0xFFFFFFFF);
        ahci_port_write(port, AHCI_PxIE, AHCI_PxIS_DHRS | AHCI_PxIS_PSS |
                        AHCI_PxIS_DSS | AHCI_PxIS_SDBS | AHCI_PxIS_FATAL);
        port->use_irq = true;
    }
    ahci_hba_write(g_ahci_controller.abar_virt, AHCI_IS, 0xFFFFFFFF);
    ahci_hba_write(g_ahci_controller.abar_virt, AHCI_GHC,
                   ahci_hba_read(g_ahci_controller.abar_virt, AHCI_GHC) |
                   AHCI_GHC_IE);
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
    
    for (uint64_t i = 0; i < AHCI_PORT_COUNT; i++) {
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
    
    for (uint64_t i = 0; i < AHCI_PORT_COUNT; i++) {
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
    return ahci_simple(port, ATA_CMD_SMART, ATA_SMART_ENABLE, NULL, 1000);
}

int ahci_smart_disable(ahci_port_t *port) {
    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    return ahci_simple(port, ATA_CMD_SMART, ATA_SMART_DISABLE, NULL, 1000);
}

int ahci_smart_get_status(ahci_port_t *port) {
    fis_reg_d2h_t *rfis;

    if (!port->present || port->type != AHCI_DEV_SATA)
        return -1;
    if (ahci_simple(port, ATA_CMD_SMART, ATA_SMART_STATUS, NULL, 1000) < 0)
        return -1;
    rfis = &port->fis->rfis;
    if (rfis->lba1 == 0x4F && rfis->lba2 == 0xC2)
        return 0;
    if (rfis->lba1 == 0xF4 && rfis->lba2 == 0x2C)
        return 1;
    return -1;
}

int ahci_smart_read_data(ahci_port_t *port, smart_data_t *data) {
    uint64_t buf_phys;
    int result;

    if (!port->present || port->type != AHCI_DEV_SATA || !data)
        return -1;
    buf_phys = (uint64_t)pmm_alloc_mapped_page();
    if (!buf_phys)
        return -1;
    memset((void *)(buf_phys + KERNEL_VMA), 0, PAGE_SIZE);
    result = ahci_simple(port, ATA_CMD_SMART, ATA_SMART_READ_DATA,
                         (void *)(buf_phys + KERNEL_VMA), 1000);
    if (result == 0)
        memcpy(data, (void *)(buf_phys + KERNEL_VMA), sizeof(smart_data_t));
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

#ifndef __is_kernel
module_init(ahci_init);
LKE_NAME("ahci");
LKE_DESC("AHCI SATA storage driver");
LKE_LICENSE("GPLv2");
LKE_VERSION_STR("1.0.0");
#endif

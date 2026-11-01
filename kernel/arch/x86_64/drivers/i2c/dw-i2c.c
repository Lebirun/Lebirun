#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/acpi.h>
#include <lebirun/timekeeping.h>
#include <lebirun/drivers/i2c/dw-i2c.h>

#define DW_I2C_VIRT (KERNEL_VMA + 0x37300000ULL)

#define IC_CON           0x00
#define IC_TAR           0x04
#define IC_DATA_CMD      0x10
#define IC_SS_SCL_HCNT   0x14
#define IC_SS_SCL_LCNT   0x18
#define IC_FS_SCL_HCNT   0x1C
#define IC_FS_SCL_LCNT   0x20
#define IC_INTR_MASK     0x30
#define IC_RAW_INTR_STAT 0x34
#define IC_RX_TL         0x38
#define IC_TX_TL         0x3C
#define IC_CLR_INTR      0x40
#define IC_CLR_TX_ABRT   0x54
#define IC_CLR_STOP_DET  0x60
#define IC_ENABLE        0x6C
#define IC_STATUS        0x70
#define IC_TXFLR         0x74
#define IC_RXFLR         0x78
#define IC_SDA_HOLD      0x7C
#define IC_ENABLE_STATUS 0x9C
#define IC_COMP_PARAM_1  0xF4
#define IC_COMP_VERSION  0xF8
#define IC_COMP_TYPE     0xFC

#define DW_COMP_TYPE     0x44570140u
#define DW_INTR_TX_ABRT  (1u << 6)
#define DW_INTR_STOP_DET (1u << 9)
#define DW_CMD_READ      0x100u
#define DW_CMD_STOP      0x200u
#define DW_CMD_RESTART   0x400u
#define DW_TIMEOUT_NS    50000000ULL

dw_i2c_t *dw_i2c_buses;
uint32_t dw_i2c_count;

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t rd(dw_i2c_t *c, uint32_t reg) {
    return *(volatile uint32_t *)(c->base + reg);
}

static inline void wr(dw_i2c_t *c, uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(c->base + reg) = v;
}

static uint32_t KERNEL_INIT pci_rd(uint32_t bdf, uint8_t off) {
    outl(0xCF8, 0x80000000u | (bdf << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static void KERNEL_INIT pci_wr(uint32_t bdf, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bdf << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

void KERNEL_INIT dw_i2c_delay(uint64_t ms) {
    uint64_t end;

    if (tsc_available()) {
        end = tsc_get_ns() + ms * 1000000ULL;
        while (tsc_get_ns() < end)
            __asm__ volatile ("pause");
        return;
    }
    for (end = ms * 20000; end; end--)
        __asm__ volatile ("pause");
}

static void dw_off(dw_i2c_t *c) {
    uint32_t i;

    wr(c, IC_ENABLE, 0);
    for (i = 0; i < 100000 && (rd(c, IC_ENABLE_STATUS) & 1); i++)
        __asm__ volatile ("pause");
    c->on = 0;
}

static void dw_target(dw_i2c_t *c, uint16_t addr, uint8_t fast) {
    if (c->on && c->tar == addr && c->fast == fast) return;
    dw_off(c);
    wr(c, IC_TAR, addr);
    wr(c, IC_CON, 0x61u | (fast ? 4u : 2u));
    wr(c, IC_ENABLE, 1);
    c->tar = addr;
    c->fast = fast;
    c->on = 1;
}

void dw_i2c_abort(dw_i2c_t *c) {
    dw_off(c);
    rd(c, IC_CLR_INTR);
}

int dw_i2c_begin(dw_i2c_t *c, uint16_t addr, uint8_t fast, const uint8_t *w,
                 uint32_t wn, uint8_t *r, uint32_t rn) {
    if (c->on && (rd(c, IC_STATUS) & 0x20)) return -1;
    dw_target(c, addr, fast);
    rd(c, IC_CLR_INTR);
    c->w = w;
    c->wn = wn;
    c->ws = 0;
    c->r = r;
    c->rn = rn;
    c->ri = 0;
    c->rg = 0;
    c->t = timekeeping_monotonic_ns();
    return 0;
}

int dw_i2c_pump(dw_i2c_t *c) {
    uint32_t cmd;
    uint32_t raw;
    int progress;

    progress = 0;
    if (rd(c, IC_RAW_INTR_STAT) & DW_INTR_TX_ABRT) {
        rd(c, IC_CLR_TX_ABRT);
        return -1;
    }
    while (c->rg < c->rn && rd(c, IC_RXFLR)) {
        c->r[c->rg++] = (uint8_t)rd(c, IC_DATA_CMD);
        progress = 1;
    }
    while (rd(c, IC_TXFLR) < c->txd) {
        if (c->ws < c->wn) {
            cmd = c->w[c->ws++];
            if (c->ws == c->wn && !c->rn) cmd |= DW_CMD_STOP;
        } else if (c->ri < c->rn && c->ri - c->rg < c->rxd) {
            cmd = DW_CMD_READ;
            if (!c->ri && c->wn) cmd |= DW_CMD_RESTART;
            if (++c->ri == c->rn) cmd |= DW_CMD_STOP;
        } else {
            break;
        }
        wr(c, IC_DATA_CMD, cmd);
        progress = 1;
    }
    if (progress) c->t = timekeeping_monotonic_ns();
    if (c->ws < c->wn || c->rg < c->rn) return 0;
    if (c->rn) return 1;
    raw = rd(c, IC_RAW_INTR_STAT);
    if (raw & DW_INTR_TX_ABRT) {
        rd(c, IC_CLR_TX_ABRT);
        return -1;
    }
    if (!(raw & DW_INTR_STOP_DET)) return 0;
    rd(c, IC_CLR_STOP_DET);
    return 1;
}

int KERNEL_INIT dw_i2c_xfer(dw_i2c_t *c, uint16_t addr, uint8_t fast,
                            const uint8_t *w, uint32_t wn, uint8_t *r,
                            uint32_t rn) {
    uint64_t spins;
    uint32_t mark;
    int ret;

    for (spins = 0; dw_i2c_begin(c, addr, fast, w, wn, r, rn) < 0; spins++) {
        if (spins > 1000000) {
            dw_i2c_abort(c);
            spins = 0;
        }
        __asm__ volatile ("pause");
    }
    spins = 0;
    mark = 0;
    while (!(ret = dw_i2c_pump(c))) {
        if (c->ws + c->ri + c->rg != mark) {
            mark = c->ws + c->ri + c->rg;
            spins = 0;
        }
        if (++spins > 2000000 ||
            timekeeping_monotonic_ns() - c->t > DW_TIMEOUT_NS) {
            dw_i2c_abort(c);
            return -1;
        }
        __asm__ volatile ("pause");
    }
    return ret < 0 ? -1 : 0;
}

static int KERNEL_INIT dw_add(uint64_t phys, uint32_t priv, int lpss) {
    dw_i2c_t tmp;
    dw_i2c_t *buses;
    uint64_t virt;
    uint32_t p;
    uint32_t i;

    for (i = 0; i < dw_i2c_count; i++)
        if (dw_i2c_buses[i].phys == phys) return 1;
    virt = DW_I2C_VIRT + (uint64_t)dw_i2c_count * PAGE_SIZE;
    vmm_map_page(virt, phys & ~(uint64_t)(PAGE_SIZE - 1),
                 VMM_PTE_PRESENT | VMM_PTE_WRITE | VMM_PTE_PCD);
    memset(&tmp, 0, sizeof(tmp));
    tmp.phys = phys;
    tmp.base = virt + (phys & (PAGE_SIZE - 1));
    if (rd(&tmp, IC_COMP_TYPE) != DW_COMP_TYPE) {
        if (lpss && !(rd(&tmp, 0x2FC) & 0xF0)) {
            wr(&tmp, 0x204, 0);
            wr(&tmp, 0x204, 7);
            wr(&tmp, 0x240, (uint32_t)phys);
            wr(&tmp, 0x244, (uint32_t)(phys >> 32));
        } else if (priv) {
            wr(&tmp, priv + 4, 3);
            wr(&tmp, priv, rd(&tmp, priv) | 1);
        }
    }
    buses = NULL;
    if (rd(&tmp, IC_COMP_TYPE) == DW_COMP_TYPE)
        buses = krealloc(dw_i2c_buses, (dw_i2c_count + 1) * sizeof(*buses));
    if (!buses) {
        vmm_unmap_page(virt);
        return 0;
    }
    dw_off(&tmp);
    wr(&tmp, IC_INTR_MASK, 0);
    p = rd(&tmp, IC_COMP_PARAM_1);
    tmp.txd = p ? ((p >> 16) & 0xFF) + 1 : 8;
    tmp.rxd = p ? ((p >> 8) & 0xFF) + 1 : 8;
    wr(&tmp, IC_SS_SCL_HCNT, 900);
    wr(&tmp, IC_SS_SCL_LCNT, 1050);
    wr(&tmp, IC_FS_SCL_HCNT, 200);
    wr(&tmp, IC_FS_SCL_LCNT, 340);
    if (rd(&tmp, IC_COMP_VERSION) >= 0x3131312A &&
        (rd(&tmp, IC_SDA_HOLD) & 0xFFFF) <= 1)
        wr(&tmp, IC_SDA_HOLD, 65);
    wr(&tmp, IC_RX_TL, 0);
    wr(&tmp, IC_TX_TL, 0);
    rd(&tmp, IC_CLR_INTR);
    tmp.tar = 0xFFFF;
    dw_i2c_buses = buses;
    dw_i2c_buses[dw_i2c_count++] = tmp;
    KERNEL_INIT_LOG("I2C: DesignWare controller at 0x%lX (fifo %u/%u)\n",
                    phys, tmp.txd, tmp.rxd);
    return 1;
}

static void KERNEL_INIT dw_pci(uint32_t bdf, int lpss) {
    uint64_t phys;
    uint32_t bar;
    uint32_t cmd;
    uint32_t cap;
    uint32_t pm;
    uint32_t v;
    uint32_t n;

    bar = pci_rd(bdf, 0x10);
    if (bar & 1) return;
    phys = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2) phys |= (uint64_t)pci_rd(bdf, 0x14) << 32;
    if (!phys) return;
    pm = 0;
    cap = (pci_rd(bdf, 0x04) >> 16) & 0x10 ? pci_rd(bdf, 0x34) & 0xFC : 0;
    for (n = 0; cap && n < 48; n++) {
        v = pci_rd(bdf, (uint8_t)cap);
        if ((v & 0xFF) == 1) {
            pm = cap + 4;
            break;
        }
        cap = (v >> 8) & 0xFC;
    }
    v = pm ? pci_rd(bdf, (uint8_t)pm) : 0;
    if (v & 3) {
        pci_wr(bdf, (uint8_t)pm, v & ~3u);
        dw_i2c_delay(10);
    }
    cmd = pci_rd(bdf, 0x04) & 0xFFFF;
    pci_wr(bdf, 0x04, cmd | 2);
    if (dw_add(phys, 0, lpss)) return;
    pci_wr(bdf, 0x04, cmd);
    if (v & 3) pci_wr(bdf, (uint8_t)pm, v);
}

static void KERNEL_INIT dw_acpi_res(const uint8_t *res, uint32_t len,
                                    void *ctx) {
    uint32_t base;

    if (res[0] == 0x86 && len == 12)
        memcpy(&base, res + 4, 4);
    else if (res[0] == 0x87 && len >= 26 && res[3] == 0)
        memcpy(&base, res + 10, 4);
    else
        return;
    if (base && !(base & (PAGE_SIZE - 1))) dw_add(base, *(uint32_t *)ctx, 0);
}

uint32_t KERNEL_INIT dw_i2c_init(void) {
    const char *intel[] = {
        KERNEL_INIT_STRING("INT33C2"), KERNEL_INIT_STRING("INT33C3"),
        KERNEL_INIT_STRING("INT3432"), KERNEL_INIT_STRING("INT3433"),
        KERNEL_INIT_STRING("80860F41"), KERNEL_INIT_STRING("808622C1"),
        NULL
    };
    const char *other[] = {
        KERNEL_INIT_STRING("AMDI0010"), KERNEL_INIT_STRING("AMDI0019"),
        KERNEL_INIT_STRING("AMDI0510"), NULL
    };
    uint32_t priv;
    uint32_t bus;
    uint32_t slot;
    uint32_t func;
    uint32_t funcs;
    uint32_t bdf;
    uint32_t v;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            bdf = (bus << 8) | (slot << 3);
            if ((pci_rd(bdf, 0x00) & 0xFFFF) == 0xFFFF) continue;
            funcs = (pci_rd(bdf, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (func = 0; func < funcs; func++) {
                v = pci_rd(bdf | func, 0x00);
                if ((v & 0xFFFF) != 0x8086) continue;
                v = pci_rd(bdf | func, 0x08) >> 16;
                if (v == 0x1180 || v == 0x0C80)
                    dw_pci(bdf | func, v == 0x1180 &&
                                       ((1u << slot) & 0x2E10000u));
            }
        }
    }
    priv = 0x800;
    acpi_scan_devices(intel, dw_acpi_res, &priv);
    priv = 0;
    acpi_scan_devices(other, dw_acpi_res, &priv);
    return dw_i2c_count;
}

void KERNEL_INIT dw_i2c_release(void) {
    uint32_t i;

    for (i = 0; i < dw_i2c_count; i++)
        vmm_unmap_page(DW_I2C_VIRT + (uint64_t)i * PAGE_SIZE);
    kfree(dw_i2c_buses);
    dw_i2c_buses = NULL;
    dw_i2c_count = 0;
}

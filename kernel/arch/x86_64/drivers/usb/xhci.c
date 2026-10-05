#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/io.h>
#include <lebirun/drivers/usb/xhci.h>
#include <lebirun/drivers/usb/hid.h>
#include <lebirun/pit.h>

#define XHCI_MMIO_VIRT (KERNEL_VMA + 0x37200000ULL)
#define XHCI_MMIO_PAGES 4u
#define XHCI_DMA_VIRT (KERNEL_VMA + 0x37204000ULL)
#define XHCI_DMA_PAGES 16u

typedef struct {
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    uint64_t mmio_phys;
    uint64_t mmio_virt;
    uint8_t caplength;
    uint16_t version;
    uint8_t max_slots;
    uint8_t max_ports;
    uint64_t op;
    uint64_t rt;
    uint64_t dboff;
    uint64_t dcbaa;
    uint64_t dcbaa_phys;
    uint64_t cmd_ring_phys;
    uint64_t cmd_ring;
    uint64_t cmd_idx;
    uint64_t cmd_cycle;
    uint64_t er_seg_phys;
    uint64_t er_seg;
    uint64_t erdq;
    uint64_t erdq_cycle;
} xhci_t;

static xhci_t g_xhci;

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

static inline uint16_t mmio_read16(uint64_t addr) {
    return *((volatile uint16_t *)addr);
}

static inline uint8_t mmio_read8(uint64_t addr) {
    return *((volatile uint8_t *)addr);
}

static inline void mmio_write64(uint64_t addr, uint64_t value) {
    *((volatile uint64_t *)addr) = value;
}

static uint64_t g_dma_phys[XHCI_DMA_PAGES];
static uint64_t g_dma_count;

static uint64_t KERNEL_INIT xhci_alloc_dma(uint64_t *phys_out) {
    uint64_t phys;
    uint64_t virt;

    if (g_dma_count >= XHCI_DMA_PAGES) return 0;
    phys = pfa_alloc();
    if (!phys) return 0;
    virt = XHCI_DMA_VIRT + g_dma_count * PAGE_SIZE;
    vmm_map_page(virt, phys, VMM_PTE_PRESENT | VMM_PTE_WRITE);
    memset((void *)virt, 0, PAGE_SIZE);
    g_dma_phys[g_dma_count] = phys;
    g_dma_count++;
    if (phys_out) *phys_out = phys;
    return virt;
}

static int KERNEL_INIT xhci_probe(void) {
    uint64_t bus;
    uint64_t slot;
    uint64_t func;
    uint64_t funcs;
    uint64_t vd;
    uint64_t hdr;
    uint64_t cls;
    uint64_t bar0;
    uint64_t bar1;
    uint64_t cmd;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            vd = pci_read_config((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((vd & 0xFFFF) == 0xFFFF) continue;
            hdr = pci_read_config((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            funcs = ((hdr >> 16) & 0x80) ? 8 : 1;
            for (func = 0; func < funcs; func++) {
                vd = pci_read_config((uint8_t)bus, (uint8_t)slot,
                                     (uint8_t)func, 0x00);
                if ((vd & 0xFFFF) == 0xFFFF) continue;
                cls = pci_read_config((uint8_t)bus, (uint8_t)slot,
                                      (uint8_t)func, 0x08);
                if (((cls >> 24) & 0xFF) != PCI_CLASS_SERIAL_BUS) continue;
                if (((cls >> 16) & 0xFF) != PCI_SUBCLASS_USB) continue;
                if (((cls >> 8) & 0xFF) != PCI_PROGIF_XHCI) continue;
                g_xhci.pci_bus = (uint8_t)bus;
                g_xhci.pci_slot = (uint8_t)slot;
                g_xhci.pci_func = (uint8_t)func;
                KERNEL_INIT_LOG("XHCI: Found USB3 controller at PCI %u:%u.%u\n",
                                bus, slot, func);
                bar0 = pci_read_config((uint8_t)bus, (uint8_t)slot,
                                       (uint8_t)func, 0x10);
                if (bar0 & 0x1) return -1;
                if (((bar0 >> 1) & 0x3) == 0x2) {
                    bar1 = pci_read_config((uint8_t)bus, (uint8_t)slot,
                                           (uint8_t)func, 0x14);
                    g_xhci.mmio_phys = (bar1 << 32) | (bar0 & ~0xFULL);
                } else {
                    g_xhci.mmio_phys = bar0 & ~0xFULL;
                }
                if (!g_xhci.mmio_phys) return -1;
                cmd = pci_read_config((uint8_t)bus, (uint8_t)slot,
                                      (uint8_t)func, 0x04);
                cmd |= (1 << 1) | (1 << 2);
                pci_write_config((uint8_t)bus, (uint8_t)slot,
                                 (uint8_t)func, 0x04, cmd);
                return 0;
            }
        }
    }
    return -1;
}

static int KERNEL_INIT xhci_wait(uint64_t addr, uint32_t mask,
                                 uint32_t want) {
    uint64_t i;
    for (i = 0; i < 1000000; i++) {
        if ((mmio_read32(addr) & mask) == want) return 0;
        __asm__ volatile ("pause");
    }
    return -1;
}

static void KERNEL_INIT cmd_word(uint64_t op, uint32_t bits) {
    uint32_t cmd = mmio_read32(op + XHCI_OP_USBCMD);
    mmio_write32(op + XHCI_OP_USBCMD, cmd | bits);
}

static const char *KERNEL_INIT xhci_speed_name(uint64_t speed) {
    if (speed == 1) return "Full-speed";
    if (speed == 2) return "Low-speed";
    if (speed == 3) return "High-speed";
    if (speed == 4) return "SuperSpeed";
    if (speed == 5) return "SuperSpeedPlus";
    return "Unknown";
}

static int KERNEL_INIT xhci_take_ownership(void) {
    uint32_t hcc;
    uint64_t xecp;
    uint64_t off;
    uint32_t val;
    uint64_t i;
    uint64_t w;

    hcc = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_HCCP1);
    xecp = ((uint64_t)((hcc >> 16) & 0xFFFF)) * 4;
    if (!xecp) return 0;
    off = xecp;
    for (i = 0; i < 16; i++) {
        uint8_t id;
        uint8_t next;
        if (off + 8 > XHCI_MMIO_PAGES * PAGE_SIZE) return 0;
        id = mmio_read8(g_xhci.mmio_virt + off);
        next = mmio_read8(g_xhci.mmio_virt + off + 1);
        if (id == XHCI_EXT_CAP_USBLEGSUP) {
            val = mmio_read32(g_xhci.mmio_virt + off);
            if (val & XHCI_LEGSUP_BIOS_OWNED) {
                mmio_write32(g_xhci.mmio_virt + off,
                             val | XHCI_LEGSUP_OS_OWNED);
                for (w = 0; w < 1000000; w++) {
                    val = mmio_read32(g_xhci.mmio_virt + off);
                    if (!(val & XHCI_LEGSUP_BIOS_OWNED)) break;
                    __asm__ volatile ("pause");
                }
                val = mmio_read32(g_xhci.mmio_virt + off);
                if (val & XHCI_LEGSUP_BIOS_OWNED) {
                    KERNEL_INIT_LOG("XHCI: BIOS refused ownership handoff\n");
                    return -1;
                }
                KERNEL_INIT_LOG("XHCI: Ownership handoff complete\n");
            } else {
                mmio_write32(g_xhci.mmio_virt + off,
                             val | XHCI_LEGSUP_OS_OWNED);
            }
            mmio_write32(g_xhci.mmio_virt + off + 4, 0);
            return 0;
        }
        if (!next) return 0;
        off += (uint64_t)next * 4;
    }
    return 0;
}

static int KERNEL_INIT xhci_setup_operational(void) {
    uint64_t op;
    uint64_t rt;
    uint32_t hcc;
    uint32_t hcs2;
    uint64_t nscratch;
    uint64_t slots;
    uint64_t dcbaa;
    uint64_t dcbaa_phys;
    uint64_t *slots64;
    uint64_t arr;
    uint64_t arr_phys;
    uint64_t n;
    uint64_t ring;
    uint64_t ring_phys;
    uint32_t *trb;
    uint64_t erst;
    uint64_t erst_phys;
    uint64_t seg;
    uint64_t seg_phys;
    uint64_t *entry;
    uint32_t dboff;
    uint32_t rtsoff;

    op = g_xhci.mmio_virt + g_xhci.caplength;
    hcc = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_HCCP1);
    if (!(hcc & 0x1)) {
        KERNEL_INIT_LOG("XHCI: 32-bit only controller not supported\n");
        return -1;
    }
    if (xhci_take_ownership() < 0) return -1;

    dcbaa = xhci_alloc_dma(&dcbaa_phys);
    if (!dcbaa) return -1;
    g_xhci.dcbaa = dcbaa;
    g_xhci.dcbaa_phys = dcbaa_phys;
    hcs2 = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_HCSP2);
    nscratch = (uint64_t)(((hcs2 >> 27) & 0x1F) << 5) |
               (uint64_t)((hcs2 >> 21) & 0x1F);
    if (nscratch > 0) {
        arr = xhci_alloc_dma(&arr_phys);
        if (!arr) return -1;
        entry = (uint64_t *)arr;
        for (n = 0; n < nscratch; n++) {
            uint64_t page = xhci_alloc_dma(&ring_phys);
            if (!page) return -1;
            if (n * 8 >= PAGE_SIZE) {
                KERNEL_INIT_LOG("XHCI: Too many scratchpad buffers\n");
                return -1;
            }
            entry[n] = ring_phys;
        }
        slots64 = (uint64_t *)dcbaa;
        slots64[0] = arr_phys;
    }

    ring = xhci_alloc_dma(&ring_phys);
    if (!ring) return -1;
    trb = (uint32_t *)(ring + 255 * 16);
    trb[0] = (uint32_t)(ring_phys & 0xFFFFFFFFULL);
    trb[1] = (uint32_t)((ring_phys >> 32) & 0xFFFFFFFFULL);
    trb[2] = 0;
    trb[3] = (XHCI_TRB_TYPE_LINK << 10) | XHCI_LINK_TC | 0x1;
    mmio_write64(op + XHCI_OP_CRCR, ring_phys | 0x1);

    slots = g_xhci.max_slots < 32 ? g_xhci.max_slots : 32;
    if (!slots) {
        KERNEL_INIT_LOG("XHCI: Controller reports no device slots\n");
        return -1;
    }
    mmio_write32(op + XHCI_OP_DCBAAP + 4,
                 (uint32_t)((dcbaa_phys >> 32) & 0xFFFFFFFFULL));
    mmio_write32(op + XHCI_OP_DCBAAP,
                 (uint32_t)(dcbaa_phys & 0xFFFFFFFFULL));
    mmio_write32(op + XHCI_OP_CONFIG, (uint32_t)slots);

    rtsoff = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_RTSOFF);
    dboff = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_DBOFF);
    rt = g_xhci.mmio_virt + (rtsoff & 0xFFFF);
    g_xhci.rt = rt;
    g_xhci.dboff = g_xhci.mmio_virt + (dboff & 0xFFFF);

    erst = xhci_alloc_dma(&erst_phys);
    if (!erst) return -1;
    seg = xhci_alloc_dma(&seg_phys);
    if (!seg) return -1;
    entry = (uint64_t *)erst;
    entry[0] = seg_phys;
    entry[1] = 256;
    entry[2] = 0;
    entry[3] = 0;
    mmio_write32(rt + XHCI_RT_IR_SET_SIZE + XHCI_RT_ERSTSZ, 1);
    mmio_write64(rt + XHCI_RT_IR_SET_SIZE + XHCI_RT_ERSTBA, erst_phys);
    mmio_write64(rt + XHCI_RT_IR_SET_SIZE + XHCI_RT_ERDP, seg_phys);
    g_xhci.cmd_ring_phys = ring_phys;
    g_xhci.cmd_ring = ring;
    g_xhci.cmd_idx = 0;
    g_xhci.cmd_cycle = 1;
    g_xhci.er_seg_phys = seg_phys;
    g_xhci.er_seg = seg;
    g_xhci.erdq = 0;
    g_xhci.erdq_cycle = 1;

    cmd_word(op, XHCI_USBCMD_RS);
    if (xhci_wait(op + XHCI_OP_USBSTS, XHCI_USBSTS_HALTED, 0) < 0) {
        KERNEL_INIT_LOG("XHCI: Run timeout\n");
        return -1;
    }
    KERNEL_INIT_LOG("XHCI: Operational setup complete (%u slots enabled)\n",
                    (uint32_t)slots);
    return 0;
}

static void KERNEL_INIT xhci_scan_ports(void) {
    uint64_t op;
    uint64_t n;
    uint64_t sc_addr;
    uint32_t sc;
    uint64_t speed;
    uint64_t i;

    op = g_xhci.mmio_virt + g_xhci.caplength;
    for (n = 0; n < g_xhci.max_ports; n++) {
        sc_addr = op + XHCI_OP_PORTSC + n * XHCI_PORTSC_STRIDE;
        sc = mmio_read32(sc_addr);
        if (!(sc & XHCI_PORTSC_CCS)) continue;
        if (!(sc & XHCI_PORTSC_PP)) {
            mmio_write32(sc_addr, sc | XHCI_PORTSC_PP);
            for (i = 0; i < 1000000; i++)
                __asm__ volatile ("pause");
            sc = mmio_read32(sc_addr);
        }
        mmio_write32(sc_addr, sc | XHCI_PORTSC_PR | XHCI_PORTSC_PRC);
        for (i = 0; i < 10000000; i++) {
            sc = mmio_read32(sc_addr);
            if (sc & XHCI_PORTSC_PRC) break;
            __asm__ volatile ("pause");
        }
        sc = mmio_read32(sc_addr);
        if (!(sc & XHCI_PORTSC_PRC)) {
            KERNEL_INIT_LOG("XHCI: Port %u: reset timeout\n", n);
            continue;
        }
        mmio_write32(sc_addr, sc | XHCI_PORTSC_PRC);
        sc = mmio_read32(sc_addr);
        speed = (sc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
        KERNEL_INIT_LOG("XHCI: Port %u: device connected, %s PORTSC=0x%08X\n",
                        n, xhci_speed_name(speed), sc);
    }
}

typedef struct {
    uint8_t used;
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    uint16_t vid;
    uint16_t pid;
    uint8_t dev_class;
    uint8_t mps;
    uint8_t configured;
    uint64_t ep0_ring;
    uint64_t ep0_ring_phys;
    uint64_t ep0_idx;
    uint8_t ep0_cycle;
    uint64_t xfer;
    uint64_t xfer_phys;
    uint64_t inctx;
    uint64_t inctx_phys;
    uint64_t outctx;
    uint64_t outctx_phys;
    uint8_t has_hid;
    uint8_t iface;
    uint8_t proto;
    uint8_t iep;
    uint8_t iep_mps;
    uint8_t iep_interval;
    uint64_t iring;
    uint64_t iring_phys;
    uint64_t iidx;
    uint8_t icycle;
    uint64_t ibuf;
    uint64_t ibuf_phys;
    uint64_t ipend;
    usb_hid_kbd_state_t hid_kbd;
} usb_dev_t;

static usb_dev_t usb_devs[8];
static uint64_t usb_dev_count;

static void xhci_doorbell(uint64_t slot, uint32_t value) {
    __asm__ volatile ("sfence" ::: "memory");
    mmio_write32(g_xhci.dboff + slot * 4, value);
}

static int xhci_next_event(uint64_t *type, uint64_t *code,
                           uint64_t *ptr, uint64_t *slot) {
    uint32_t *ev;
    uint32_t dw3;

    ev = (uint32_t *)(g_xhci.er_seg + g_xhci.erdq * 16);
    dw3 = ev[3];
    if (((dw3 & 0x1) != g_xhci.erdq_cycle)) return 0;
    *ptr = (uint64_t)ev[0] | ((uint64_t)ev[1] << 32);
    *code = (ev[2] >> 24) & 0xFF;
    *type = (dw3 >> 10) & 0x3F;
    *slot = (dw3 >> 24) & 0xFF;
    g_xhci.erdq++;
    if (g_xhci.erdq >= 256) {
        g_xhci.erdq = 0;
        g_xhci.erdq_cycle ^= 0x1;
    }
    return 1;
}

static void xhci_erdp_update(void) {
    uint64_t rt = g_xhci.rt + XHCI_RT_IR_SET_SIZE;
    mmio_write64(rt + XHCI_RT_ERDP,
                 (g_xhci.er_seg_phys + g_xhci.erdq * 16) | 0x8);
}

static int KERNEL_INIT xhci_wait_cmd_complete(uint64_t cmd_phys,
                                              uint64_t *slot_out) {
    uint64_t i;
    uint64_t type;
    uint64_t code;
    uint64_t ptr;
    uint64_t slot;

    for (i = 0; i < 10000000; i++) {
        while (xhci_next_event(&type, &code, &ptr, &slot)) {
            xhci_erdp_update();
            if (type == 33 && ptr == cmd_phys) {
                if (slot_out) *slot_out = slot;
                if (code != 1) {
                    KERNEL_INIT_LOG("XHCI: Command failed, code %u\n",
                                    (uint32_t)code);
                    return -1;
                }
                return 0;
            }
        }
        __asm__ volatile ("pause");
    }
    KERNEL_INIT_LOG("XHCI: Command timeout\n");
    return -1;
}

static uint64_t KERNEL_INIT xhci_cmd_submit(uint64_t lo, uint64_t hi,
                                            uint64_t status, uint64_t ctrl) {
    uint32_t *trb;
    uint64_t phys;

    if (g_xhci.cmd_idx >= 250) return 0;
    trb = (uint32_t *)(g_xhci.cmd_ring + g_xhci.cmd_idx * 16);
    phys = g_xhci.cmd_ring_phys + g_xhci.cmd_idx * 16;
    trb[0] = (uint32_t)(lo & 0xFFFFFFFFULL);
    trb[1] = (uint32_t)(hi & 0xFFFFFFFFULL);
    trb[2] = (uint32_t)(status & 0xFFFFFFFFULL);
    trb[3] = (uint32_t)((ctrl & ~0x1ULL) | (g_xhci.cmd_cycle & 0x1));
    g_xhci.cmd_idx++;
    xhci_doorbell(0, 0);
    return phys;
}

static int KERNEL_INIT xhci_enable_slot(uint64_t *slot_out) {
    uint64_t phys;

    phys = xhci_cmd_submit(0, 0, 0, (9u << 10));
    if (!phys) return -1;
    return xhci_wait_cmd_complete(phys, slot_out);
}

static int KERNEL_INIT xhci_build_input_ctx(usb_dev_t *dev, uint64_t mps,
                                            uint64_t add_mask) {    uint64_t ctx;
    uint64_t ctx_phys;
    uint32_t *p;

    if (!dev->inctx) {
        ctx = xhci_alloc_dma(&ctx_phys);
        if (!ctx) return -1;
        dev->inctx = ctx;
        dev->inctx_phys = ctx_phys;
    }
    ctx = dev->inctx;
    ctx_phys = dev->inctx_phys;
    p = (uint32_t *)ctx;
    p[0] = 0;
    p[1] = (uint32_t)(add_mask & 0xFFFFFFFFULL);
    p[8] = ((uint64_t)1 << 27);
    p[8] |= ((uint64_t)dev->speed << 20);
    p[9] = ((uint64_t)(dev->port + 1) << 16);
    p[10] = 0;
    p[11] = 0;
    p[16] = 0;
    p[17] = ((uint64_t)mps << 16) | (4u << 3) | (3u << 1);
    p[18] = (uint32_t)((dev->ep0_ring_phys + dev->ep0_idx * 16) & 0xFFFFFFFFULL) |
            (dev->ep0_cycle & 0x1);
    p[19] = (uint32_t)(((dev->ep0_ring_phys + dev->ep0_idx * 16) >> 32) & 0xFFFFFFFFULL);
    p[20] = 8 | ((uint64_t)mps << 16);
    p[21] = 0;
    p[22] = 0;
    p[23] = 0;
    return 0;
}

static int KERNEL_INIT xhci_address_device(usb_dev_t *dev) {
    uint64_t phys;
    uint64_t *dcbaa;

    dev->ep0_ring = xhci_alloc_dma(&dev->ep0_ring_phys);
    if (!dev->ep0_ring) return -1;
    dev->ep0_idx = 0;
    dev->ep0_cycle = 1;
    dev->xfer = xhci_alloc_dma(&dev->xfer_phys);
    if (!dev->xfer) return -1;
    dev->outctx = xhci_alloc_dma(&dev->outctx_phys);
    if (!dev->outctx) return -1;
    dcbaa = (uint64_t *)g_xhci.dcbaa;
    dcbaa[dev->slot] = dev->outctx_phys;
    if (xhci_build_input_ctx(dev, dev->mps, 0x3) < 0) return -1;
    phys = xhci_cmd_submit(dev->inctx_phys, 0, 0,
                           (11u << 10) | ((uint64_t)dev->slot << 24));
    if (!phys) return -1;
    return xhci_wait_cmd_complete(phys, 0);
}

static int KERNEL_INIT xhci_evaluate_mps(usb_dev_t *dev, uint64_t mps) {
    uint64_t phys;

    if (xhci_build_input_ctx(dev, mps, 0x3) < 0) return -1;
    phys = xhci_cmd_submit(dev->inctx_phys, 0, 0,
                           (13u << 10) | ((uint64_t)dev->slot << 24));
    if (!phys) return -1;
    if (xhci_wait_cmd_complete(phys, 0) < 0) return -1;
    dev->mps = (uint8_t)mps;
    return 0;
}

static int KERNEL_INIT xhci_wait_xfer(uint64_t status_phys,
                                      uint64_t setup_phys,
                                      uint64_t data_phys) {
    uint64_t i;
    uint64_t type;
    uint64_t code;
    uint64_t ptr;
    uint64_t slot;
    uint64_t rt;
    uint32_t iman;

    rt = g_xhci.rt + XHCI_RT_IR_SET_SIZE;
    mmio_write32(rt + XHCI_RT_IMAN, 0x1);
    for (i = 0; i < 10000000; i++) {
        while (xhci_next_event(&type, &code, &ptr, &slot)) {
            xhci_erdp_update();
            if (type == 32 && (ptr == status_phys || ptr == setup_phys ||
                               ptr == data_phys)) {
                if (code != 1) {
                    KERNEL_INIT_LOG("XHCI: Transfer failed, code %u\n",
                                    (uint32_t)code);
                    return -1;
                }
                return 0;
            }
        }
        __asm__ volatile ("pause");
    }
    iman = mmio_read32(rt + XHCI_RT_IMAN);
    {
        uint64_t op2 = g_xhci.mmio_virt + g_xhci.caplength;
        uint32_t usbsts = mmio_read32(op2 + XHCI_OP_USBSTS);
        KERNEL_INIT_LOG("XHCI: Transfer timeout (IP=%u HCE=%u HCH=%u)\n",
                        (uint32_t)(iman & 0x1),
                        (uint32_t)((usbsts >> 12) & 0x1),
                        (uint32_t)(usbsts & 0x1));
    }
    return -1;
}

static int KERNEL_INIT xhci_control(usb_dev_t *dev, uint8_t *setup,
                                    uint64_t len, uint64_t dir_in) {
    uint32_t *trb;
    uint64_t setup_phys;
    uint64_t data_phys;
    uint64_t status_phys;
    uint64_t ntrb;
    uint32_t ctrl;

    if (dev->ep0_idx + 3 >= 250) return -1;
    setup_phys = dev->ep0_ring_phys + dev->ep0_idx * 16;
    trb = (uint32_t *)(dev->ep0_ring + dev->ep0_idx * 16);
    for (ntrb = 0; ntrb < 8; ntrb++)
        ((uint8_t *)trb)[ntrb] = setup[ntrb];
    ctrl = (uint32_t)(dev->ep0_cycle & 0x1) | (2u << 10) | (1u << 6) |
           (1u << 4) | (uint32_t)(((len ? (dir_in ? 3u : 2u) : 0u)) << 16);
    trb[2] = 8;
    trb[3] = ctrl;
    dev->ep0_idx++;

    data_phys = 0;
    if (len) {
        data_phys = dev->ep0_ring_phys + dev->ep0_idx * 16;
        trb = (uint32_t *)(dev->ep0_ring + dev->ep0_idx * 16);
        trb[0] = (uint32_t)(dev->xfer_phys & 0xFFFFFFFFULL);
        trb[1] = (uint32_t)((dev->xfer_phys >> 32) & 0xFFFFFFFFULL);
        trb[2] = (uint32_t)(len & 0x1FFFFULL);
        trb[3] = (uint32_t)(dev->ep0_cycle & 0x1) | (3u << 10) | (1u << 4) |
                 (uint32_t)((dir_in ? 1u : 0u) << 16);
        dev->ep0_idx++;
    }

    status_phys = dev->ep0_ring_phys + dev->ep0_idx * 16;
    trb = (uint32_t *)(dev->ep0_ring + dev->ep0_idx * 16);
    trb[0] = 0;
    trb[1] = 0;
    trb[2] = 0;
    trb[3] = (uint32_t)(dev->ep0_cycle & 0x1) | (4u << 10) | (1u << 5) |
             (uint32_t)((!len || !dir_in ? 1u : 0u) << 16);
    dev->ep0_idx++;

    xhci_doorbell(dev->slot, 1);
    return xhci_wait_xfer(status_phys, setup_phys, data_phys);
}

static int KERNEL_INIT xhci_get_descriptor(usb_dev_t *dev, uint64_t dtype,
                                           uint64_t dindex, uint64_t len) {
    uint8_t setup[8];

    setup[0] = 0x80;
    setup[1] = 6;
    setup[2] = (uint8_t)(dindex & 0xFF);
    setup[3] = (uint8_t)(dtype & 0xFF);
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)((len >> 8) & 0xFF);
    memset((void *)dev->xfer, 0, PAGE_SIZE);
    if (xhci_control(dev, setup, len, 1) < 0) {
        uint32_t *octx = (uint32_t *)(dev->outctx + 32);
        KERNEL_INIT_LOG("XHCI: FAIL slot=%u ep0 out state=%u deq=%lX\n",
                        dev->slot, (uint32_t)(octx[0] & 0x7),
                        ((uint64_t)octx[3] << 32) | (uint64_t)(octx[2] & ~0xFULL));
        return -1;
    }
    return 0;
}

static int KERNEL_INIT xhci_set_configuration(usb_dev_t *dev,
                                              uint64_t value) {
    uint8_t setup[8];

    setup[0] = 0x00;
    setup[1] = 9;
    setup[2] = (uint8_t)(value & 0xFF);
    setup[3] = (uint8_t)((value >> 8) & 0xFF);
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = 0;
    setup[7] = 0;
    return xhci_control(dev, setup, 0, 0);
}

static int KERNEL_INIT xhci_parse_hid(usb_dev_t *dev, uint8_t *cfg,
                                       uint64_t total) {
    uint64_t off;
    uint8_t iface = 0;
    uint8_t proto = 0;
    uint8_t candidate = 0;

    dev->has_hid = 0;
    if (total < 9 || cfg[0] == 0) return 0;
    off = cfg[0];
    while (off + 2 <= total) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == 4 && len >= 9) {
            uint8_t want = 0;
            candidate = 0;
#if CONFIG_DRIVER_USB_HID_KBD
            want |= (uint8_t)(cfg[off + 7] == 1);
#endif
#if CONFIG_DRIVER_USB_HID_MOUSE
            want |= (uint8_t)(cfg[off + 7] == 2);
#endif
            if (cfg[off + 5] == 3 && cfg[off + 6] == 1 && want) {
                iface = cfg[off + 2];
                proto = cfg[off + 7];
                candidate = 1;
            }
        } else if (type == 5 && len >= 7 && candidate) {
            uint8_t addr = cfg[off + 2];
            if ((cfg[off + 3] & 0x3) == 3 && (addr & 0x80)) {
                dev->has_hid = 1;
                dev->iface = iface;
                dev->proto = proto;
                dev->iep = (uint8_t)(((addr & 0xF) * 2) + 1);
                dev->iep_mps = cfg[off + 4];
                dev->iep_interval = cfg[off + 6];
                KERNEL_INIT_LOG("USB: Port %u: HID iface=%u proto=%u ep=%u mps=%u interval=%u\n",
                                dev->port, iface, proto, dev->iep,
                                dev->iep_mps, dev->iep_interval);
                return 1;
            }
            candidate = 0;
        }
        off += len;
    }
    return 0;
}

static int KERNEL_INIT xhci_configure_hid(usb_dev_t *dev) {
    uint64_t phys;
    uint32_t *p;
    uint64_t dci;
    uint64_t base;

    dev->iring = xhci_alloc_dma(&dev->iring_phys);
    if (!dev->iring) return -1;
    dev->iidx = 0;
    dev->icycle = 1;
    dev->ibuf = xhci_alloc_dma(&dev->ibuf_phys);
    if (!dev->ibuf) return -1;
    dci = dev->iep;
    base = 8 + (uint64_t)dci * 8;
    if (xhci_build_input_ctx(dev, dev->mps,
                             0x1 | (1u << dci)) < 0) return -1;
    p = (uint32_t *)dev->inctx;
    p[base + 0] = (uint64_t)dev->iep_interval << 16;
    p[base + 1] = ((uint64_t)dev->iep_mps << 16) | (7u << 3) | (3u << 1);
    p[base + 2] = (uint32_t)(dev->iring_phys & 0xFFFFFFFFULL) | 0x1;
    p[base + 3] = (uint32_t)((dev->iring_phys >> 32) & 0xFFFFFFFFULL);
    p[base + 4] = (uint64_t)dev->iep_mps | ((uint64_t)dev->iep_mps << 16);
    p[base + 5] = 0;
    p[base + 6] = 0;
    p[base + 7] = 0;
    phys = xhci_cmd_submit(dev->inctx_phys, 0, 0,
                           (12u << 10) | ((uint64_t)dev->slot << 24));
    if (!phys) return -1;
    if (xhci_wait_cmd_complete(phys, 0) < 0) {
        KERNEL_INIT_LOG("USB: Port %u: configure endpoint %u failed\n",
                        dev->port, dev->iep);
        return -1;
    }
    KERNEL_INIT_LOG("USB: Port %u: endpoint %u configured\n",
                    dev->port, dev->iep);
    return 0;
}

static int KERNEL_INIT xhci_hid_ctrl(usb_dev_t *dev, uint8_t req,
                                     uint64_t value) {
    uint8_t setup[8];

    setup[0] = 0x21;
    setup[1] = req;
    setup[2] = (uint8_t)(value & 0xFF);
    setup[3] = (uint8_t)((value >> 8) & 0xFF);
    setup[4] = dev->iface;
    setup[5] = 0;
    setup[6] = 0;
    setup[7] = 0;
    return xhci_control(dev, setup, 0, 0);
}

static int KERNEL_INIT xhci_enumerate_port(uint64_t port) {
    uint64_t op;
    uint64_t sc_addr;
    uint32_t sc;
    uint64_t speed;
    uint64_t slot;
    usb_dev_t *dev;
    uint8_t *desc;
    uint64_t mps;
    uint64_t total;
    uint64_t cfg_value;
    uint64_t n;
    uint64_t i;

    op = g_xhci.mmio_virt + g_xhci.caplength;
    sc_addr = op + XHCI_OP_PORTSC + port * XHCI_PORTSC_STRIDE;
    sc = mmio_read32(sc_addr);
    if (!(sc & XHCI_PORTSC_CCS)) return 0;
    if (!(sc & XHCI_PORTSC_PED)) return 0;
    for (i = 0; i < 500000; i++)
        __asm__ volatile ("pause");
    speed = (sc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
    if (usb_dev_count >= 8) return 0;
    if (xhci_enable_slot(&slot) < 0) return -1;
    if (!slot || slot > 255) return -1;
    n = usb_dev_count++;
    dev = &usb_devs[n];
    memset(dev, 0, sizeof(*dev));
    dev->used = 1;
    dev->slot = (uint8_t)slot;
    dev->port = (uint8_t)port;
    dev->speed = (uint8_t)speed;
    dev->mps = (speed == 3) ? 64 : 8;
    if (xhci_address_device(dev) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    if (xhci_get_descriptor(dev, 1, 0, 0) < 0) {
        KERNEL_INIT_LOG("USB: Port %u: zero-length probe failed\n", port);
    }
    if (xhci_get_descriptor(dev, 1, 0, 8) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    desc = (uint8_t *)dev->xfer;
    mps = desc[7];
    if (mps != dev->mps && mps >= 8 && mps <= 64) {
        if (xhci_evaluate_mps(dev, mps) < 0) {
            dev->used = 0;
            usb_dev_count--;
            return -1;
        }
    }
    if (xhci_get_descriptor(dev, 1, 0, 18) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    desc = (uint8_t *)dev->xfer;
    dev->vid = (uint16_t)desc[8] | ((uint16_t)desc[9] << 8);
    dev->pid = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);
    dev->dev_class = desc[4];
    if (xhci_get_descriptor(dev, 2, 0, 9) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    desc = (uint8_t *)dev->xfer;
    total = (uint64_t)desc[2] | ((uint64_t)desc[3] << 8);
    if (total < 9 || total > PAGE_SIZE) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    cfg_value = desc[5];
    KERNEL_INIT_LOG("USB: Port %u: VID=0x%04X PID=0x%04X class=%u interfaces=%u\n",
                    port, dev->vid, dev->pid, dev->dev_class, desc[4]);
    if (xhci_get_descriptor(dev, 2, 0, total) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    xhci_parse_hid(dev, (uint8_t *)dev->xfer, total);
    if (dev->has_hid && xhci_configure_hid(dev) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    if (xhci_set_configuration(dev, cfg_value) < 0) {
        dev->used = 0;
        usb_dev_count--;
        return -1;
    }
    if (dev->has_hid) {
        if (xhci_hid_ctrl(dev, 0x0B, 0) < 0 ||
            xhci_hid_ctrl(dev, 0x0A, 0) < 0) {
            KERNEL_INIT_LOG("USB: Port %u: HID boot setup failed\n", port);
            dev->used = 0;
            usb_dev_count--;
            return -1;
        }
    }
    if (dev->inctx) {
        pfa_free(dev->inctx_phys);
        dev->inctx = 0;
        dev->inctx_phys = 0;
    }
    dev->configured = 1;
    return 0;
}

static void xhci_hid_repeat(void) {
#if CONFIG_DRIVER_USB_HID_KBD
    uint64_t n;

    for (n = 0; n < usb_dev_count; n++) {
        usb_dev_t *dev = &usb_devs[n];
        if (!dev->used || !dev->has_hid || !dev->configured ||
            dev->proto != 1) continue;
        usb_hid_kbd_repeat_one(&dev->hid_kbd);
    }
#endif
}

static void xhci_hid_report(usb_dev_t *dev) {
    (void)dev;
#if CONFIG_DRIVER_USB_HID_KBD
    if (dev->proto == 1) {
        usb_hid_kbd_report(&dev->hid_kbd, (uint8_t *)dev->ibuf);
        return;
    }
#endif
#if CONFIG_DRIVER_USB_HID_MOUSE
    if (dev->proto != 1)
        usb_hid_mouse_report((uint8_t *)dev->ibuf, dev->iep_mps);
#endif
}

static void xhci_hid_queue(usb_dev_t *dev) {
    uint32_t *trb;
    uint64_t phys;

    if (dev->iidx >= 255) {
        trb = (uint32_t *)(dev->iring + 255 * 16);
        trb[0] = (uint32_t)(dev->iring_phys & 0xFFFFFFFFULL);
        trb[1] = (uint32_t)((dev->iring_phys >> 32) & 0xFFFFFFFFULL);
        trb[2] = 0;
        trb[3] = (6u << 10) | (1u << 1) | (dev->icycle & 0x1);
        dev->icycle ^= 0x1;
        dev->iidx = 0;
    }
    phys = dev->iring_phys + dev->iidx * 16;
    trb = (uint32_t *)(dev->iring + dev->iidx * 16);
    trb[0] = (uint32_t)(dev->ibuf_phys & 0xFFFFFFFFULL);
    trb[1] = (uint32_t)((dev->ibuf_phys >> 32) & 0xFFFFFFFFULL);
    trb[2] = dev->iep_mps;
    trb[3] = (dev->icycle & 0x1) | (1u << 10) | (1u << 5);
    dev->ipend = phys;
    dev->iidx++;
    xhci_doorbell(dev->slot, dev->iep);
}

static void xhci_hid_poll(uint64_t ticks) {
    uint64_t type;
    uint64_t code;
    uint64_t ptr;
    uint64_t slot;
    uint64_t n;

    (void)ticks;
    while (xhci_next_event(&type, &code, &ptr, &slot)) {
        if (type != 32 || !ptr) continue;
        for (n = 0; n < usb_dev_count; n++) {
            usb_dev_t *dev = &usb_devs[n];
            if (!dev->used || !dev->has_hid || !dev->configured ||
                ptr != dev->ipend) continue;
            dev->ipend = 0;
            if (code == 1 || code == 13)
                xhci_hid_report(dev);
            xhci_hid_queue(dev);
            break;
        }
    }
    xhci_erdp_update();
    xhci_hid_repeat();
}

static void KERNEL_INIT xhci_enumerate(void) {
    uint64_t n;

    for (n = 0; n < g_xhci.max_ports; n++)
        xhci_enumerate_port(n);
}

int KERNEL_INIT xhci_init(void) {
    uint64_t i;
    uint64_t op;
    uint32_t hcs1;
    uint32_t cmd;
    uint64_t polled = 0;

    memset(&g_xhci, 0, sizeof(g_xhci));
    if (xhci_probe() < 0) return -1;

    for (i = 0; i < XHCI_MMIO_PAGES; i++) {
        vmm_map_page(XHCI_MMIO_VIRT + i * PAGE_SIZE,
                     (g_xhci.mmio_phys & ~(uint64_t)(PAGE_SIZE - 1)) +
                         i * PAGE_SIZE,
                     VMM_PTE_PRESENT | VMM_PTE_WRITE | VMM_PTE_PCD);
    }
    g_xhci.mmio_virt = XHCI_MMIO_VIRT +
        (g_xhci.mmio_phys & (uint64_t)(PAGE_SIZE - 1));

    g_xhci.caplength = mmio_read8(g_xhci.mmio_virt + XHCI_CAP_CAPLENGTH);
    g_xhci.version = mmio_read16(g_xhci.mmio_virt + XHCI_CAP_VERSION);
    hcs1 = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_HCSP1);
    g_xhci.max_slots = (uint8_t)(hcs1 & 0xFF);
    g_xhci.max_ports = (uint8_t)((hcs1 >> 24) & 0xFF);
    KERNEL_INIT_LOG("XHCI: MMIO = 0x%016lX, version %u.%u, %u slots, %u ports\n",
                    g_xhci.mmio_phys, g_xhci.version >> 8,
                    g_xhci.version & 0xFF, g_xhci.max_slots,
                    g_xhci.max_ports);

    op = g_xhci.mmio_virt + g_xhci.caplength;
    if (!(mmio_read32(op + XHCI_OP_USBSTS) & XHCI_USBSTS_HALTED)) {
        cmd = mmio_read32(op + XHCI_OP_USBCMD);
        mmio_write32(op + XHCI_OP_USBCMD, cmd & ~XHCI_USBCMD_RS);
        if (xhci_wait(op + XHCI_OP_USBSTS, XHCI_USBSTS_HALTED,
                      XHCI_USBSTS_HALTED) < 0) {
            KERNEL_INIT_LOG("XHCI: Halt timeout\n");
            return -1;
        }
    }
    cmd = mmio_read32(op + XHCI_OP_USBCMD);
    mmio_write32(op + XHCI_OP_USBCMD, cmd | XHCI_USBCMD_HCRST);
    if (xhci_wait(op + XHCI_OP_USBCMD, XHCI_USBCMD_HCRST, 0) < 0) {
        KERNEL_INIT_LOG("XHCI: Reset timeout\n");
        return -1;
    }
    KERNEL_INIT_LOG("XHCI: Controller reset complete\n");
    if (xhci_setup_operational() < 0) return -1;
    xhci_scan_ports();
    xhci_enumerate();
    if (usb_dev_count == 0) {
        while (g_dma_count > 0)
            pfa_free(g_dma_phys[--g_dma_count]);
        op = g_xhci.mmio_virt + g_xhci.caplength;
        cmd = mmio_read32(op + XHCI_OP_USBCMD);
        mmio_write32(op + XHCI_OP_USBCMD, cmd & ~XHCI_USBCMD_RS);
        xhci_wait(op + XHCI_OP_USBSTS, XHCI_USBSTS_HALTED,
                  XHCI_USBSTS_HALTED);
        return 0;
    }
    for (i = 0; i < usb_dev_count; i++) {
        usb_dev_t *dev = &usb_devs[i];
        if (!dev->used || !dev->has_hid || !dev->configured) continue;
        xhci_hid_queue(dev);
        if (!polled) {
            pit_register_callback(xhci_hid_poll, 1, false);
            polled = 1;
        }
    }
    return 0;
}

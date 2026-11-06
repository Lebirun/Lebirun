#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/io.h>
#include <lebirun/drivers/usb/xhci.h>
#include <lebirun/drivers/usb/hid.h>
#include <lebirun/pit.h>
#include <lebirun/smp.h>
#include <lebirun/spinlock.h>
#include <lebirun/mutex.h>
#include <lebirun/blkdev.h>
#include <lebirun/devfs.h>
#include <lebirun/timekeeping.h>
#include <lebirun/lke.h>

#define XHCI_MMIO_VIRT (KERNEL_VMA + 0x37200000ULL)
#define XHCI_MMIO_PAGES 4u
#define XHCI_RING_TRBS 252u
#define XHCI_STREAM_OFF 0xFC0u

#if CONFIG_DRIVER_USB_STORAGE || CONFIG_DRIVER_USB_HOTPLUG
#define XHCI_RUNTIME
#else
#define XHCI_RUNTIME KERNEL_INIT
#endif
#define XHCI_ENUM USB_HOTPLUG_TEXT
#define XHCI_PORTSC_KEEP 0x0E00C200u
#define XHCI_PORTSC_CHANGES 0x00FE0000u
#define XHCI_PORTSC_CSC (1u << 17)

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
    uint8_t context_size;
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
    uint64_t xfer;
    uint64_t xfer_phys;
    uint64_t inctx;
    uint64_t inctx_phys;
    uint64_t cmd_pend;
    uint64_t cmd_slot;
    volatile uint8_t cmd_busy;
    uint8_t cmd_code;
    uint8_t running;
    uint64_t port_chg[4];
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

static uint64_t *g_dma_phys;
static uint64_t g_dma_count;

static uint64_t XHCI_ENUM xhci_page(uint64_t **pages, uint64_t *count,
                                    uint64_t *phys_out) {
    uint64_t *list;
    uint64_t phys;

    list = (uint64_t *)krealloc(*pages, (*count + 1) * sizeof(uint64_t));
    if (!list) return 0;
    *pages = list;
    phys = (uint64_t)pmm_alloc_mapped_page();
    if (!phys) return 0;
    memset((void *)(phys + KERNEL_VMA), 0, PAGE_SIZE);
    list[(*count)++] = phys;
    if (phys_out) *phys_out = phys;
    return phys + KERNEL_VMA;
}

static uint64_t KERNEL_INIT xhci_alloc_dma(uint64_t *phys_out) {
    return xhci_page(&g_dma_phys, &g_dma_count, phys_out);
}

static void XHCI_ENUM xhci_free_pages(uint64_t *pages, uint64_t count) {
    while (count)
        pfa_free(pages[--count]);
    kfree(pages);
}

void XHCI_ENUM xhci_delay(uint64_t ms) {
    extern int scheduler_initialized;
    uint64_t end;

    __asm__ volatile ("pushf; pop %0" : "=r"(end) : : "memory");
    if (scheduler_initialized && current_task && (end & 0x200)) {
        sleep_ms(ms);
        return;
    }
    if (tsc_available()) {
        end = tsc_get_ns() + ms * 1000000ULL;
        while (tsc_get_ns() < end)
            __asm__ volatile ("pause");
        return;
    }
    for (end = ms * 20000; end; end--)
        __asm__ volatile ("pause");
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
    g_xhci.context_size = (hcc & (1u << 2)) ? 64 : 32;
    if (xhci_take_ownership() < 0) return -1;

    dcbaa = xhci_alloc_dma(&dcbaa_phys);
    if (!dcbaa) return -1;
    g_xhci.dcbaa = dcbaa;
    g_xhci.dcbaa_phys = dcbaa_phys;
    hcs2 = mmio_read32(g_xhci.mmio_virt + XHCI_CAP_HCSP2);
    nscratch = (uint64_t)(((hcs2 >> 21) & 0x1F) << 5) |
               (uint64_t)((hcs2 >> 27) & 0x1F);
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
    g_xhci.xfer = xhci_alloc_dma(&g_xhci.xfer_phys);
    g_xhci.inctx = xhci_alloc_dma(&g_xhci.inctx_phys);
    if (!g_xhci.xfer || !g_xhci.inctx) return -1;

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
    for (n = 0; CONFIG_DRIVER_USB_HOTPLUG && n < g_xhci.max_ports; n++) {
        sc_addr = op + XHCI_OP_PORTSC + n * XHCI_PORTSC_STRIDE;
        sc = mmio_read32(sc_addr);
        if (!(sc & XHCI_PORTSC_PP))
            mmio_write32(sc_addr, (sc & XHCI_PORTSC_KEEP) | XHCI_PORTSC_PP);
    }
    if (CONFIG_DRIVER_USB_HOTPLUG) xhci_delay(20);
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

typedef struct xhci_ring {
    struct xhci_ring *next;
    uint64_t virt;
    uint64_t phys;
    uint64_t lo;
    uint64_t hi;
    uint16_t idx;
    uint16_t mps;
    uint8_t cycle;
    uint8_t slot;
    uint8_t dci;
    uint8_t addr;
    uint8_t burst;
    uint8_t stream;
    uint8_t ival;
    uint8_t notify;
    volatile uint8_t busy;
    volatile uint8_t code;
} xhci_ring_t;

struct usb_dev {
    struct usb_dev *next;
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    uint8_t depth;
    uint8_t tt_slot;
    uint8_t tt_port;
    uint8_t is_hub;
    uint8_t nports;
    uint8_t ttt;
    uint32_t route;
    uint16_t vid;
    uint16_t pid;
    uint8_t dev_class;
    uint16_t mps;
    xhci_ring_t ep0;
    xhci_ring_t *rings;
    uint64_t *pages;
    uint64_t npages;
    void *storage;
    uint8_t nrings;
    uint8_t hub_armed;
    uint8_t hub_pass;
    uint8_t hub_errors;
    volatile uint8_t gone;
    volatile uint8_t hid_done;
    volatile uint8_t hid_code;
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
    uint8_t has_abs;
    uint16_t abs_x_off;
    uint8_t abs_x_len;
    uint16_t abs_y_off;
    uint8_t abs_y_len;
    uint32_t abs_max;
};

static usb_dev_t *usb_devs;
static xhci_ring_t *xhci_pending;
static spinlock_t xhci_lock;
static mutex_t xhci_cmd_lock;
#if CONFIG_DRIVER_USB_HOTPLUG
static task_t *xhci_hp_task;
static int xhci_hp_cb;
static volatile int xhci_hp_pending;
static volatile int xhci_hp_kick;

static void xhci_hp_notify(void) {
    xhci_hp_pending = 1;
    xhci_hp_kick = 1;
}
#endif

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

static uint64_t XHCI_RUNTIME xhci_lock_irq(void) {
    uint64_t flags;

    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    spin_lock(&xhci_lock);
    return flags;
}

static void XHCI_RUNTIME xhci_unlock_irq(uint64_t flags) {
    spin_unlock(&xhci_lock);
    if (flags & 0x200) __asm__ volatile ("sti" ::: "memory");
}

static void xhci_unlink(xhci_ring_t *r) {
    xhci_ring_t **pp;

    for (pp = &xhci_pending; *pp; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    }
    r->busy = 0;
}

static void xhci_events(void) {
    uint64_t type;
    uint64_t code;
    uint64_t ptr;
    uint64_t slot;
    xhci_ring_t *r;
    usb_dev_t *dev;

    while (xhci_next_event(&type, &code, &ptr, &slot)) {
        smp_tlb_flush_poll();
        if (type == 33) {
            if (g_xhci.cmd_busy && ptr == g_xhci.cmd_pend) {
                g_xhci.cmd_code = (uint8_t)code;
                g_xhci.cmd_slot = slot;
                g_xhci.cmd_busy = 0;
            }
            continue;
        }
#if CONFIG_DRIVER_USB_HOTPLUG
        if (type == 34 && (ptr >> 24) & 0xFF) {
            ptr = ((ptr >> 24) & 0xFF) - 1;
            g_xhci.port_chg[ptr / 64] |= 1ULL << (ptr % 64);
            xhci_hp_notify();
            continue;
        }
#endif
        if (type != 32 || !ptr) continue;
        for (r = xhci_pending; r && (ptr < r->lo || ptr > r->hi); r = r->next);
        if (r) {
            r->code = (uint8_t)code;
            xhci_unlink(r);
#if CONFIG_DRIVER_USB_HOTPLUG
            if (r->notify) xhci_hp_notify();
#endif
            continue;
        }
        for (dev = usb_devs; dev; dev = dev->next) {
            if (ptr != dev->ipend) continue;
            dev->ipend = 0;
            dev->hid_code = (uint8_t)code;
            dev->hid_done = 1;
            break;
        }
    }
    xhci_erdp_update();
}

static int XHCI_RUNTIME xhci_wait_busy(volatile uint8_t *busy,
                                       uint64_t ms) {
    extern int scheduler_initialized;
    uint64_t end;
    uint64_t spins;
    uint64_t flags;

    end = tsc_available() ? tsc_get_ns() + ms * 1000000ULL : 0;
    for (spins = 0;; spins++) {
        flags = xhci_lock_irq();
        xhci_events();
        xhci_unlock_irq(flags);
        if (!*busy) return 0;
        if (end ? tsc_get_ns() >= end : spins >= ms * 20000) return -1;
        if (spins >= 16384 && !(spins & 1023) && scheduler_initialized &&
            current_task && (flags & 0x200))
            schedule();
        else
            __asm__ volatile ("pause");
    }
}

static uint64_t XHCI_RUNTIME xhci_cmd_submit(uint64_t lo, uint64_t hi,
                                             uint64_t status,
                                             uint64_t ctrl) {
    uint32_t *trb;
    uint64_t phys;

    if (g_xhci.cmd_idx >= 255) {
        trb = (uint32_t *)(g_xhci.cmd_ring + 255 * 16);
        trb[3] = (XHCI_TRB_TYPE_LINK << 10) | XHCI_LINK_TC |
                 (uint32_t)(g_xhci.cmd_cycle & 0x1);
        g_xhci.cmd_cycle ^= 0x1;
        g_xhci.cmd_idx = 0;
    }
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

static int XHCI_RUNTIME xhci_command(uint64_t lo, uint64_t status,
                                     uint64_t ctrl, uint64_t *slot_out) {
    uint64_t flags;
    int ret;

    mutex_lock(&xhci_cmd_lock);
    flags = xhci_lock_irq();
    g_xhci.cmd_busy = 1;
    g_xhci.cmd_pend = xhci_cmd_submit(lo, lo >> 32, status, ctrl);
    xhci_unlock_irq(flags);
    ret = xhci_wait_busy(&g_xhci.cmd_busy, 1000);
    g_xhci.cmd_busy = 0;
    if (ret < 0) {
        printf("XHCI: Command timeout\n");
    } else if (g_xhci.cmd_code != 1) {
        printf("XHCI: Command failed, code %u\n", g_xhci.cmd_code);
        ret = -1;
    } else if (slot_out) {
        *slot_out = g_xhci.cmd_slot;
    }
    mutex_unlock(&xhci_cmd_lock);
    return ret;
}

static void XHCI_RUNTIME xhci_ring_room(xhci_ring_t *r, uint32_t n) {
    uint32_t *trb;

    if (r->idx + n < XHCI_RING_TRBS) return;
    trb = (uint32_t *)(r->virt + (uint64_t)r->idx * 16);
    trb[0] = (uint32_t)r->phys;
    trb[1] = (uint32_t)(r->phys >> 32);
    trb[2] = 0;
    trb[3] = (XHCI_TRB_TYPE_LINK << 10) | XHCI_LINK_TC | r->cycle;
    r->cycle ^= 1;
    r->idx = 0;
}

static uint64_t XHCI_RUNTIME xhci_ring_put(xhci_ring_t *r, uint64_t ptr,
                                           uint32_t status, uint32_t ctrl) {
    uint32_t *trb = (uint32_t *)(r->virt + (uint64_t)r->idx * 16);

    trb[0] = (uint32_t)ptr;
    trb[1] = (uint32_t)(ptr >> 32);
    trb[2] = status;
    __asm__ volatile ("" ::: "memory");
    trb[3] = ctrl | r->cycle;
    return r->phys + (uint64_t)r->idx++ * 16;
}

static void XHCI_RUNTIME xhci_ring_go(xhci_ring_t *r, uint64_t lo,
                                      uint64_t hi) {
    r->lo = lo;
    r->hi = hi;
    r->code = 0;
    r->busy = 1;
    r->next = xhci_pending;
    xhci_pending = r;
    xhci_doorbell(r->slot, r->dci | ((uint32_t)r->stream << 16));
}

static uint8_t XHCI_RUNTIME xhci_ring_wait(xhci_ring_t *r, uint64_t ms) {
    uint64_t flags;

    if (xhci_wait_busy(&r->busy, ms) == 0) return r->code;
    flags = xhci_lock_irq();
    xhci_unlink(r);
    xhci_unlock_irq(flags);
    return 0;
}

static void XHCI_RUNTIME xhci_fill_ep(uint32_t *p, xhci_ring_t *r) {
    uint64_t deq = r->phys + (uint64_t)r->idx * 16;
    uint64_t *sa = (uint64_t *)(r->virt + XHCI_STREAM_OFF);

    memset(p, 0, 32);
    p[1] = ((uint32_t)r->mps << 16) | ((uint32_t)r->burst << 8) |
           (((r->addr & 0x80 ? 4u : 0u) + (r->ival ? 3u : 2u)) << 3) |
           (3u << 1);
    if (r->stream) {
        sa[2] = deq | r->cycle | (1u << 1);
        sa[3] = 0;
        deq = r->phys + XHCI_STREAM_OFF;
        p[0] = (1u << 10) | (1u << 15);
    } else {
        deq |= r->cycle;
    }
    if (r->ival) p[0] |= (uint32_t)(r->ival - 1) << 16;
    p[2] = (uint32_t)deq;
    p[3] = (uint32_t)(deq >> 32);
    p[4] = r->ival ? (uint32_t)r->mps | ((uint32_t)r->mps << 16) : 3072;
}

static int XHCI_RUNTIME xhci_ep_reinit(usb_dev_t *dev, xhci_ring_t *r) {
    uint64_t csz = g_xhci.context_size;
    uint64_t ids = ((uint64_t)dev->slot << 24) | ((uint64_t)r->dci << 16);
    uint32_t state;
    uint32_t *in;
    uint64_t phys;
    int ret;

    if (dev->gone) return -1;
    state = *(volatile uint32_t *)(dev->outctx + r->dci * csz) & 7;
    if (state == 2) {
        if (xhci_command(0, 0, (14u << 10) | ids, 0) < 0) return -1;
        return xhci_command((r->phys + (uint64_t)r->idx * 16) | r->cycle |
                            (r->stream ? 2u : 0u),
                            (uint64_t)r->stream << 16, (16u << 10) | ids, 0);
    }
    if (r->dci == 1) return 0;
    if (state == 1 && xhci_command(0, 0, (15u << 10) | ids, 0) < 0)
        return -1;
    phys = (uint64_t)pmm_alloc_mapped_page();
    if (!phys) return -1;
    in = (uint32_t *)(phys + KERNEL_VMA);
    memset(in, 0, PAGE_SIZE);
    in[0] = 1u << r->dci;
    in[1] = (1u << r->dci) | 1u;
    memcpy(in + csz / 4, (void *)dev->outctx, csz);
    xhci_fill_ep(in + (r->dci + 1) * csz / 4, r);
    ret = xhci_command(phys, 0, (12u << 10) | ((uint64_t)dev->slot << 24), 0);
    pfa_free(phys);
    return ret;
}

static int XHCI_ENUM xhci_enable_slot(uint64_t *slot_out) {
    return xhci_command(0, 0, 9u << 10, slot_out);
}

static void XHCI_ENUM xhci_build_input_ctx(usb_dev_t *dev, uint64_t mps,
                                           uint64_t add_mask) {
    uint64_t ctx;
    uint32_t *p;

    ctx = g_xhci.inctx;
    p = (uint32_t *)ctx;
    p[0] = 0;
    p[1] = (uint32_t)(add_mask & 0xFFFFFFFFULL);
    p = (uint32_t *)(ctx + g_xhci.context_size);
    p[0] = ((31u - (uint32_t)__builtin_clz((uint32_t)add_mask | 2u)) << 27) |
           ((uint32_t)dev->speed << 20) |
           ((uint32_t)dev->is_hub << 26) | dev->route;
    p[1] = ((uint32_t)(dev->port + 1) << 16) | ((uint32_t)dev->nports << 24);
    p[2] = dev->tt_slot | ((uint32_t)dev->tt_port << 8) |
           ((uint32_t)dev->ttt << 16);
    p[3] = 0;
    p = (uint32_t *)(ctx + 2 * g_xhci.context_size);
    p[0] = 0;
    p[1] = ((uint64_t)mps << 16) | (4u << 3) | (3u << 1);
    p[2] = (uint32_t)((dev->ep0.phys + dev->ep0.idx * 16) & 0xFFFFFFFFULL) |
            (dev->ep0.cycle & 0x1);
    p[3] = (uint32_t)(((dev->ep0.phys + dev->ep0.idx * 16) >> 32) & 0xFFFFFFFFULL);
    p[4] = 8 | ((uint64_t)mps << 16);
    p[5] = 0;
    p[6] = 0;
    p[7] = 0;
}

static int XHCI_ENUM xhci_address_device(usb_dev_t *dev) {
    uint64_t *dcbaa;

    dev->ep0.virt = xhci_page(&dev->pages, &dev->npages, &dev->ep0.phys);
    if (!dev->ep0.virt) return -1;
    dev->ep0.idx = 0;
    dev->ep0.cycle = 1;
    dev->ep0.slot = dev->slot;
    dev->ep0.dci = 1;
    dev->outctx = xhci_page(&dev->pages, &dev->npages, &dev->outctx_phys);
    if (!dev->outctx) return -1;
    dcbaa = (uint64_t *)g_xhci.dcbaa;
    dcbaa[dev->slot] = dev->outctx_phys;
    xhci_build_input_ctx(dev, dev->mps, 0x3);
    return xhci_command(g_xhci.inctx_phys, 0,
                        (11u << 10) | ((uint64_t)dev->slot << 24), 0);
}

static int XHCI_ENUM xhci_evaluate_mps(usb_dev_t *dev, uint64_t mps) {
    xhci_build_input_ctx(dev, mps, 0x3);
    if (xhci_command(g_xhci.inctx_phys, 0,
                     (13u << 10) | ((uint64_t)dev->slot << 24), 0) < 0)
        return -1;
    dev->mps = (uint16_t)mps;
    return 0;
}

static int XHCI_RUNTIME xhci_control(usb_dev_t *dev, uint8_t *setup,
                                     uint64_t len, uint64_t dir_in) {
    xhci_ring_t *r = &dev->ep0;
    uint64_t flags;
    uint64_t pkt;
    uint64_t lo;
    uint64_t hi;
    uint8_t code;

    memcpy(&pkt, setup, sizeof(pkt));
    flags = xhci_lock_irq();
    if (dev->gone) {
        xhci_unlock_irq(flags);
        return -1;
    }
    xhci_ring_room(r, 3);
    lo = xhci_ring_put(r, pkt, 8, (2u << 10) | (1u << 6) | (1u << 4) |
                       (uint32_t)((len ? (dir_in ? 3u : 2u) : 0u) << 16));
    if (len)
        xhci_ring_put(r, g_xhci.xfer_phys, (uint32_t)(len & 0x1FFFFULL),
                      (3u << 10) | (1u << 4) | (dir_in ? 1u << 16 : 0u));
    hi = xhci_ring_put(r, 0, 0, (4u << 10) | (1u << 5) |
                       (!len || !dir_in ? 1u << 16 : 0u));
    xhci_ring_go(r, lo, hi);
    xhci_unlock_irq(flags);
    code = xhci_ring_wait(r, 2000);
    if (code == 1) return 0;
    if (dev->gone) return -1;
    if (code) printf("XHCI: Transfer failed, code %u\n", code);
    else printf("XHCI: Transfer timeout\n");
    if (code == 6) xhci_ep_reinit(dev, r);
    return -1;
}

static int XHCI_ENUM xhci_request(usb_dev_t *dev, uint8_t type,
                                  uint8_t req, uint16_t value,
                                  uint16_t index, uint16_t len) {
    uint8_t setup[8];

    setup[0] = type;
    setup[1] = req;
    setup[2] = (uint8_t)(value & 0xFF);
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)(index & 0xFF);
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);
    if (len) memset((void *)g_xhci.xfer, 0, len);
    return xhci_control(dev, setup, len, type & 0x80);
}

static int XHCI_ENUM xhci_get_descriptor(usb_dev_t *dev, uint64_t dtype,
                                         uint64_t dindex, uint64_t len) {
    uint32_t *octx;

    if (xhci_request(dev, 0x80, 6, (uint16_t)((dtype << 8) | dindex), 0,
                     (uint16_t)len) < 0) {
        octx = (uint32_t *)(dev->outctx + g_xhci.context_size);
        USB_LOG("XHCI: FAIL slot=%u ep0 out state=%u deq=%lX\n",
                dev->slot, (uint32_t)(octx[0] & 0x7),
                ((uint64_t)octx[3] << 32) | (uint64_t)(octx[2] & ~0xFULL));
        return -1;
    }
    return 0;
}

static int XHCI_ENUM xhci_hid_parse_abs(uint8_t *d, uint64_t len,
                                        uint16_t *x_off, uint8_t *x_len,
                                        uint16_t *y_off, uint8_t *y_len,
                                        uint32_t *max) {
    uint64_t pos;
    uint64_t bit;
    uint32_t page;
    int32_t lmin;
    int32_t lmax;
    uint32_t size;
    uint32_t count;
    uint32_t pend_page[8];
    uint32_t pend_use[8];
    int pend_n;
    uint32_t loc_min;
    uint32_t loc_min_page;
    int loc_have_min;
    uint16_t xo;
    uint8_t xl;
    uint16_t yo;
    uint8_t yl;
    int32_t xo_lmin;
    int32_t xo_lmax;
    int32_t yo_lmin;
    int32_t yo_lmax;
    int have_x;
    int have_y;
    uint64_t end;

    if (!d || !x_off || !x_len || !y_off || !y_len || !max) return -1;
    pos = 0;
    bit = 0;
    page = 0;
    lmin = 0;
    lmax = 0;
    size = 0;
    count = 0;
    pend_n = 0;
    loc_have_min = 0;
    loc_min = 0;
    loc_min_page = 0;
    xo = 0;
    xl = 0;
    yo = 0;
    yl = 0;
    have_x = 0;
    have_y = 0;
    xo_lmin = xo_lmax = yo_lmin = yo_lmax = 0;
    while (pos < len) {
        uint8_t b;
        uint64_t nb;
        uint64_t v;
        int32_t sv;
        uint64_t type;
        uint64_t tag;
        uint64_t i;

        b = d[pos];
        if (b == 0xFE) {
            if (pos + 2 >= len) return -1;
            if (pos + 3 + d[pos + 1] < pos) return -1;
            pos += 3 + d[pos + 1];
            if (pos > len) return -1;
            continue;
        }
        nb = b & 3;
        if (nb == 3) nb = 4;
        if (pos + 1 + nb > len) return -1;
        v = 0;
        for (i = 0; i < nb; i++) v |= (uint64_t)d[pos + 1 + i] << (i * 8);
        if (nb == 1) sv = (int8_t)v;
        else if (nb == 2) sv = (int16_t)v;
        else sv = (int32_t)v;
        type = (b >> 2) & 3;
        tag = (b >> 4) & 15;
        pos += 1 + nb;
        if (type == 0) {
            if (tag == 8) {
                if (count > 256) return -1;
                if ((v & 0x01) == 0 && (v & 0x04) == 0) {
                    for (i = 0; i < count; i++) {
                        uint64_t off;

                        if ((int)i >= pend_n) break;
                        if (pend_page[i] != 1) continue;
                        if (size < 1 || size > 32) return -1;
                        off = bit + i * size;
                        if (off > 0xFFFF) return -1;
                        if (pend_use[i] == 0x30 && !have_x) {
                            xo = (uint16_t)off;
                            xl = (uint8_t)size;
                            xo_lmin = lmin;
                            xo_lmax = lmax;
                            have_x = 1;
                        } else if (pend_use[i] == 0x31 && !have_y) {
                            yo = (uint16_t)off;
                            yl = (uint8_t)size;
                            yo_lmin = lmin;
                            yo_lmax = lmax;
                            have_y = 1;
                        }
                    }
                }
                if (size > 32 || count > 0xFFFF) return -1;
                bit += (uint64_t)size * count;
                if (bit > 0xFFFF) return -1;
            }
            pend_n = 0;
            loc_have_min = 0;
        } else if (type == 1) {
            if (tag == 0) page = (uint32_t)v;
            else if (tag == 1) lmin = sv;
            else if (tag == 2) lmax = sv;
            else if (tag == 7) {
                size = (uint32_t)v;
                if (size > 32) return -1;
            } else if (tag == 9) count = (uint32_t)v;
        } else if (type == 2) {
            uint32_t upage;
            uint32_t u;

            if (nb == 2 && tag == 0) {
                upage = (uint32_t)(v >> 16);
                u = (uint32_t)(v & 0xFFFF);
            } else {
                upage = page;
                u = (uint32_t)v;
            }
            if (tag == 0) {
                if (pend_n < 8) {
                    pend_page[pend_n] = upage;
                    pend_use[pend_n] = u;
                    pend_n++;
                }
                loc_have_min = 0;
            } else if (tag == 1) {
                loc_min = u;
                loc_min_page = upage;
                loc_have_min = 1;
            } else if (tag == 2) {
                if (!loc_have_min) continue;
                loc_have_min = 0;
                while (pend_n < 8) {
                    pend_page[pend_n] = loc_min_page;
                    pend_use[pend_n] = loc_min;
                    pend_n++;
                    if (loc_min >= u) break;
                    loc_min++;
                }
            }
        }
    }
    if (!have_x || !have_y) return -1;
    if (xo_lmin != 0 || yo_lmin != 0) return -1;
    if (xo_lmax <= 0 || yo_lmax <= 0 || xo_lmax != yo_lmax) return -1;
    end = (uint64_t)xo + xl;
    if ((uint64_t)yo + yl > end) end = (uint64_t)yo + yl;
    if (end > len * 8) return -1;
    *x_off = xo;
    *x_len = xl;
    *y_off = yo;
    *y_len = yl;
    *max = (uint32_t)xo_lmax;
    return 0;
}

static int XHCI_ENUM xhci_parse_hid(usb_dev_t *dev, uint8_t *cfg,
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
            uint8_t abs_pointer = 0;
            candidate = 0;
#if CONFIG_DRIVER_USB_HID_KBD
            want |= (uint8_t)(cfg[off + 7] == 1);
#endif
#if CONFIG_DRIVER_USB_HID_MOUSE
            want |= (uint8_t)(cfg[off + 7] == 2);
            abs_pointer = (uint8_t)(cfg[off + 5] == 3 && cfg[off + 6] == 0 &&
                                    cfg[off + 7] == 0);
#endif
            if ((cfg[off + 5] == 3 && cfg[off + 6] == 1 && want) ||
                abs_pointer) {
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
                USB_LOG("USB: Port %u: HID iface=%u proto=%u ep=%u mps=%u interval=%u\n",
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

static uint32_t XHCI_ENUM xhci_interval(uint8_t speed, uint8_t interval) {
    uint32_t ival;

    if (speed >= 3) return interval ? (interval > 16 ? 15 : interval - 1u) : 0;
    for (ival = 3; ival < 10 && (2u << ival) <= interval * 8u; ival++);
    return ival;
}

static int XHCI_ENUM xhci_configure_hid(usb_dev_t *dev) {
    uint32_t *p;
    uint64_t dci;
    uint64_t base;
    uint32_t ival;

    dev->iring = xhci_page(&dev->pages, &dev->npages, &dev->iring_phys);
    if (!dev->iring) return -1;
    dev->iidx = 0;
    dev->icycle = 1;
    dev->ibuf = xhci_page(&dev->pages, &dev->npages, &dev->ibuf_phys);
    if (!dev->ibuf) return -1;
    dci = dev->iep;
    base = (dci + 1) * (g_xhci.context_size / sizeof(uint32_t));
    xhci_build_input_ctx(dev, dev->mps, 0x1 | (1u << dci));
    p = (uint32_t *)g_xhci.inctx;
    ival = xhci_interval(dev->speed, dev->iep_interval);
    p[base + 0] = ival << 16;
    p[base + 1] = ((uint64_t)dev->iep_mps << 16) | (7u << 3) | (3u << 1);
    p[base + 2] = (uint32_t)(dev->iring_phys & 0xFFFFFFFFULL) | 0x1;
    p[base + 3] = (uint32_t)((dev->iring_phys >> 32) & 0xFFFFFFFFULL);
    p[base + 4] = (uint64_t)dev->iep_mps | ((uint64_t)dev->iep_mps << 16);
    p[base + 5] = 0;
    p[base + 6] = 0;
    p[base + 7] = 0;
    if (xhci_command(g_xhci.inctx_phys, 0,
                     (12u << 10) | ((uint64_t)dev->slot << 24), 0) < 0) {
        USB_LOG("USB: Port %u: configure endpoint %u failed\n",
                dev->port, dev->iep);
        return -1;
    }
    USB_LOG("USB: Port %u: endpoint %u configured\n",
            dev->port, dev->iep);
    return 0;
}

static int XHCI_ENUM xhci_setup_hub(usb_dev_t *dev, uint8_t *cfg,
                                    uint64_t total) {
    uint8_t *d = (uint8_t *)g_xhci.xfer;
    uint32_t mask = 0x1;
    uint64_t pgood;
    uint64_t off;
    xhci_ring_t ep;
    uint8_t n;

    memset(&ep, 0, sizeof(ep));
    for (off = 0; CONFIG_DRIVER_USB_HOTPLUG && off + 7 <= total &&
         cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] == 5 && cfg[off] >= 7 && (cfg[off + 3] & 3) == 3 &&
            (cfg[off + 2] & 0x80)) {
            ep.addr = cfg[off + 2];
            ep.mps = (uint16_t)((cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF);
            ep.ival = (uint8_t)(xhci_interval(dev->speed, cfg[off + 6]) + 1);
            break;
        }
    }
    if (xhci_request(dev, 0xA0, 6, dev->speed >= 4 ? 0x2A00 : 0x2900, 0,
                     dev->speed >= 4 ? 12 : 9) < 0) return -1;
    dev->is_hub = 1;
    dev->nports = d[2];
    dev->ttt = dev->speed == 3 ? (d[3] >> 5) & 3 : 0;
    pgood = (uint64_t)d[5] * 2;
    if (ep.mps && (dev->rings = (xhci_ring_t *)kcalloc(1, sizeof(ep)))) {
        ep.virt = xhci_page(&dev->pages, &dev->npages, &ep.phys);
        ep.cycle = 1;
        ep.slot = dev->slot;
        ep.dci = (uint8_t)((ep.addr & 0xF) * 2 + 1);
        ep.notify = 1;
        *dev->rings = ep;
        dev->nrings = ep.virt ? 1 : 0;
    }
    if (dev->nrings) mask |= 1u << ep.dci;
    xhci_build_input_ctx(dev, dev->mps, mask);
    if (dev->nrings)
        xhci_fill_ep((uint32_t *)(g_xhci.inctx + (ep.dci + 1) *
                                  g_xhci.context_size), dev->rings);
    if (xhci_command(g_xhci.inctx_phys, 0,
                     (12u << 10) | ((uint64_t)dev->slot << 24), 0) < 0)
        return -1;
    if (dev->speed >= 4 && xhci_request(dev, 0x20, 12, dev->depth, 0, 0) < 0)
        return -1;
    for (n = 1; n <= dev->nports && n <= 15; n++)
        xhci_request(dev, 0x23, 3, 8, n, 0);
    xhci_delay(pgood + 100);
    USB_LOG("USB: Port %u: hub route=0x%05X with %u ports\n",
            dev->port, dev->route, dev->nports);
    return 0;
}

static int XHCI_ENUM xhci_enumerate_dev(uint8_t port, uint8_t speed,
                                        uint32_t route, uint8_t depth,
                                        uint8_t tt_slot, uint8_t tt_port);

static int XHCI_ENUM xhci_hub_port(usb_dev_t *hub, uint8_t n) {
    uint8_t *st = (uint8_t *)g_xhci.xfer;
    uint16_t status;
    uint8_t speed;
    uint8_t tt_slot;
    uint8_t tt_port;
    uint64_t i;

    if (xhci_request(hub, 0xA3, 0, 0, n, 4) < 0 || !(st[0] & 1)) return -1;
    if (xhci_request(hub, 0x23, 3, 4, n, 0) < 0) return -1;
    for (i = 0; i < 50; i++) {
        xhci_delay(10);
        if (xhci_request(hub, 0xA3, 0, 0, n, 4) < 0) return -1;
        if ((st[2] & 0x10) && !(st[0] & 0x10)) break;
    }
    status = (uint16_t)(st[0] | (st[1] << 8));
    xhci_request(hub, 0x23, 1, 20, n, 0);
    xhci_request(hub, 0x23, 1, 16, n, 0);
    if ((status & 3) != 3) {
        USB_LOG("USB: Hub route=0x%05X port %u: reset failed\n",
                hub->route, n);
        return -1;
    }
    xhci_delay(10);
    if (hub->speed >= 4) speed = 4;
    else if (status & 0x200) speed = 2;
    else if (status & 0x400) speed = 3;
    else speed = 1;
    tt_slot = hub->tt_slot;
    tt_port = hub->tt_port;
    if (hub->speed == 3 && speed < 3) {
        tt_slot = hub->slot;
        tt_port = n;
    }
    return xhci_enumerate_dev(hub->port, speed,
                              hub->route | ((uint32_t)n << (4 * hub->depth)),
                              (uint8_t)(hub->depth + 1), tt_slot, tt_port);
}

static void XHCI_ENUM xhci_drop_dev(usb_dev_t *dev) {
    if (xhci_command(0, 0, (10u << 10) | ((uint64_t)dev->slot << 24), 0) == 0) {
        ((uint64_t *)g_xhci.dcbaa)[dev->slot] = 0;
        xhci_free_pages(dev->pages, dev->npages);
    } else {
        kfree(dev->pages);
    }
    kfree(dev->rings);
    kfree(dev);
}

static void XHCI_ENUM xhci_dev_start(usb_dev_t *dev);

static void XHCI_ENUM xhci_add_dev(usb_dev_t *dev) {
    uint64_t flags;

    flags = xhci_lock_irq();
    dev->next = usb_devs;
    usb_devs = dev;
    xhci_unlock_irq(flags);
    if (g_xhci.running) xhci_dev_start(dev);
}

static int XHCI_RUNTIME xhci_ring_submit(usb_dev_t *dev, xhci_ring_t *r,
                                         void *buf, uint32_t len) {
    uint8_t *p = (uint8_t *)buf;
    uint64_t flags;
    uint64_t seg;
    uint64_t done;
    uint64_t packets;
    uint64_t lo;
    uint64_t hi;
    uint32_t td;
    uint32_t n;

    n = len / PAGE_SIZE + 2;
    if (!len || r->busy || n >= XHCI_RING_TRBS) return -1;
    for (done = 0; done < len; done += seg) {
        seg = PAGE_SIZE - ((uint64_t)(uintptr_t)(p + done) & (PAGE_SIZE - 1));
        if (seg > len - done) seg = len - done;
        if (!blkdev_phys(p + done)) return -1;
    }
    packets = (len + r->mps - 1) / r->mps;
    lo = 0;
    hi = 0;
    flags = xhci_lock_irq();
    if (dev->gone) {
        xhci_unlock_irq(flags);
        return -1;
    }
    xhci_ring_room(r, n);
    for (done = 0; done < len; done += seg) {
        seg = PAGE_SIZE - ((uint64_t)(uintptr_t)(p + done) & (PAGE_SIZE - 1));
        if (seg > len - done) seg = len - done;
        td = done + seg < len ? (uint32_t)(packets - (done + seg) / r->mps) : 0;
        if (td > 31) td = 31;
        hi = xhci_ring_put(r, blkdev_phys(p + done), (uint32_t)seg | (td << 17),
                           (1u << 10) | (r->addr & 0x80 ? 1u << 2 : 0u) |
                           (done + seg < len ? 1u << 4 : 1u << 5));
        if (!done) lo = hi;
    }
    xhci_ring_go(r, lo, hi);
    xhci_unlock_irq(flags);
    return 0;
}

#if CONFIG_DRIVER_USB_STORAGE || CONFIG_DRIVER_USB_HOTPLUG
static int XHCI_RUNTIME xhci_clear_halt(usb_dev_t *dev, xhci_ring_t *r) {
    uint8_t setup[8] = { 0x02, 0x01, 0, 0, 0, 0, 0, 0 };
    uint64_t flags;
    int ret;

    if (dev->gone) return -1;
    flags = xhci_lock_irq();
    xhci_unlink(r);
    xhci_unlock_irq(flags);
    setup[4] = r->addr;
    ret = xhci_ep_reinit(dev, r);
    return xhci_control(dev, setup, 0, 0) < 0 ? -1 : ret;
}
#endif

#if CONFIG_DRIVER_USB_STORAGE
int usb_ctrl(usb_dev_t *dev, uint8_t type, uint8_t req, uint16_t value,
             uint16_t index) {
    uint8_t setup[8] = {
        type, req, (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8), 0, 0
    };

    return xhci_control(dev, setup, 0, 0);
}

int usb_clear_halt(usb_dev_t *dev, uint32_t pipe) {
    if (pipe >= dev->nrings) return -1;
    return xhci_clear_halt(dev, &dev->rings[pipe]);
}

int usb_bulk_submit(usb_dev_t *dev, uint32_t pipe, void *buf, uint32_t len) {
    if (pipe >= dev->nrings) return -1;
    return xhci_ring_submit(dev, &dev->rings[pipe], buf, len);
}

int usb_bulk_wait(usb_dev_t *dev, uint32_t pipe, uint64_t ms) {
    uint8_t code;

    if (pipe >= dev->nrings) return -1;
    code = xhci_ring_wait(&dev->rings[pipe], ms);
    if (code == 1 || code == 13) return 0;
    if (dev->gone) return -1;
    usb_clear_halt(dev, pipe);
    return code == 6 ? 1 : -1;
}

static int XHCI_ENUM xhci_parse_msc(uint8_t *cfg, uint64_t total,
                                    uint8_t proto, xhci_ring_t *eps,
                                    uint8_t *iface, uint8_t *alt) {
    xhci_ring_t cur;
    xhci_ring_t *last;
    uint64_t off;
    uint8_t len;
    uint8_t type;
    uint8_t want;
    uint8_t have;
    uint8_t match;

    want = proto == 0x50 ? 3 : 15;
    have = 0;
    match = 0;
    last = NULL;
    for (off = 0; off + 2 <= total; off += len) {
        len = cfg[off];
        type = cfg[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == 4 && len >= 9) {
            if (have == want) break;
            match = cfg[off + 5] == 8 && cfg[off + 7] == proto;
            have = 0;
            last = NULL;
            *iface = cfg[off + 2];
            *alt = cfg[off + 3];
        } else if (match && type == 5 && len >= 7) {
            last = NULL;
            if ((cfg[off + 3] & 3) != 2) continue;
            memset(&cur, 0, sizeof(cur));
            cur.addr = cfg[off + 2];
            cur.mps = (uint16_t)((cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF);
            last = &cur;
            if (proto == 0x50) {
                last = &eps[cur.addr & 0x80 ? 0 : 1];
                *last = cur;
                have |= cur.addr & 0x80 ? 1 : 2;
            }
        } else if (match && type == 0x30 && len >= 6 && last) {
            last->burst = cfg[off + 2];
            last->stream = cfg[off + 3] & 0x1F;
        } else if (match && type == 0x24 && len >= 4 && last == &cur &&
                   cfg[off + 2] >= 1 && cfg[off + 2] <= 4) {
            eps[cfg[off + 2] - 1] = cur;
            have |= (uint8_t)(1u << (cfg[off + 2] - 1));
        }
    }
    return have == want;
}

static int XHCI_ENUM xhci_msc(usb_dev_t *dev, uint8_t *cfg, uint64_t total,
                              uint8_t cfg_value) {
    xhci_ring_t eps[4];
    xhci_ring_t *r;
    uint32_t mask;
    uint8_t iface;
    uint8_t alt;
    uint8_t maxlun;
    uint8_t i;
    int uas;
    int streams;

    memset(eps, 0, sizeof(eps));
    uas = 0;
    if (!xhci_parse_msc(cfg, total, 0x50, eps, &iface, &alt)) {
        memset(eps, 0, sizeof(eps));
        if (!xhci_parse_msc(cfg, total, 0x62, eps, &iface, &alt)) return 1;
        uas = 1;
    }
    streams = uas && dev->speed >= 4;
    if (streams &&
        !((mmio_read32(g_xhci.mmio_virt + XHCI_CAP_HCCP1) >> 12) & 0xF))
        return -1;
    dev->nrings = uas ? 4 : 2;
    dev->rings = (xhci_ring_t *)kcalloc(dev->nrings, sizeof(*r));
    if (!dev->rings) return -1;
    mask = 1;
    for (i = 0; i < dev->nrings; i++) {
        r = &dev->rings[i];
        *r = eps[i];
        if (!r->mps || (streams && i && !r->stream)) return -1;
        r->stream = (uint8_t)(streams && i);
        r->virt = xhci_page(&dev->pages, &dev->npages, &r->phys);
        if (!r->virt) return -1;
        r->cycle = 1;
        r->slot = dev->slot;
        r->dci = (uint8_t)((r->addr & 0xF) * 2 + (r->addr >> 7));
        mask |= 1u << r->dci;
    }
    xhci_build_input_ctx(dev, dev->mps, mask);
    for (i = 0; i < dev->nrings; i++)
        xhci_fill_ep((uint32_t *)(g_xhci.inctx + (dev->rings[i].dci + 1) *
                                  g_xhci.context_size), &dev->rings[i]);
    if (xhci_command(g_xhci.inctx_phys, 0,
                     (12u << 10) | ((uint64_t)dev->slot << 24), 0) < 0 ||
        xhci_request(dev, 0x00, 9, cfg_value, 0, 0) < 0 ||
        (alt && xhci_request(dev, 0x01, 11, alt, iface, 0) < 0))
        return -1;
    maxlun = 0;
    if (!uas && xhci_request(dev, 0xA1, 0xFE, 0, iface, 1) == 0)
        maxlun = ((uint8_t *)g_xhci.xfer)[0] & 0xF;
    USB_LOG("USB: Port %u: mass storage iface=%u %s\n", dev->port,
            iface, uas ? "UAS" : "BOT");
    dev->storage = usb_storage_attach(dev, iface, uas + streams, maxlun);
    if (!dev->storage) return -1;
    xhci_add_dev(dev);
    return 0;
}
#endif

static int XHCI_ENUM xhci_enumerate_dev(uint8_t port, uint8_t speed,
                                        uint32_t route, uint8_t depth,
                                        uint8_t tt_slot, uint8_t tt_port) {
    usb_dev_t *dev;
    uint8_t *desc = (uint8_t *)g_xhci.xfer;
    uint64_t slot;
    uint64_t mps;
    uint64_t total;
    uint64_t cfg_value;
    uint8_t n;

    if (xhci_enable_slot(&slot) < 0) return -1;
    if (!slot || slot > 255) return -1;
    dev = (usb_dev_t *)kcalloc(1, sizeof(*dev));
    if (!dev) return -1;
    dev->slot = (uint8_t)slot;
    dev->port = port;
    dev->speed = speed;
    dev->route = route;
    dev->depth = depth;
    dev->tt_slot = tt_slot;
    dev->tt_port = tt_port;
    dev->mps = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    if (xhci_address_device(dev) < 0) goto drop;
    if (xhci_get_descriptor(dev, 1, 0, 0) < 0) {
        USB_LOG("USB: Port %u: zero-length probe failed\n", port);
    }
    if (xhci_get_descriptor(dev, 1, 0, 8) < 0) goto drop;
    mps = speed >= 4 ? 1u << (desc[7] & 0xF) : desc[7];
    if (mps != dev->mps && mps >= 8 && mps <= 512 &&
        xhci_evaluate_mps(dev, mps) < 0) goto drop;
    if (xhci_get_descriptor(dev, 1, 0, 18) < 0) goto drop;
    dev->vid = (uint16_t)desc[8] | ((uint16_t)desc[9] << 8);
    dev->pid = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);
    dev->dev_class = desc[4];
    if (xhci_get_descriptor(dev, 2, 0, 9) < 0) goto drop;
    total = (uint64_t)desc[2] | ((uint64_t)desc[3] << 8);
    if (total < 9 || total > PAGE_SIZE) goto drop;
    cfg_value = desc[5];
    USB_LOG("USB: Port %u: VID=0x%04X PID=0x%04X class=%u interfaces=%u\n",
            port, dev->vid, dev->pid, dev->dev_class, desc[4]);
    if (xhci_get_descriptor(dev, 2, 0, total) < 0) goto drop;
    if (dev->dev_class != 9)
        xhci_parse_hid(dev, desc, total);
    if (dev->has_hid && (dev->proto == 2 || dev->proto == 0)) {
        uint16_t xo;
        uint16_t yo;
        uint8_t xl;
        uint8_t yl;
        uint32_t m;
        uint64_t end;

        xo = 0;
        yo = 0;
        xl = 0;
        yl = 0;
        m = 0;
        if (xhci_request(dev, 0x81, 6, 0x2200, dev->iface, 256) == 0) {
            if (xhci_hid_parse_abs(desc, 256, &xo, &xl, &yo, &yl, &m) == 0) {
                end = (uint64_t)xo + xl;
                if ((uint64_t)yo + yl > end) end = (uint64_t)yo + yl;
                if ((end + 7) / 8 <= dev->iep_mps) {
                    dev->has_abs = 1;
                    dev->abs_x_off = xo;
                    dev->abs_x_len = xl;
                    dev->abs_y_off = yo;
                    dev->abs_y_len = yl;
                    dev->abs_max = m;
                    USB_LOG("USB: Port %u: HID absolute axes xoff=%u xlen=%u yoff=%u ylen=%u max=%u\n",
                            dev->port, xo, xl, yo, yl, m);
                }
            }
        }
    }
    if (dev->has_hid && dev->proto == 0 && !dev->has_abs) {
        USB_LOG("USB: Port %u: unsupported HID device, ignoring\n",
                port);
        goto drop;
    }
#if CONFIG_DRIVER_USB_STORAGE
    if (!dev->has_hid && dev->dev_class != 9) {
        int msc = xhci_msc(dev, desc, total, (uint8_t)cfg_value);

        if (msc == 0) return 0;
        if (msc < 0) goto drop;
    }
#endif
    if (dev->has_hid && xhci_configure_hid(dev) < 0) goto drop;
    if (xhci_request(dev, 0x00, 9, (uint16_t)cfg_value, 0, 0) < 0) goto drop;
    if (dev->has_hid) {
        if ((!dev->has_abs &&
             xhci_request(dev, 0x21, 0x0B, 0, dev->iface, 0) < 0) ||
            xhci_request(dev, 0x21, 0x0A, 0, dev->iface, 0) < 0) {
            USB_LOG("USB: Port %u: HID boot setup failed\n", port);
            goto drop;
        }
        xhci_add_dev(dev);
        return 0;
    }
    if (dev->dev_class == 9 && depth < 5 &&
        xhci_setup_hub(dev, desc, total) == 0) {
        mps = 0;
        for (n = 1; n <= dev->nports && n <= 15; n++)
            mps += xhci_hub_port(dev, n) == 0;
        if (CONFIG_DRIVER_USB_HOTPLUG) {
            xhci_add_dev(dev);
            return 0;
        }
        if (mps) {
            kfree(dev->pages);
            kfree(dev->rings);
            kfree(dev);
            return 0;
        }
    }
drop:
    xhci_drop_dev(dev);
    return -1;
}

static int KERNEL_INIT xhci_enumerate_port(uint64_t port) {
    uint64_t sc;
    uint64_t i;

    sc = mmio_read32(g_xhci.mmio_virt + g_xhci.caplength + XHCI_OP_PORTSC +
                     port * XHCI_PORTSC_STRIDE);
    if (!(sc & XHCI_PORTSC_CCS)) return 0;
    if (!(sc & XHCI_PORTSC_PED)) return 0;
    for (i = 0; i < 500000; i++)
        __asm__ volatile ("pause");
    return xhci_enumerate_dev((uint8_t)port,
                              (uint8_t)((sc >> XHCI_PORTSC_SPEED_SHIFT) &
                                        XHCI_PORTSC_SPEED_MASK), 0, 0, 0, 0);
}

static void xhci_hid_repeat(void) {
#if CONFIG_DRIVER_USB_HID_KBD
    usb_dev_t *dev;

    for (dev = usb_devs; dev; dev = dev->next) {
        if (dev->proto == 1)
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
    if (dev->proto != 1) {
        if (dev->has_abs)
            usb_hid_mouse_abs_report((uint8_t *)dev->ibuf, dev->abs_x_off,
                                     dev->abs_x_len, dev->abs_y_off,
                                     dev->abs_y_len, dev->abs_max);
        else
            usb_hid_mouse_report((uint8_t *)dev->ibuf, dev->iep_mps);
    }
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
    usb_dev_t *dev;

    (void)ticks;
    if (spin_trylock(&xhci_lock)) {
        xhci_events();
        smp_tlb_flush_poll();
        for (dev = usb_devs; dev; dev = dev->next) {
            if (!dev->hid_done) continue;
            dev->hid_done = 0;
            if (dev->hid_code == 1 || dev->hid_code == 13)
                xhci_hid_report(dev);
            xhci_hid_queue(dev);
        }
        xhci_hid_repeat();
        spin_unlock(&xhci_lock);
    }
#if CONFIG_DRIVER_USB_HOTPLUG
    if (xhci_hp_kick && xhci_hp_task) {
        xhci_hp_kick = 0;
        wake_task(xhci_hp_task);
    }
#endif
}

#if CONFIG_DRIVER_USB_HOTPLUG
static void xhci_hid_rate(void) {
    usb_dev_t *dev;

    for (dev = usb_devs; dev && !dev->has_hid; dev = dev->next);
    pit_set_callback_interval(xhci_hp_cb, dev ? 1 : pit_freq / 5 + 1);
}
#endif

static void XHCI_ENUM xhci_dev_start(usb_dev_t *dev) {
    uint64_t flags;

    if (dev->has_hid) {
        flags = xhci_lock_irq();
        xhci_hid_queue(dev);
        xhci_unlock_irq(flags);
    }
    if (dev->is_hub && dev->nrings)
        dev->hub_armed = xhci_ring_submit(dev, dev->rings,
            (void *)(dev->rings->virt + XHCI_STREAM_OFF),
            (uint32_t)(dev->nports + 8) / 8) == 0;
#if CONFIG_DRIVER_USB_HOTPLUG
    if (g_xhci.running) xhci_hid_rate();
#endif
}

#if CONFIG_DRIVER_USB_HOTPLUG
static void xhci_remove(usb_dev_t *dev) {
    static uint8_t zero[8];
    usb_dev_t **pp;
    uint64_t flags;
    uint32_t i;

    flags = xhci_lock_irq();
    dev->gone = 1;
    for (pp = &usb_devs; *pp && *pp != dev; pp = &(*pp)->next);
    if (*pp) *pp = dev->next;
    dev->ep0.code = 0;
    xhci_unlink(&dev->ep0);
    for (i = 0; i < dev->nrings; i++) {
        dev->rings[i].code = 0;
        xhci_unlink(&dev->rings[i]);
    }
#if CONFIG_DRIVER_USB_HID_KBD
    if (dev->has_hid && dev->proto == 1)
        usb_hid_kbd_report(&dev->hid_kbd, zero);
#endif
#if CONFIG_DRIVER_USB_HID_MOUSE
    if (dev->has_hid && dev->proto != 1 && !dev->has_abs)
        usb_hid_mouse_report(zero, dev->iep_mps);
#endif
    xhci_unlock_irq(flags);
#if CONFIG_DRIVER_USB_STORAGE
    if (dev->storage) usb_storage_detach(dev->storage);
#endif
    printf("USB: Port %u: device %04X:%04X removed\n", dev->port, dev->vid,
           dev->pid);
    xhci_drop_dev(dev);
    xhci_hid_rate();
}

static void xhci_remove_route(uint8_t port, uint32_t route, uint8_t depth) {
    uint32_t mask = depth ? (1u << (4 * depth)) - 1 : 0;
    usb_dev_t *dev;
    usb_dev_t *pick;

    for (;;) {
        pick = NULL;
        for (dev = usb_devs; dev; dev = dev->next) {
            if (dev->port == port && dev->depth >= depth &&
                (dev->route & mask) == route &&
                (!pick || dev->depth > pick->depth))
                pick = dev;
        }
        if (!pick) return;
        xhci_remove(pick);
    }
}

static void xhci_root_port(uint8_t n) {
    uint64_t addr;
    uint32_t sc;
    uint32_t i;

    addr = g_xhci.mmio_virt + g_xhci.caplength + XHCI_OP_PORTSC +
           (uint64_t)n * XHCI_PORTSC_STRIDE;
    sc = mmio_read32(addr);
    mmio_write32(addr, (sc & XHCI_PORTSC_KEEP) | (sc & XHCI_PORTSC_CHANGES));
    if (!(sc & XHCI_PORTSC_CSC)) return;
    xhci_remove_route(n, 0, 0);
    if (!(sc & XHCI_PORTSC_CCS)) return;
    xhci_delay(100);
    sc = mmio_read32(addr);
    if (!(sc & XHCI_PORTSC_CCS)) return;
    mmio_write32(addr, (sc & XHCI_PORTSC_KEEP) | XHCI_PORTSC_PR);
    for (i = 0; i < 500 && !(sc & XHCI_PORTSC_PRC); i++) {
        xhci_delay(1);
        sc = mmio_read32(addr);
    }
    mmio_write32(addr, (sc & XHCI_PORTSC_KEEP) | (sc & XHCI_PORTSC_CHANGES));
    if (!(sc & XHCI_PORTSC_PED)) return;
    xhci_delay(10);
    xhci_enumerate_dev(n, (uint8_t)((sc >> XHCI_PORTSC_SPEED_SHIFT) &
                                    XHCI_PORTSC_SPEED_MASK), 0, 0, 0, 0);
}

static void xhci_hub_change(usb_dev_t *hub, uint8_t n) {
    uint8_t *st = (uint8_t *)g_xhci.xfer;
    uint16_t status;
    uint16_t change;
    uint8_t b;

    if (xhci_request(hub, 0xA3, 0, 0, n, 4) < 0) return;
    status = (uint16_t)(st[0] | (st[1] << 8));
    change = (uint16_t)(st[2] | (st[3] << 8));
    for (b = 0; b < 8; b++) {
        if (change & (1u << b))
            xhci_request(hub, 0x23, 1, b < 5 ? 16 + b : b == 5 ? 29 : 19 + b,
                         n, 0);
    }
    if (!(change & 1)) return;
    xhci_remove_route(hub->port, hub->route | ((uint32_t)n << (4 * hub->depth)),
                      (uint8_t)(hub->depth + 1));
    if (!(status & 1)) return;
    xhci_delay(100);
    xhci_hub_port(hub, n);
}

static void xhci_hub_event(usb_dev_t *hub) {
    xhci_ring_t *r = hub->rings;
    uint8_t *bits = (uint8_t *)(r->virt + XHCI_STREAM_OFF);
    uint32_t map;
    uint8_t n;

    hub->hub_armed = 0;
    if (r->code == 1 || r->code == 13) {
        hub->hub_errors = 0;
        map = (uint32_t)bits[0] | ((uint32_t)bits[1] << 8);
        for (n = 1; n <= hub->nports && n <= 15; n++) {
            if (map & (1u << n)) xhci_hub_change(hub, n);
        }
    } else if (++hub->hub_errors >= 3 || xhci_clear_halt(hub, r) < 0) {
        return;
    }
    xhci_dev_start(hub);
}

static void xhci_hotplug_work(void) {
    static uint8_t pass;
    uint64_t chg[4];
    uint64_t flags;
    usb_dev_t *dev;
    uint32_t n;

    flags = xhci_lock_irq();
    memcpy(chg, g_xhci.port_chg, sizeof(chg));
    memset(g_xhci.port_chg, 0, sizeof(chg));
    xhci_unlock_irq(flags);
    g_xhci.xfer_phys = (uint64_t)pmm_alloc_mapped_page();
    g_xhci.inctx_phys = (uint64_t)pmm_alloc_mapped_page();
    if (g_xhci.xfer_phys && g_xhci.inctx_phys) {
        g_xhci.xfer = g_xhci.xfer_phys + KERNEL_VMA;
        g_xhci.inctx = g_xhci.inctx_phys + KERNEL_VMA;
        memset((void *)g_xhci.inctx, 0, PAGE_SIZE);
        for (n = 0; n < g_xhci.max_ports; n++) {
            if (chg[n / 64] & (1ULL << (n % 64))) xhci_root_port((uint8_t)n);
        }
        pass++;
        for (;;) {
            for (dev = usb_devs; dev; dev = dev->next) {
                if (dev->is_hub && dev->hub_armed && !dev->rings->busy &&
                    dev->hub_pass != pass)
                    break;
            }
            if (!dev) break;
            dev->hub_pass = pass;
            xhci_hub_event(dev);
        }
    } else {
        flags = xhci_lock_irq();
        for (n = 0; n < 4; n++) g_xhci.port_chg[n] |= chg[n];
        xhci_unlock_irq(flags);
    }
    if (g_xhci.xfer_phys) pfa_free(g_xhci.xfer_phys);
    if (g_xhci.inctx_phys) pfa_free(g_xhci.inctx_phys);
    g_xhci.xfer_phys = 0;
    g_xhci.inctx_phys = 0;
    g_xhci.xfer = 0;
    g_xhci.inctx = 0;
#if CONFIG_DRIVER_USB_STORAGE
    devfs_add_blkdevs();
#endif
}

static void xhci_hotplug_thread(void) {
    for (;;) {
        lock_scheduler();
        if (!xhci_hp_pending) current_task->state = TASK_BLOCKED;
        unlock_scheduler();
        smp_this_cpu()->schedule_force = 1;
        schedule();
        if (__atomic_exchange_n(&xhci_hp_pending, 0, __ATOMIC_ACQ_REL))
            xhci_hotplug_work();
    }
}
#endif

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
    usb_dev_t *dev;
    int hid;

    memset(&g_xhci, 0, sizeof(g_xhci));
    spinlock_init(&xhci_lock);
    mutex_init(&xhci_cmd_lock);
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
    if (!usb_devs && !CONFIG_DRIVER_USB_HOTPLUG) {
        cmd = mmio_read32(op + XHCI_OP_USBCMD);
        mmio_write32(op + XHCI_OP_USBCMD, cmd & ~XHCI_USBCMD_RS);
        xhci_wait(op + XHCI_OP_USBSTS, XHCI_USBSTS_HALTED,
                  XHCI_USBSTS_HALTED);
        xhci_free_pages(g_dma_phys, g_dma_count);
        g_dma_phys = NULL;
        g_dma_count = 0;
        return -1;
    }
    pfa_free(g_xhci.xfer_phys);
    pfa_free(g_xhci.inctx_phys);
    g_xhci.xfer_phys = 0;
    g_xhci.inctx_phys = 0;
    g_xhci.xfer = 0;
    g_xhci.inctx = 0;
    kfree(g_dma_phys);
    g_dma_phys = NULL;
    g_dma_count = 0;
    g_xhci.running = 1;
    hid = 0;
    for (dev = usb_devs; dev; dev = dev->next) {
        xhci_dev_start(dev);
        hid |= dev->has_hid;
    }
#if CONFIG_DRIVER_USB_HOTPLUG
    xhci_hp_cb = pit_register_callback(xhci_hid_poll,
                                       hid ? 1 : pit_freq / 5 + 1, false);
    xhci_hp_task = create_kernel_task(xhci_hotplug_thread, TASK_BLOCKED);
    if (xhci_hp_task) {
        strcpy(xhci_hp_task->name, "usb-hotplug");
        lock_scheduler();
        add_task_to_runqueue(xhci_hp_task);
        unlock_scheduler();
    }
#else
    if (hid) pit_register_callback(xhci_hid_poll, 1, false);
#endif
    return 0;
}

#ifndef __is_kernel
module_init(xhci_init);
LKE_NAME("xhci");
LKE_DESC("xHCI USB host controller and USB device drivers");
LKE_LICENSE("GPLv2");
LKE_VERSION_STR("1.0.0");
#endif

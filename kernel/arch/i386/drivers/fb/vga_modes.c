#include <kernel/drivers/fb/vga_modes.h>
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

enum {
    VGA_SEQ_INDEX      = 0x3C4,
    VGA_SEQ_DATA       = 0x3C5,
    VGA_CRTC_INDEX     = 0x3D4,
    VGA_CRTC_DATA      = 0x3D5,

    PCI_CONFIG_ADDRESS = 0x0CF8,
    PCI_CONFIG_DATA    = 0x0CFC,

    PCI_VGA_CLASS      = 0x0300,
    PCI_VENDOR_CIRRUS  = 0x1013,
    PCI_DEVICE_5446    = 0x00B8
};

static int cirrus_cached = -1;
static uint32_t cirrus_fb_base = 0;
static uint32_t cirrus_vram = 0;

static inline uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    address = (uint32_t)(1u << 31)
            | ((uint32_t)bus << 16)
            | ((uint32_t)(slot & 0x1F) << 11)
            | ((uint32_t)(func & 0x07) << 8)
            | ((uint32_t)offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void vga_seq_write(uint8_t index, uint8_t value) {
    outb(VGA_SEQ_INDEX, index);
    outb(VGA_SEQ_DATA, value);
}

static uint8_t vga_seq_read(uint8_t index) {
    outb(VGA_SEQ_INDEX, index);
    return inb(VGA_SEQ_DATA);
}

static void vga_crtc_write(uint8_t index, uint8_t value) {
    outb(VGA_CRTC_INDEX, index);
    outb(VGA_CRTC_DATA, value);
}

static uint8_t vga_crtc_read(uint8_t index) {
    outb(VGA_CRTC_INDEX, index);
    return inb(VGA_CRTC_DATA);
}

static int pci_find_cirrus(void) {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint32_t id_reg;
    uint32_t class_reg;
    uint16_t vendor;
    uint16_t device;
    uint16_t class_code;
    uint32_t hdr;
    uint32_t bar0;

    for (bus = 0; bus < 255; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                id_reg = pci_read32(bus, slot, func, 0x00);
                vendor = (uint16_t)(id_reg & 0xFFFF);
                if (vendor == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                device = (uint16_t)(id_reg >> 16);
                class_reg = pci_read32(bus, slot, func, 0x08);
                class_code = (uint16_t)(class_reg >> 16);

                if (class_code != PCI_VGA_CLASS) {
                    if (func == 0) {
                        hdr = pci_read32(bus, slot, func, 0x0C);
                        if (!((hdr >> 16) & 0x80)) break;
                    }
                    continue;
                }

                if (vendor == PCI_VENDOR_CIRRUS && device == PCI_DEVICE_5446) {
                    bar0 = pci_read32(bus, slot, func, 0x10);
                    cirrus_fb_base = bar0 & 0xFFFFFFF0u;
                    return 1;
                }

                if (func == 0) {
                    hdr = pci_read32(bus, slot, func, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }
    return 0;
}

static uint32_t cirrus_detect_vram(void) {
    uint8_t sr0f;
    uint32_t size;

    vga_seq_write(0x06, 0x12);

    sr0f = vga_seq_read(0x0F);
    size = 0;

    switch ((sr0f >> 2) & 0x03) {
    case 0: size = 256 * 1024; break;
    case 1: size = 512 * 1024; break;
    case 2: size = 1024 * 1024; break;
    case 3: size = 2048 * 1024; break;
    }

    if (size < 4 * 1024 * 1024) {
        size = 4 * 1024 * 1024;
    }

    return size;
}

int vga_is_cirrus(void) {
    if (cirrus_cached >= 0) return cirrus_cached;
    cirrus_cached = pci_find_cirrus();
    if (cirrus_cached) {
        cirrus_vram = cirrus_detect_vram();
    }
    return cirrus_cached;
}

uint32_t vga_get_framebuffer_base(void) {
    return cirrus_fb_base;
}

uint32_t vga_get_vram_bytes(void) {
    return cirrus_vram;
}

static inline void vga_misc_write(uint8_t value) {
    outb(0x3C2, value);
}

static inline void vga_attr_write(uint8_t index, uint8_t value) {
    inb(0x3DA);
    outb(0x3C0, index);
    outb(0x3C0, value);
}

static inline void vga_gc_write(uint8_t index, uint8_t value) {
    outb(0x3CE, index);
    outb(0x3CF, value);
}

static void vga_set_crtc_timing(uint16_t width, uint16_t height) {
    uint32_t htotal;
    uint32_t hdispend;
    uint32_t hblankstart;
    uint32_t hblankend;
    uint32_t hsyncstart;
    uint32_t hsyncend;
    uint32_t vtotal;
    uint32_t vdispend;
    uint32_t vblankstart;
    uint32_t vblankend;
    uint32_t vsyncstart;
    uint32_t vsyncend;
    uint8_t overflow;
    uint8_t max_scanline;
    uint8_t cr1a_val;
    uint8_t misc_val;

    hdispend = (uint32_t)width / 8 - 1;
    hblankstart = hdispend + 1;
    hsyncstart = hdispend + 2;
    hsyncend = hsyncstart + 12;
    hblankend = hsyncend + 2;
    htotal = hblankend + 2;

    vdispend = (uint32_t)height - 1;
    vblankstart = vdispend + 1;
    vsyncstart = vdispend + 3;
    vsyncend = vsyncstart + 2;
    vblankend = vsyncend + 2;
    vtotal = vblankend + 2;

    misc_val = 0x23;
    if (height >= 400) {
        misc_val |= 0xC0;
    } else {
        misc_val |= 0x40;
    }
    vga_misc_write(misc_val);

    vga_crtc_write(0x00, (uint8_t)(htotal & 0xFF));
    vga_crtc_write(0x01, (uint8_t)(hdispend & 0xFF));
    vga_crtc_write(0x02, (uint8_t)(hblankstart & 0xFF));
    vga_crtc_write(0x03, (uint8_t)(0x80 | (hblankend & 0x1F)));
    vga_crtc_write(0x04, (uint8_t)(hsyncstart & 0xFF));
    vga_crtc_write(0x05, (uint8_t)(((hblankend & 0x20) << 2) | (hsyncend & 0x1F)));
    vga_crtc_write(0x06, (uint8_t)(vtotal & 0xFF));

    overflow = 0x00;
    if (vtotal & 0x100) overflow |= 0x01;
    if (vdispend & 0x100) overflow |= 0x02;
    if (vsyncstart & 0x100) overflow |= 0x04;
    if (vblankstart & 0x100) overflow |= 0x08;
    if (vtotal & 0x200) overflow |= 0x20;
    if (vdispend & 0x200) overflow |= 0x40;
    if (vsyncstart & 0x200) overflow |= 0x80;
    vga_crtc_write(0x07, overflow);

    vga_crtc_write(0x08, 0x00);

    max_scanline = 0x00;
    if (vblankstart & 0x200) max_scanline |= 0x20;
    vga_crtc_write(0x09, max_scanline);

    vga_crtc_write(0x10, (uint8_t)(vsyncstart & 0xFF));
    vga_crtc_write(0x11, (uint8_t)(vsyncend & 0x0F));
    vga_crtc_write(0x12, (uint8_t)(vdispend & 0xFF));
    vga_crtc_write(0x15, (uint8_t)(vblankstart & 0xFF));
    vga_crtc_write(0x16, (uint8_t)(vblankend & 0xFF));

    cr1a_val = vga_crtc_read(0x1A);
    cr1a_val &= 0xC0;
    if (vblankstart & 0x100) cr1a_val |= 0x01;
    if (vtotal & 0x100) cr1a_val |= 0x02;
    if (vdispend & 0x100) cr1a_val |= 0x04;
    if (vsyncstart & 0x100) cr1a_val |= 0x08;
    vga_crtc_write(0x1A, cr1a_val);

    vga_crtc_write(0x17, 0xC3);
}

int vga_set_mode(uint16_t width, uint16_t height, uint16_t bpp, uint32_t *out_pitch) {
    uint32_t pitch;
    uint32_t needed;
    uint8_t sr07_val;
    uint8_t hidden_dac;
    uint32_t offset_val;
    uint8_t cr1b_val;
    uint8_t crtc11;
    volatile uint32_t delay;
    int i;

    if (!vga_is_cirrus()) return -1;
    if (width == 0 || height == 0) return -2;
    if (bpp != 32 && bpp != 24 && bpp != 16) return -2;

    pitch = (uint32_t)width * (uint32_t)(bpp / 8);
    needed = pitch * (uint32_t)height;
    if (cirrus_vram && needed > cirrus_vram) return -4;

    vga_seq_write(0x06, 0x12);

    vga_seq_write(0x00, 0x01);
    for (delay = 0; delay < 10000; delay++) {
        __asm__ volatile("pause");
    }

    vga_seq_write(0x01, 0x21);

    sr07_val = vga_seq_read(0x07);
    sr07_val &= 0xE0;

    if (bpp >= 24) {
        sr07_val |= 0x09;
    } else if (bpp == 16) {
        sr07_val |= 0x07;
    } else {
        sr07_val |= 0x01;
    }

    vga_seq_write(0x07, sr07_val);

    (void)hidden_dac;
    inb(0x3C6);
    inb(0x3C6);
    inb(0x3C6);
    inb(0x3C6);
    hidden_dac = inb(0x3C6);

    inb(0x3C6);
    inb(0x3C6);
    inb(0x3C6);
    inb(0x3C6);

    if (bpp == 32) {
        outb(0x3C6, 0xC5);
    } else if (bpp == 24) {
        outb(0x3C6, 0xC5);
    } else if (bpp == 16) {
        outb(0x3C6, 0xC1);
    } else {
        outb(0x3C6, 0x00);
    }

    crtc11 = vga_crtc_read(0x11);
    vga_crtc_write(0x11, crtc11 & 0x7F);

    vga_set_crtc_timing(width, height);

    offset_val = pitch / 8;
    vga_crtc_write(0x13, (uint8_t)(offset_val & 0xFF));

    cr1b_val = vga_crtc_read(0x1B);
    cr1b_val &= 0xEF;
    if (offset_val & 0x100) {
        cr1b_val |= 0x10;
    }
    vga_crtc_write(0x1B, cr1b_val);

    vga_crtc_write(0x0C, 0x00);
    vga_crtc_write(0x0D, 0x00);

    vga_crtc_write(0x11, (uint8_t)((vga_crtc_read(0x11) & 0x0F) | 0x80));

    vga_gc_write(0x05, 0x40);
    vga_gc_write(0x06, 0x05);

    inb(0x3DA);
    for (i = 0; i < 16; i++) {
        vga_attr_write((uint8_t)i, (uint8_t)i);
    }
    vga_attr_write(0x10, 0x41);
    vga_attr_write(0x11, 0x00);
    vga_attr_write(0x12, 0x0F);
    vga_attr_write(0x13, 0x00);
    vga_attr_write(0x14, 0x00);
    inb(0x3DA);
    outb(0x3C0, 0x20);

    vga_seq_write(0x01, 0x01);

    vga_seq_write(0x00, 0x03);

    for (delay = 0; delay < 50000; delay++) {
        __asm__ volatile("pause");
    }

    if (out_pitch) {
        *out_pitch = pitch;
    }

    return 0;
}

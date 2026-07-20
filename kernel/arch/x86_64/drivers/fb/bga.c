#include <lebirun/drivers/fb/bga.h>
#include <stdint.h>

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

enum {
    VBE_DISPI_IOPORT_INDEX = 0x01CE,
    VBE_DISPI_IOPORT_DATA = 0x01CF,

    VBE_DISPI_INDEX_ID = 0,
    VBE_DISPI_INDEX_XRES = 1,
    VBE_DISPI_INDEX_YRES = 2,
    VBE_DISPI_INDEX_BPP = 3,
    VBE_DISPI_INDEX_ENABLE = 4,
    VBE_DISPI_INDEX_BANK = 5,
    VBE_DISPI_INDEX_VIRT_WIDTH = 6,
    VBE_DISPI_INDEX_VIRT_HEIGHT = 7,
    VBE_DISPI_INDEX_X_OFFSET = 8,
    VBE_DISPI_INDEX_Y_OFFSET = 9,
    VBE_DISPI_INDEX_VIDEO_MEMORY_64K = 0x0A,

    VBE_DISPI_DISABLED = 0x00,
    VBE_DISPI_ENABLED = 0x01,
    VBE_DISPI_LFB_ENABLED = 0x40,
    VBE_DISPI_NOCLEARMEM = 0x80,

    VBE_DISPI_ID0 = 0xB0C0,
    VBE_DISPI_ID5 = 0xB0C5
};

static int bga_cached = -1;

static inline void bga_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static inline uint16_t bga_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int bga_is_available(void) {
    uint16_t id;
    uint16_t readback;

    if (bga_cached >= 0) return bga_cached;

    id = bga_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID0 || id > VBE_DISPI_ID5) {
        bga_cached = 0;
        return 0;
    }

    bga_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    readback = bga_read(VBE_DISPI_INDEX_ID);
    if (readback < VBE_DISPI_ID0 || readback > VBE_DISPI_ID5) {
        bga_cached = 0;
        return 0;
    }

    bga_cached = 1;
    return 1;
}

uint64_t bga_get_vram_bytes(void) {
    uint16_t blocks64k;
    if (!bga_is_available()) return 0;
    blocks64k = bga_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
    if (blocks64k == 0) return 0;
    return (uint64_t)blocks64k * 64u * 1024u;
}

int bga_set_mode(uint16_t width, uint16_t height, uint16_t bpp, uint64_t *out_pitch) {
    uint16_t got_w;
    uint16_t got_h;
    uint16_t got_bpp;
    uint16_t virt_w;
    uint64_t bytespp;
    volatile uint64_t delay;
    if (!bga_is_available()) return -1;
    if (width == 0 || height == 0) return -2;
    if (bpp != 32 && bpp != 24 && bpp != 16) return -2;

    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    for (delay = 0; delay < 100000; delay++) {
        asm volatile("pause");
    }
    bga_write(VBE_DISPI_INDEX_XRES, width);
    bga_write(VBE_DISPI_INDEX_YRES, height);
    bga_write(VBE_DISPI_INDEX_BPP, bpp);
    bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_ENABLE, (uint16_t)(VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM));
    for (delay = 0; delay < 200000; delay++) {
        asm volatile("pause");
    }

    got_w = bga_read(VBE_DISPI_INDEX_XRES);
    got_h = bga_read(VBE_DISPI_INDEX_YRES);
    got_bpp = bga_read(VBE_DISPI_INDEX_BPP);
    virt_w = bga_read(VBE_DISPI_INDEX_VIRT_WIDTH);

    if (got_w != width || got_h != height || got_bpp != bpp) {
        return -3;
    }

    if (out_pitch) {
        bytespp = (uint64_t)(bpp / 8u);
        *out_pitch = (uint64_t)virt_w * bytespp;
    }

    return 0;
}

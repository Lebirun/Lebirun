#include <stdint.h>
#include <string.h>
#include <lebirun/common.h>
#include <lebirun/io.h>
#include <lebirun/mem_map.h>
#include <lebirun/power.h>
#include <lebirun/watchdog.h>
#include <lebirun/vfs.h>

extern void acpi_read_phys(uint64_t phys_addr, void *buf, uint64_t len);
extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);

static uint16_t pm1a_cnt;
static uint16_t pm1b_cnt;
static uint16_t smi_cmd;
static uint8_t acpi_on;
static uint8_t slp_typa;
static uint8_t slp_typb;
static uint8_t s5_ok;
static uint8_t reset_ok;
static uint8_t reset_space;
static uint8_t reset_value;
static uint64_t reset_addr;

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static void power_spin(uint64_t n) {
    while (n--)
        __asm__ __volatile__("pause");
}

static int KERNEL_INIT aml_byte(const uint8_t **p, const uint8_t *end,
                                uint8_t *out) {
    const uint8_t *q = *p;

    if (q >= end) return 0;
    if (*q == 0x0A) {
        if (++q >= end) return 0;
    } else if (*q == 0xFF || (*q >= 0x0B && *q <= 0x0E)) {
        return 0;
    }
    *out = *q & 7;
    *p = q + 1;
    return 1;
}

static int KERNEL_INIT parse_s5(const uint8_t *buf, uint64_t i, uint64_t n) {
    const uint8_t *p = buf + i + 4;
    const uint8_t *end = buf + n;

    if (buf[i - 1] != 0x08 && (buf[i - 1] != '\\' || buf[i - 2] != 0x08))
        return 0;
    if (p + 3 > end || *p++ != 0x12) return 0;
    p += ((*p >> 6) & 3) + 2;
    if (!aml_byte(&p, end, &slp_typa)) return 0;
    if (!aml_byte(&p, end, &slp_typb)) slp_typb = slp_typa;
    return 1;
}

static void KERNEL_INIT find_s5(uint64_t dsdt) {
    uint8_t buf[256];
    uint32_t len;
    uint64_t off;
    uint64_t n;
    uint64_t i;

    if (!dsdt) return;
    acpi_read_phys(dsdt, buf, 8);
    if (memcmp(buf, "DSDT", 4) != 0) return;
    len = *(uint32_t *)(buf + 4);
    for (off = 34; off < len; off += sizeof(buf) - 32) {
        n = len - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        acpi_read_phys(dsdt + off, buf, n);
        for (i = 2; i + 4 <= n; i++) {
            if (memcmp(buf + i, "_S5_", 4) == 0 && parse_s5(buf, i, n)) {
                s5_ok = 1;
                return;
            }
        }
        if (off + n >= len) break;
    }
}

void KERNEL_INIT power_acpi_init(uint64_t fadt_phys) {
    uint8_t f[196];
    uint32_t len;
    uint64_t dsdt;
    uint64_t x;

    memset(f, 0, sizeof(f));
    len = 0;
    acpi_read_phys(fadt_phys + 4, &len, 4);
    if (len < 116) return;
    acpi_read_phys(fadt_phys, f, len < sizeof(f) ? len : sizeof(f));

    smi_cmd = (uint16_t)*(uint32_t *)(f + 48);
    acpi_on = f[52];
    pm1a_cnt = (uint16_t)*(uint32_t *)(f + 64);
    pm1b_cnt = (uint16_t)*(uint32_t *)(f + 68);
    if (f[172] == 1 && (x = *(uint64_t *)(f + 176))) pm1a_cnt = (uint16_t)x;
    if (f[184] == 1 && (x = *(uint64_t *)(f + 188))) pm1b_cnt = (uint16_t)x;

    reset_space = f[116];
    reset_addr = *(uint64_t *)(f + 120);
    reset_value = f[128];
    reset_ok = len >= 129 && (*(uint32_t *)(f + 112) & (1u << 10)) &&
               reset_addr && reset_space <= 2;

    dsdt = *(uint64_t *)(f + 140);
    if (!dsdt) dsdt = *(uint32_t *)(f + 40);
    find_s5(dsdt);
    KERNEL_INIT_LOG("ACPI: PM1a=0x%x PM1b=0x%x S5=%u reset=%u\n",
                    pm1a_cnt, pm1b_cnt, s5_ok, reset_ok);
}

static void power_prepare(void) {
    vfs_sync_all(0);
    watchdog_disable();
    __asm__ __volatile__("cli");
}

static void acpi_sleep_s5(void) {
    uint64_t i;

    if (!s5_ok || !pm1a_cnt) return;
    if (smi_cmd && acpi_on && !(inw(pm1a_cnt) & 1)) {
        outb(smi_cmd, acpi_on);
        for (i = 0; i < 3000000 && !(inw(pm1a_cnt) & 1); i++)
            __asm__ __volatile__("pause");
    }
    outw(pm1a_cnt, (inw(pm1a_cnt) & ~0x3C00) | (slp_typa << 10));
    if (pm1b_cnt)
        outw(pm1b_cnt, (inw(pm1b_cnt) & ~0x3C00) | (slp_typb << 10));
    outw(pm1a_cnt, (inw(pm1a_cnt) & ~0x3C00) | (slp_typa << 10) | 0x2000);
    if (pm1b_cnt)
        outw(pm1b_cnt, (inw(pm1b_cnt) & ~0x3C00) | (slp_typb << 10) | 0x2000);
    power_spin(10000000);
}

static void acpi_reset(void) {
    uint64_t a = reset_addr;

    if (!reset_ok) return;
    if (reset_space == 0) {
        temp_map_raw(TEMP_SLOT(4), a & ~0xFFFULL);
        *(volatile uint8_t *)(TEMP_SLOT(4) + (a & 0xFFF)) = reset_value;
    } else if (reset_space == 1) {
        outb((uint16_t)a, reset_value);
    } else {
        outl(0xCF8, 0x80000000u | (uint32_t)(((a >> 32) & 0x1F) << 11) |
                    (uint32_t)(((a >> 16) & 7) << 8) | (uint32_t)(a & 0xFC));
        outb((uint16_t)(0xCFC + (a & 3)), reset_value);
    }
    power_spin(10000000);
}

void power_shutdown(void) {
    power_prepare();
    acpi_sleep_s5();
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;)
        __asm__ __volatile__("hlt");
}

void power_reboot(void) {
    uint8_t good;

    power_prepare();
    acpi_reset();

    good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);

    outb(0x0CF9, 0x02);
    outb(0x0CF9, 0x06);

    for (;;)
        __asm__ __volatile__("hlt");
}

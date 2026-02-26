#include <stdint.h>
#include <kernel/io.h>
#include <kernel/power.h>

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

void power_init(void) {
}

void power_shutdown(void) {
    __asm__ __volatile__("cli");
    outw(0x604, 0x2000);
    outb(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;)
        __asm__ __volatile__("hlt");
}

void power_reboot(void) {
    uint8_t good;

    __asm__ __volatile__("cli");

    good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);

    outb(0x0CF9, 0x02);
    outb(0x0CF9, 0x06);

    for (;;)
        __asm__ __volatile__("hlt");
}

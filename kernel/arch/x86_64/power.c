#include <stdint.h>
#include <lebirun/io.h>
#include <lebirun/power.h>
#include <lebirun/watchdog.h>
#include <lebirun/vfs.h>

static void power_prepare(void) {
    vfs_sync_all(0);
    watchdog_disable();
    __asm__ __volatile__("cli");
}

void power_shutdown(void) {
    power_prepare();
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;)
        __asm__ __volatile__("hlt");
}

void power_reboot(void) {
    uint8_t good;

    power_prepare();

    good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);

    outb(0x0CF9, 0x02);
    outb(0x0CF9, 0x06);

    for (;;)
        __asm__ __volatile__("hlt");
}

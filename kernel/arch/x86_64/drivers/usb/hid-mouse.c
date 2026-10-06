#include <stdint.h>
#include <lebirun/drivers/usb/hid.h>
#include <lebirun/mouse.h>
#include <lebirun/pit.h>
#include <lebirun/timekeeping.h>

extern volatile uint64_t tick_count;
extern uint64_t pit_freq;

static uint64_t hid_mouse_now_ns(void) {
    if (tsc_available()) return tsc_get_ns();
    return tick_count * (1000000000ULL / (pit_freq ? pit_freq : 1));
}

static uint64_t hid_mouse_last_ns;

int usb_hid_mouse_recent(void) {
    uint64_t now;

    if (!hid_mouse_last_ns) return 0;
    now = hid_mouse_now_ns();
    if (now < hid_mouse_last_ns) return 1;
    return now - hid_mouse_last_ns < 250000000ULL;
}

void usb_hid_mouse_report(uint8_t *r, uint64_t mps) {
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
    int8_t z;

    hid_mouse_last_ns = hid_mouse_now_ns();
    buttons = (uint8_t)(r[0] & 0x07);
    dx = (int8_t)r[1];
    dy = (int8_t)(0 - (int)(int8_t)r[2]);
    if (mouse_get_packet_size() == 4) {
        z = mps >= 4 ? (int8_t)r[3] : 0;
        mouse_inject_packet(buttons, dx, dy, z, 1);
    } else {
        mouse_inject_packet(buttons, dx, dy, 0, 0);
    }
}

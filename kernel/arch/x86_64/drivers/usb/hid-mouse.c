#include <stdint.h>
#include <lebirun/drivers/usb/hid.h>
#include <lebirun/mouse.h>
#include <lebirun/evdev.h>
#include <lebirun/framebuffer.h>
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

static uint32_t hid_abs_field(uint8_t *r, uint16_t off, uint8_t len) {
    uint64_t v;
    uint8_t i;

    v = 0;
    for (i = 0; i < len; i++) {
        if ((r[(off + i) / 8] >> ((off + i) % 8)) & 1)
            v |= (1ULL << i);
    }
    return (uint32_t)v;
}

static void hid_mouse_inject_delta(int32_t dx, int32_t dy) {
    int32_t cx;
    int32_t cy;

    while (dx != 0 || dy != 0) {
        cx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
        cy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
        dx -= cx;
        dy -= cy;
        mouse_inject_packet(0, (int8_t)cx, (int8_t)-cy, 0,
                            mouse_get_packet_size() == 4);
    }
}

void usb_hid_mouse_abs_report(uint8_t *r, uint16_t x_off, uint8_t x_len,
                               uint16_t y_off, uint8_t y_len, uint32_t max) {
    static uint8_t prev_buttons;
    static int32_t prev_px;
    static int32_t prev_py;
    static uint64_t prev_w;
    static uint64_t prev_h;
    static int have_prev;
    struct evdev_device *dev;
    framebuffer_t *fb;
    uint32_t x;
    uint32_t y;
    uint8_t buttons;
    uint8_t changed;
    int32_t px;
    int32_t py;
    int32_t dx;
    int32_t dy;

    if (!r || !x_len || !y_len || x_len > 32 || y_len > 32 || !max) return;
    hid_mouse_last_ns = hid_mouse_now_ns();
    dev = evdev_get_mouse();
    if (!dev) return;
    evdev_mouse_set_abs((int32_t)max);
    x = hid_abs_field(r, x_off, x_len);
    y = hid_abs_field(r, y_off, y_len);
    if (x > max) x = max;
    if (y > max) y = max;
    buttons = (uint8_t)(r[0] & 0x07);
    changed = (uint8_t)(buttons ^ prev_buttons);
    if (changed & 0x01)
        evdev_push_event(dev, EV_KEY, BTN_LEFT, (buttons & 0x01) ? 1 : 0);
    if (changed & 0x02)
        evdev_push_event(dev, EV_KEY, BTN_RIGHT, (buttons & 0x02) ? 1 : 0);
    if (changed & 0x04)
        evdev_push_event(dev, EV_KEY, BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);
    prev_buttons = buttons;
    evdev_push_event(dev, EV_ABS, ABS_X, (int32_t)x);
    evdev_push_event(dev, EV_ABS, ABS_Y, (int32_t)y);
    evdev_push_sync(dev);
    fb = fb_get();
    if (!fb || fb->width < 2 || fb->height < 2) return;
    px = (int32_t)((uint64_t)x * (fb->width - 1) / max);
    py = (int32_t)((uint64_t)y * (fb->height - 1) / max);
    if (!have_prev || fb->width != prev_w || fb->height != prev_h) {
        hid_mouse_inject_delta(-8191, -8191);
        prev_px = 0;
        prev_py = 0;
        prev_w = fb->width;
        prev_h = fb->height;
        have_prev = 1;
    }
    if ((px == 0 || px == (int32_t)fb->width - 1) && prev_px != px) {
        hid_mouse_inject_delta(px == 0 ? -8191 : 8191, 0);
        prev_px = px;
    }
    if ((py == 0 || py == (int32_t)fb->height - 1) && prev_py != py) {
        hid_mouse_inject_delta(0, py == 0 ? -8191 : 8191);
        prev_py = py;
    }
    dx = px - prev_px;
    dy = py - prev_py;
    prev_px = px;
    prev_py = py;
    hid_mouse_inject_delta(dx, dy);
}

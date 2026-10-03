#include <stdint.h>
#include <lebirun/drivers/usb/hid.h>
#include <lebirun/keyboard.h>
#include <lebirun/pit.h>

extern volatile uint64_t tick_count;
extern uint64_t pit_freq;

static const uint16_t hid_to_ps2[98] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23,
    0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
    0x15, 0x2C, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F,
    0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x00, 0x27,
    0x28, 0x29, 0x33, 0x34, 0x35, 0x3A, 0x3B, 0x3C,
    0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44,
    0x57, 0x58, 0x137, 0x46, 0x00, 0x152, 0x147, 0x149,
    0x153, 0x14F, 0x151, 0x14D, 0x14B, 0x150, 0x148, 0x45,
    0x135, 0x37, 0x4A, 0x4E, 0x11C, 0x4F, 0x50, 0x51,
    0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,
    0x56, 0x15D,
};

static void hid_feed(uint8_t code, uint8_t ext, uint8_t release) {
    if (ext) keyboard_feed_ps2(0xE0);
    keyboard_feed_ps2(release ? (uint8_t)(code | 0x80) : code);
}

static void hid_press(usb_hid_kbd_state_t *st, uint8_t usage) {
    uint16_t ps2;

    if (usage < 0x04 || usage > 0x65) return;
    ps2 = hid_to_ps2[usage - 0x04];
    if (!ps2) return;
    hid_feed((uint8_t)ps2, (uint8_t)(ps2 >> 8), 0);
    st->rpt = usage;
    st->rpt_due = tick_count + pit_freq / 2;
}

void usb_hid_kbd_report(usb_hid_kbd_state_t *st, uint8_t *r) {
    static const uint8_t mod_code[8] = {
        0x1D, 0x2A, 0x38, 0x5B, 0x1D, 0x36, 0x38, 0x5C
    };
    static const uint8_t mod_ext[8] = {
        0, 0, 0, 1, 1, 0, 1, 1
    };
    uint8_t changed;
    uint64_t i;
    uint64_t j;
    uint8_t found;

    if (r[2] == 0x01 && r[3] == 0x01 && r[4] == 0x01 &&
        r[5] == 0x01 && r[6] == 0x01 && r[7] == 0x01) return;
    changed = (uint8_t)(r[0] ^ st->mods);
    for (i = 0; i < 8; i++) {
        if (changed & (1u << i))
            hid_feed(mod_code[i], mod_ext[i], !(r[0] & (1u << i)));
    }
    for (i = 0; i < 6; i++) {
        if (!st->keys[i]) continue;
        found = 0;
        for (j = 2; j < 8; j++) {
            if (r[j] == st->keys[i]) {
                found = 1;
                break;
            }
        }
        if (found) continue;
        if (st->keys[i] == st->rpt) st->rpt = 0;
        if (st->keys[i] >= 0x04 && st->keys[i] <= 0x65) {
            uint16_t ps2 = hid_to_ps2[st->keys[i] - 0x04];
            if (ps2) hid_feed((uint8_t)ps2, (uint8_t)(ps2 >> 8), 1);
        }
    }
    for (i = 2; i < 8; i++) {
        if (!r[i]) continue;
        found = 0;
        for (j = 0; j < 6; j++) {
            if (st->keys[j] == r[i]) {
                found = 1;
                break;
            }
        }
        if (!found) hid_press(st, r[i]);
    }
    st->mods = r[0];
    for (i = 0; i < 6; i++)
        st->keys[i] = r[2 + i];
}

void usb_hid_kbd_repeat_one(usb_hid_kbd_state_t *st) {
    uint16_t ps2;

    if (!st->rpt) return;
    if (tick_count < st->rpt_due) return;
    if (st->rpt < 0x04 || st->rpt > 0x65) {
        st->rpt = 0;
        return;
    }
    ps2 = hid_to_ps2[st->rpt - 0x04];
    if (!ps2) {
        st->rpt = 0;
        return;
    }
    hid_feed((uint8_t)ps2, (uint8_t)(ps2 >> 8), 0);
    st->rpt_due = tick_count + pit_freq / 30;
}

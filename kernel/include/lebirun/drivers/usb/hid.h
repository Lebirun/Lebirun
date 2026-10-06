#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

typedef struct {
    uint8_t mods;
    uint8_t keys[6];
    uint8_t rpt;
    uint64_t rpt_due;
} usb_hid_kbd_state_t;

void usb_hid_kbd_report(usb_hid_kbd_state_t *st, uint8_t *report);
void usb_hid_kbd_repeat_one(usb_hid_kbd_state_t *st);
int usb_hid_kbd_recent(void);
void usb_hid_mouse_report(uint8_t *report, uint64_t mps);
int usb_hid_mouse_recent(void);

#endif

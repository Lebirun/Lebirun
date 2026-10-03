#include <stdint.h>
#include <lebirun/drivers/usb/hid.h>
#include <lebirun/mouse.h>

void usb_hid_mouse_report(uint8_t *r, uint64_t mps) {
    mouse_inject_packet((uint8_t)(r[0] & 0x07), (int8_t)r[1], (int8_t)r[2],
                        mps >= 4 ? (int8_t)r[3] : 0, mps >= 4);
}

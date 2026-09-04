#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <lebirun/registers.h>

#define MOUSE_LEFT_BUTTON   0x01
#define MOUSE_RIGHT_BUTTON  0x02
#define MOUSE_MIDDLE_BUTTON 0x04

struct mouse_packet {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
};

void mouse_init(void);
void mouse_handler(registers_t *regs);
int mouse_has_data(void);
int mouse_read(uint8_t *buf, uint32_t count);
uint32_t mouse_get_packet_size(void);
void mouse_debug_snapshot(void);

#endif

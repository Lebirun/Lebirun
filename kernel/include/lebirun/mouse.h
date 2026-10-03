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
void mouse_inject_packet(uint8_t buttons, int8_t dx, int8_t dy, int8_t z,
                         int has_z);
int mouse_has_data(void);
int mouse_read(uint8_t *buf, uint32_t count);
uint32_t mouse_get_packet_size(void);

#endif

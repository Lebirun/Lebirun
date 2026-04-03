#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <kernel/registers.h>
#include <kernel/task.h>

#define MOUSE_LEFT_BUTTON   0x01
#define MOUSE_RIGHT_BUTTON  0x02
#define MOUSE_MIDDLE_BUTTON 0x04

#define MOUSE_BUF_SIZE 4096

struct mouse_packet {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
};

void mouse_init(void);
void mouse_handler(registers_t *regs);
int mouse_has_data(void);
int mouse_read(uint8_t *buf, uint32_t count);
wait_queue_t *mouse_get_waitq(void);

#endif

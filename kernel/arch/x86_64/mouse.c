#include <kernel/io.h>
#include <kernel/mouse.h>
#include <kernel/idt.h>
#include <kernel/debug.h>
#include <kernel/task.h>
#include <string.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define MOUSE_IRQ        12

static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[3];
static uint8_t ring_buffer[MOUSE_BUF_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static wait_queue_t mouse_waitq;

static void ps2_wait_input(void) {
    int timeout = 100000;
    while (timeout--) {
        if ((inb(PS2_STATUS_PORT) & 0x02) == 0)
            return;
    }
}

static void ps2_wait_output(void) {
    int timeout = 100000;
    while (timeout--) {
        if (inb(PS2_STATUS_PORT) & 0x01)
            return;
    }
}

static void ps2_mouse_write(uint8_t data) {
    ps2_wait_input();
    outb(PS2_CMD_PORT, 0xD4);
    ps2_wait_input();
    outb(PS2_DATA_PORT, data);
}

static uint8_t ps2_mouse_read(void) {
    ps2_wait_output();
    return inb(PS2_DATA_PORT);
}

static void ring_put(uint8_t byte) {
    uint32_t next;
    next = (ring_head + 1) % MOUSE_BUF_SIZE;
    if (next == ring_tail)
        return;
    ring_buffer[ring_head] = byte;
    ring_head = next;
}

void mouse_handler(registers_t *regs) {
    uint8_t status;
    uint8_t data;
    uint8_t buttons;
    int8_t dx;
    int8_t dy;

    (void)regs;

    status = inb(PS2_STATUS_PORT);
    if (!(status & 0x20))
        return;

    data = inb(PS2_DATA_PORT);

    switch (mouse_cycle) {
    case 0:
        if (data & 0x08) {
            mouse_bytes[0] = (int8_t)data;
            mouse_cycle = 1;
        }
        break;
    case 1:
        mouse_bytes[1] = (int8_t)data;
        mouse_cycle = 2;
        break;
    case 2:
        mouse_bytes[2] = (int8_t)data;
        mouse_cycle = 0;

        buttons = (uint8_t)(mouse_bytes[0] & 0x07);
        dx = mouse_bytes[1];
        dy = mouse_bytes[2];

        if (mouse_bytes[0] & 0x10)
            dx = (int8_t)(dx | 0xFFFFFF00);
        if (mouse_bytes[0] & 0x20)
            dy = (int8_t)(dy | 0xFFFFFF00);

        ring_put(buttons);
        ring_put((uint8_t)dx);
        ring_put((uint8_t)dy);

        waitq_wake_all(&mouse_waitq);
        break;
    }
}

int mouse_has_data(void) {
    return ring_head != ring_tail;
}

int mouse_read(uint8_t *buf, uint32_t count) {
    uint32_t i;
    i = 0;
    while (i < count && ring_head != ring_tail) {
        buf[i] = ring_buffer[ring_tail];
        ring_tail = (ring_tail + 1) % MOUSE_BUF_SIZE;
        i++;
    }
    return (int)i;
}

wait_queue_t *mouse_get_waitq(void) {
    return &mouse_waitq;
}

void mouse_init(void) {
    uint8_t ack;
    uint8_t config;

    ring_head = 0;
    ring_tail = 0;
    mouse_cycle = 0;
    memset(ring_buffer, 0, sizeof(ring_buffer));
    waitq_init(&mouse_waitq);

    ps2_wait_input();
    outb(PS2_CMD_PORT, 0xA8);

    ps2_wait_input();
    outb(PS2_CMD_PORT, 0x20);
    ps2_wait_output();
    config = inb(PS2_DATA_PORT);
    config |= 0x02;
    config &= ~0x20;
    ps2_wait_input();
    outb(PS2_CMD_PORT, 0x60);
    ps2_wait_input();
    outb(PS2_DATA_PORT, config);

    ps2_mouse_write(0xFF);
    ack = ps2_mouse_read();
    (void)ack;
    ps2_mouse_read();
    ps2_mouse_read();

    ps2_mouse_write(0xF6);
    ps2_mouse_read();

    ps2_mouse_write(0xF4);
    ps2_mouse_read();

    irq_register_handler(MOUSE_IRQ, mouse_handler);
    irq_unmask(2);
    irq_unmask(MOUSE_IRQ);
}

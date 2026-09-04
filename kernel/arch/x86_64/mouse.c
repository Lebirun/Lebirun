#include <lebirun/io.h>
#include <lebirun/mouse.h>
#include <lebirun/idt.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/spinlock.h>
#include <lebirun/vring.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define MOUSE_IRQ        12
#define MOUSE_RING_INITIAL 16

static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[4];
static uint32_t mouse_packet_size = 3;
static uint8_t *ring_buffer;
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static uint32_t ring_capacity;
static spinlock_t mouse_lock = {0};
static uint64_t mouse_debug_packets;
static uint64_t mouse_debug_queued;

static uint64_t mouse_irqsave(void) {
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void mouse_irqrestore(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
}

static uint32_t mouse_ring_used(void) {
    if (ring_head >= ring_tail) return ring_head - ring_tail;
    return ring_capacity - ring_tail + ring_head;
}

static int mouse_reserve_ring(uint32_t additional) {
    uint8_t *new_ring;
    uint32_t used;
    uint32_t new_capacity;
    uint32_t i;

    used = ring_buffer ? mouse_ring_used() : 0;
    new_capacity = ring_capacity;
    if (new_capacity == 0) new_capacity = MOUSE_RING_INITIAL;
    while ((uint64_t)used + additional + 1 > new_capacity) {
        if (new_capacity > UINT32_MAX / 2) return 0;
        new_capacity *= 2;
    }
    if (ring_buffer && new_capacity == ring_capacity) return 1;
    new_ring = (uint8_t *)kmalloc(new_capacity);
    if (!new_ring) return 0;
    for (i = 0; i < used; i++) {
        new_ring[i] = ring_buffer[(ring_tail + i) % ring_capacity];
    }
    kfree(ring_buffer);
    ring_buffer = new_ring;
    ring_capacity = new_capacity;
    ring_tail = 0;
    ring_head = used;
    return 1;
}

static void KERNEL_INIT ps2_wait_input(void) {
    int timeout = 100000;
    while (timeout--) {
        if ((inb(PS2_STATUS_PORT) & 0x02) == 0)
            return;
    }
}

static void KERNEL_INIT ps2_wait_output(void) {
    int timeout = 100000;
    while (timeout--) {
        if (inb(PS2_STATUS_PORT) & 0x01)
            return;
    }
}

static void KERNEL_INIT ps2_mouse_write(uint8_t data) {
    ps2_wait_input();
    outb(PS2_CMD_PORT, 0xD4);
    ps2_wait_input();
    outb(PS2_DATA_PORT, data);
}

static uint8_t KERNEL_INIT ps2_mouse_read(void) {
    ps2_wait_output();
    return inb(PS2_DATA_PORT);
}

static void ring_put(uint8_t byte) {
    if (!ring_buffer)
        return;
    ring_buffer[ring_head] = byte;
    ring_head = (ring_head + 1) % ring_capacity;
}

void mouse_handler(registers_t *regs) {
    uint8_t status;
    uint8_t data;
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
    int complete;
    uint64_t flags;

    (void)regs;

    status = inb(PS2_STATUS_PORT);
    if (!(status & 0x20))
        return;

    data = inb(PS2_DATA_PORT);

    complete = 0;
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
        if (mouse_packet_size == 4)
            mouse_cycle = 3;
        else {
            mouse_cycle = 0;
            complete = 1;
        }
        break;
    case 3:
        mouse_bytes[3] = (int8_t)data;
        mouse_cycle = 0;
        complete = 1;
        break;
    }

    if (!complete) return;
    buttons = (uint8_t)(mouse_bytes[0] & 0x07);
    dx = mouse_bytes[1];
    dy = mouse_bytes[2];

    flags = mouse_irqsave();
    spin_lock(&mouse_lock);
    mouse_debug_packets++;
    if (ring_buffer && mouse_reserve_ring(mouse_packet_size)) {
        ring_put(buttons);
        ring_put((uint8_t)dx);
        ring_put((uint8_t)dy);
        if (mouse_packet_size == 4)
            ring_put((uint8_t)mouse_bytes[3]);
        mouse_debug_queued++;
    }
    spin_unlock(&mouse_lock);
    mouse_irqrestore(flags);

    descriptor_ready_notify_irq();
}

void mouse_debug_snapshot(void) {
    uint64_t flags;
    uint64_t packets;
    uint64_t queued;
    uint32_t used;
    uint32_t capacity;
    uint32_t packet_size;

    flags = mouse_irqsave();
    spin_lock(&mouse_lock);
    packets = mouse_debug_packets;
    queued = mouse_debug_queued;
    used = ring_buffer ? mouse_ring_used() : 0;
    capacity = ring_capacity;
    packet_size = mouse_packet_size;
    spin_unlock(&mouse_lock);
    mouse_irqrestore(flags);
    vt_debug_printf("[VTDBG MOUSE] packets=%llu queued=%llu bytes=%u capacity=%u packet=%u\n",
                    packets, queued, used, capacity, packet_size);
}

int mouse_has_data(void) {
    uint64_t flags;
    int result;

    flags = mouse_irqsave();
    spin_lock(&mouse_lock);
    mouse_reserve_ring(0);
    result = ring_head != ring_tail;
    spin_unlock(&mouse_lock);
    mouse_irqrestore(flags);
    return result;
}

int mouse_read(uint8_t *buf, uint32_t count) {
    uint32_t i;
    uint64_t flags;

    i = 0;
    flags = mouse_irqsave();
    spin_lock(&mouse_lock);
    if (!mouse_reserve_ring(0)) {
        spin_unlock(&mouse_lock);
        mouse_irqrestore(flags);
        return 0;
    }
    while (i < count && ring_head != ring_tail) {
        buf[i] = ring_buffer[ring_tail];
        ring_tail = (ring_tail + 1) % ring_capacity;
        i++;
    }
    spin_unlock(&mouse_lock);
    mouse_irqrestore(flags);
    return (int)i;
}

uint32_t mouse_get_packet_size(void) {
    return mouse_packet_size;
}

void KERNEL_INIT mouse_init(void) {
    uint8_t config;
    uint8_t device_id;

    ring_head = 0;
    ring_tail = 0;
    ring_buffer = NULL;
    ring_capacity = 0;
    mouse_cycle = 0;
    mouse_packet_size = 3;

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
    ps2_mouse_read();
    ps2_mouse_read();
    ps2_mouse_read();

    ps2_mouse_write(0xF6);
    ps2_mouse_read();

    ps2_mouse_write(0xF3);
    ps2_mouse_read();
    ps2_mouse_write(200);
    ps2_mouse_read();
    ps2_mouse_write(0xF3);
    ps2_mouse_read();
    ps2_mouse_write(100);
    ps2_mouse_read();
    ps2_mouse_write(0xF3);
    ps2_mouse_read();
    ps2_mouse_write(80);
    ps2_mouse_read();
    ps2_mouse_write(0xF2);
    ps2_mouse_read();
    device_id = ps2_mouse_read();
    if (device_id == 3 || device_id == 4)
        mouse_packet_size = 4;

    ps2_mouse_write(0xF4);
    ps2_mouse_read();

    irq_register_handler(MOUSE_IRQ, mouse_handler);
    irq_unmask(2);
    irq_unmask(MOUSE_IRQ);
}

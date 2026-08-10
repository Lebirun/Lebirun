#include <lebirun/io.h>
#include <lebirun/mouse.h>
#include <lebirun/idt.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/spinlock.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define MOUSE_IRQ        12
#define MOUSE_RING_INITIAL 16

static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[3];
static uint8_t *ring_buffer;
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static uint32_t ring_capacity;
static wait_queue_t mouse_waitq;
static spinlock_t mouse_lock = {0};

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
    uint64_t flags;

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

        flags = mouse_irqsave();
        spin_lock(&mouse_lock);
        if (ring_buffer && mouse_reserve_ring(3)) {
            ring_put(buttons);
            ring_put((uint8_t)dx);
            ring_put((uint8_t)dy);
        }
        spin_unlock(&mouse_lock);
        mouse_irqrestore(flags);

        waitq_wake_all(&mouse_waitq);
        descriptor_ready_notify();
        break;
    }
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

wait_queue_t *mouse_get_waitq(void) {
    return &mouse_waitq;
}

void KERNEL_INIT mouse_init(void) {
    uint8_t config;

    ring_head = 0;
    ring_tail = 0;
    ring_buffer = NULL;
    ring_capacity = 0;
    mouse_cycle = 0;
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
    ps2_mouse_read();
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

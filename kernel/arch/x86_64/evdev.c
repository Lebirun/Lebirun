#include <lebirun/evdev.h>
#include <lebirun/keyboard.h>
#include <lebirun/mouse.h>
#include <lebirun/pit.h>
#include <lebirun/debug.h>
#include <lebirun/mem_map.h>
#include <string.h>

static struct evdev_device evdev_kbd;
static struct evdev_device evdev_mouse;

static vfs_node_t evdev_input_dir;
static vfs_node_t evdev_event0;
static vfs_node_t evdev_event1;

static dirent_t evdev_dirent;

static uint8_t prev_mouse_buttons = 0;

extern volatile uint64_t tick_count;
extern uint64_t pit_freq;

static const uint16_t scancode_to_evdev[128] = {
    KEY_RESERVED,  KEY_ESC,       KEY_1,         KEY_2,
    KEY_3,         KEY_4,         KEY_5,         KEY_6,
    KEY_7,         KEY_8,         KEY_9,         KEY_0,
    KEY_MINUS,     KEY_EQUAL,     KEY_BACKSPACE, KEY_TAB,
    KEY_Q,         KEY_W,         KEY_E,         KEY_R,
    KEY_T,         KEY_Y,         KEY_U,         KEY_I,
    KEY_O,         KEY_P,         KEY_LEFTBRACE, KEY_RIGHTBRACE,
    KEY_ENTER,     KEY_LEFTCTRL,  KEY_A,         KEY_S,
    KEY_D,         KEY_F,         KEY_G,         KEY_H,
    KEY_J,         KEY_K,         KEY_L,         KEY_SEMICOLON,
    KEY_APOSTROPHE,KEY_GRAVE,     KEY_LEFTSHIFT, KEY_BACKSLASH,
    KEY_Z,         KEY_X,         KEY_C,         KEY_V,
    KEY_B,         KEY_N,         KEY_M,         KEY_COMMA,
    KEY_DOT,       KEY_SLASH,     KEY_RIGHTSHIFT,KEY_KPASTERISK,
    KEY_LEFTALT,   KEY_SPACE,     KEY_CAPSLOCK,  KEY_F1,
    KEY_F2,        KEY_F3,        KEY_F4,        KEY_F5,
    KEY_F6,        KEY_F7,        KEY_F8,        KEY_F9,
    KEY_F10,       KEY_NUMLOCK,   KEY_SCROLLLOCK,KEY_KP7,
    KEY_KP8,       KEY_KP9,       KEY_KPMINUS,   KEY_KP4,
    KEY_KP5,       KEY_KP6,       KEY_KPPLUS,    KEY_KP1,
    KEY_KP2,       KEY_KP3,       KEY_KP0,       KEY_KPDOT,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_F11,
    KEY_F12,       KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,
    KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED,  KEY_RESERVED
};

static void evdev_ensure_ring(struct evdev_device *dev) {
    struct input_event *ring;

    if (!dev) return;
    if (dev->ring && dev->ring_capacity > 0) return;
    ring = (struct input_event *)kmalloc(EVDEV_BUF_INIT_EVENTS * sizeof(struct input_event));
    if (!ring) return;
    memset(ring, 0, EVDEV_BUF_INIT_EVENTS * sizeof(struct input_event));
    dev->ring = ring;
    dev->ring_capacity = EVDEV_BUF_INIT_EVENTS;
    dev->head = 0;
    dev->tail = 0;
}

static void evdev_ring_put(struct evdev_device *dev, const struct input_event *ev) {
    uint32_t next;
    uint32_t old_capacity;
    uint32_t new_capacity;
    uint32_t count;
    uint32_t i;
    struct input_event *new_ring;

    if (!dev->ring || dev->ring_capacity == 0)
        return;
    next = (dev->head + 1) % dev->ring_capacity;
    if (next == dev->tail) {
        old_capacity = dev->ring_capacity;
        if (old_capacity >= EVDEV_BUF_EVENTS)
            return;
        new_capacity = old_capacity * 2;
        if (new_capacity > EVDEV_BUF_EVENTS)
            new_capacity = EVDEV_BUF_EVENTS;
        new_ring = (struct input_event *)kmalloc(new_capacity * sizeof(struct input_event));
        if (!new_ring)
            return;
        count = 0;
        while (dev->tail != dev->head && count < old_capacity - 1) {
            new_ring[count] = dev->ring[dev->tail];
            dev->tail = (dev->tail + 1) % old_capacity;
            count++;
        }
        for (i = count; i < new_capacity; i++) {
            memset(&new_ring[i], 0, sizeof(struct input_event));
        }
        kfree(dev->ring);
        dev->ring = new_ring;
        dev->ring_capacity = new_capacity;
        dev->tail = 0;
        dev->head = count;
        next = (dev->head + 1) % dev->ring_capacity;
        if (next == dev->tail)
            return;
    }
    dev->ring[dev->head] = *ev;
    dev->head = next;
}

static int evdev_ring_get(struct evdev_device *dev, struct input_event *ev) {
    if (!dev->ring || dev->ring_capacity == 0)
        return 0;
    if (dev->head == dev->tail)
        return 0;
    *ev = dev->ring[dev->tail];
    dev->tail = (dev->tail + 1) % dev->ring_capacity;
    return 1;
}

int evdev_has_data(struct evdev_device *dev) {
    return dev->head != dev->tail;
}

void evdev_push_event(struct evdev_device *dev, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    uint64_t ticks;
    ticks = tick_count;
    ev.tv_sec = ticks / pit_freq;
    ev.tv_usec = ((ticks % pit_freq) * 1000000) / pit_freq;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    evdev_ring_put(dev, &ev);
}

void evdev_push_sync(struct evdev_device *dev) {
    evdev_push_event(dev, EV_SYN, SYN_REPORT, 0);
    waitq_wake_all(&dev->waitq);
}

static void evdev_kbd_observer(struct keyboard_event event) {
    uint16_t keycode;
    if (event.scancode >= 128)
        return;
    keycode = scancode_to_evdev[event.scancode];
    if (keycode == KEY_RESERVED)
        return;
    evdev_push_event(&evdev_kbd, EV_KEY, keycode, event.is_release ? 0 : 1);
    evdev_push_sync(&evdev_kbd);
}

static void evdev_process_mouse(void) {
    uint8_t pkt[3];
    int nread;
    uint8_t buttons;
    int8_t dx;
    int8_t dy;

    while (mouse_has_data()) {
        nread = mouse_read(pkt, 3);
        if (nread < 3)
            break;
        buttons = pkt[0];
        dx = (int8_t)pkt[1];
        dy = (int8_t)pkt[2];

        if (dx != 0)
            evdev_push_event(&evdev_mouse, EV_REL, REL_X, (int32_t)dx);
        if (dy != 0)
            evdev_push_event(&evdev_mouse, EV_REL, REL_Y, (int32_t)(-dy));

        if ((buttons & 0x01) != (prev_mouse_buttons & 0x01))
            evdev_push_event(&evdev_mouse, EV_KEY, BTN_LEFT, (buttons & 0x01) ? 1 : 0);
        if ((buttons & 0x02) != (prev_mouse_buttons & 0x02))
            evdev_push_event(&evdev_mouse, EV_KEY, BTN_RIGHT, (buttons & 0x02) ? 1 : 0);
        if ((buttons & 0x04) != (prev_mouse_buttons & 0x04))
            evdev_push_event(&evdev_mouse, EV_KEY, BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);

        prev_mouse_buttons = buttons;
        evdev_push_sync(&evdev_mouse);
    }
}

uint64_t evdev_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct evdev_device *dev;
    struct input_event ev;
    uint64_t written;
    uint64_t ev_size;
    int guard;

    (void)offset;
    dev = (struct evdev_device *)node->private_data;
    if (!dev)
        return 0;

    evdev_ensure_ring(dev);
    if (dev == &evdev_mouse)
        evdev_process_mouse();

    guard = 0;
    while (!evdev_has_data(dev) && guard < 10000) {
        if (dev == &evdev_mouse)
            evdev_process_mouse();
        if (evdev_has_data(dev))
            break;
        waitq_add(&dev->waitq, current_task);
        block_current();
        if (dev == &evdev_mouse)
            evdev_process_mouse();
        guard++;
    }

    ev_size = sizeof(struct input_event);
    written = 0;
    while (written + ev_size <= size) {
        if (!evdev_ring_get(dev, &ev))
            break;
        memcpy(buffer + written, &ev, ev_size);
        written += ev_size;
    }
    return written;
}

uint64_t evdev_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    (void)buffer;
    (void)size;
    return 0;
}

static void set_bit(uint8_t *bits, int bit) {
    bits[bit / 8] |= (1 << (bit % 8));
}

int evdev_ioctl(vfs_node_t *node, unsigned long request, void *arg) {
    struct evdev_device *dev;
    struct input_id *id_out;
    int *ver;
    unsigned long base;
    unsigned long ev_type;
    unsigned long len;
    unsigned long copylen;

    dev = (struct evdev_device *)node->private_data;
    if (!dev)
        return -22;

    if (request == EVIOCGVERSION) {
        ver = (int *)arg;
        *ver = 0x010001;
        return 0;
    }

    if (request == EVIOCGID) {
        id_out = (struct input_id *)arg;
        *id_out = dev->id;
        return 0;
    }

    base = request & 0xFFFF00FF;

    if (base == EVIOCGNAME_BASE) {
        len = (request >> 16) & 0x1FFF;
        copylen = strlen(dev->name);
        if (copylen >= len)
            copylen = len - 1;
        memcpy(arg, dev->name, copylen);
        ((char *)arg)[copylen] = '\0';
        return (int)copylen;
    }

    if (base == EVIOCGPROP_BASE) {
        len = (request >> 16) & 0x1FFF;
        copylen = sizeof(dev->prop_bits);
        if (copylen > len)
            copylen = len;
        memcpy(arg, dev->prop_bits, copylen);
        return 0;
    }

    if ((request & 0xFF) >= 0x20 && (request & 0xFF) <= 0x3F) {
        ev_type = (request >> 8) & 0xFF;
        len = (request >> 16) & 0x1FFF;
        switch (ev_type) {
        case 0:
            copylen = sizeof(dev->ev_bits);
            if (copylen > len) copylen = len;
            memcpy(arg, dev->ev_bits, copylen);
            return 0;
        case EV_KEY:
            copylen = sizeof(dev->key_bits);
            if (copylen > len) copylen = len;
            memcpy(arg, dev->key_bits, copylen);
            return 0;
        case EV_REL:
            copylen = sizeof(dev->rel_bits);
            if (copylen > len) copylen = len;
            memcpy(arg, dev->rel_bits, copylen);
            return 0;
        case EV_ABS:
            copylen = sizeof(dev->abs_bits);
            if (copylen > len) copylen = len;
            memcpy(arg, dev->abs_bits, copylen);
            return 0;
        default:
            if (len > 0) memset(arg, 0, len);
            return 0;
        }
    }

    if ((request & 0xFF) >= 0x40 && (request & 0xFF) <= 0x7F) {
        memset(arg, 0, sizeof(struct input_absinfo));
        return 0;
    }

    return -22;
}

static dirent_t *evdev_input_readdir(vfs_node_t *node, uint64_t index) {
    (void)node;
    if (index == 0) {
        memset(&evdev_dirent, 0, sizeof(evdev_dirent));
        strcpy(evdev_dirent.name, "event0");
        evdev_dirent.inode = 300;
        evdev_dirent.type = VFS_CHARDEVICE;
        return &evdev_dirent;
    }
    if (index == 1) {
        memset(&evdev_dirent, 0, sizeof(evdev_dirent));
        strcpy(evdev_dirent.name, "event1");
        evdev_dirent.inode = 301;
        evdev_dirent.type = VFS_CHARDEVICE;
        return &evdev_dirent;
    }
    return NULL;
}

static vfs_node_t *evdev_input_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "event0") == 0)
        return &evdev_event0;
    if (strcmp(name, "event1") == 0)
        return &evdev_event1;
    return NULL;
}

static void devfs_open_stub(vfs_node_t *node, uint64_t flags) {
    struct evdev_device *dev;

    (void)flags;
    if (!node) return;
    dev = (struct evdev_device *)node->private_data;
    evdev_ensure_ring(dev);
}
static void devfs_close_stub(vfs_node_t *node) { (void)node; }

vfs_node_t *evdev_get_input_dir(void) {
    return &evdev_input_dir;
}

vfs_node_t *evdev_get_event_node(int index) {
    if (index == 0) return &evdev_event0;
    if (index == 1) return &evdev_event1;
    return NULL;
}

struct evdev_device *evdev_get_kbd(void) {
    return &evdev_kbd;
}

struct evdev_device *evdev_get_mouse(void) {
    return &evdev_mouse;
}

void evdev_init(void) {
    int i;

    memset(&evdev_kbd, 0, sizeof(evdev_kbd));
    waitq_init(&evdev_kbd.waitq);
    strcpy(evdev_kbd.name, "Lebirun PS/2 Keyboard");
    evdev_kbd.id.bustype = BUS_I8042;
    evdev_kbd.id.vendor = 0x0001;
    evdev_kbd.id.product = 0x0001;
    evdev_kbd.id.version = 0x0001;
    set_bit(evdev_kbd.ev_bits, EV_SYN);
    set_bit(evdev_kbd.ev_bits, EV_KEY);
    for (i = 0; i < 128; i++) {
        if (scancode_to_evdev[i] != KEY_RESERVED)
            set_bit(evdev_kbd.key_bits, scancode_to_evdev[i]);
    }

    memset(&evdev_mouse, 0, sizeof(evdev_mouse));
    waitq_init(&evdev_mouse.waitq);
    strcpy(evdev_mouse.name, "Lebirun PS/2 Mouse");
    evdev_mouse.id.bustype = BUS_I8042;
    evdev_mouse.id.vendor = 0x0002;
    evdev_mouse.id.product = 0x0001;
    evdev_mouse.id.version = 0x0001;
    set_bit(evdev_mouse.ev_bits, EV_SYN);
    set_bit(evdev_mouse.ev_bits, EV_KEY);
    set_bit(evdev_mouse.ev_bits, EV_REL);
    set_bit(evdev_mouse.key_bits, BTN_LEFT);
    set_bit(evdev_mouse.key_bits, BTN_RIGHT);
    set_bit(evdev_mouse.key_bits, BTN_MIDDLE);
    set_bit(evdev_mouse.rel_bits, REL_X);
    set_bit(evdev_mouse.rel_bits, REL_Y);

    memset(&evdev_input_dir, 0, sizeof(vfs_node_t));
    strcpy(evdev_input_dir.name, "input");
    evdev_input_dir.flags = VFS_DIRECTORY;
    evdev_input_dir.mask = 0755;
    evdev_input_dir.readdir = evdev_input_readdir;
    evdev_input_dir.finddir = evdev_input_finddir;
    evdev_input_dir.ref_count = 1;

    memset(&evdev_event0, 0, sizeof(vfs_node_t));
    strcpy(evdev_event0.name, "event0");
    evdev_event0.flags = VFS_CHARDEVICE;
    evdev_event0.mask = 0660;
    evdev_event0.read = evdev_read;
    evdev_event0.write = evdev_write;
    evdev_event0.ioctl = evdev_ioctl;
    evdev_event0.open = devfs_open_stub;
    evdev_event0.close = devfs_close_stub;
    evdev_event0.parent = &evdev_input_dir;
    evdev_event0.ref_count = 1;
    evdev_event0.private_data = &evdev_kbd;

    memset(&evdev_event1, 0, sizeof(vfs_node_t));
    strcpy(evdev_event1.name, "event1");
    evdev_event1.flags = VFS_CHARDEVICE;
    evdev_event1.mask = 0660;
    evdev_event1.read = evdev_read;
    evdev_event1.write = evdev_write;
    evdev_event1.ioctl = evdev_ioctl;
    evdev_event1.open = devfs_open_stub;
    evdev_event1.close = devfs_close_stub;
    evdev_event1.parent = &evdev_input_dir;
    evdev_event1.ref_count = 1;
    evdev_event1.private_data = &evdev_mouse;

    keyboard_register_observer(evdev_kbd_observer);
}

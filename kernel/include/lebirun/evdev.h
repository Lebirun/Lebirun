#ifndef EVDEV_H
#define EVDEV_H

#include <stdint.h>
#include <lebirun/vfs.h>
#include <lebirun/task.h>

#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04

#define SYN_REPORT      0

#define REL_X           0x00
#define REL_Y           0x01
#define REL_WHEEL       0x08

#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112

#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_NUMLOCK     69
#define KEY_SCROLLLOCK  70
#define KEY_KP7         71
#define KEY_KP8         72
#define KEY_KP9         73
#define KEY_KPMINUS     74
#define KEY_KP4         75
#define KEY_KP5         76
#define KEY_KP6         77
#define KEY_KPPLUS      78
#define KEY_KP1         79
#define KEY_KP2         80
#define KEY_KP3         81
#define KEY_KP0         82
#define KEY_KPDOT       83
#define KEY_F11         87
#define KEY_F12         88
#define KEY_KPENTER     96
#define KEY_RIGHTCTRL   97
#define KEY_KPSLASH     98
#define KEY_RIGHTALT    100
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_INSERT      110
#define KEY_DELETE      111
#define KEY_MAX         0x2FF

#define BUS_I8042       0x11

#define EVIOCGVERSION   0x80044501
#define EVIOCGID        0x80084502
#define EVIOCGNAME(len) (0x80004506 | ((len) << 16))
#define EVIOCGBIT(ev,len) (0x80004520 | ((ev) << 8) | ((len) << 16))
#define EVIOCGABS(abs)  (0x80184540 | (abs))
#define EVIOCGPROP(len) (0x80004509 | ((len) << 16))

#define EVIOCGBIT_BASE  0x80004520
#define EVIOCGNAME_BASE 0x80004506
#define EVIOCGPROP_BASE 0x80004509
#define EVIOCGABS_BASE  0x80184540

struct input_event {
    uint64_t tv_sec;
    uint64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

#define EVDEV_BUF_EVENTS 256
#define EVDEV_BUF_INIT_EVENTS 8

struct evdev_device {
    struct input_event *ring;
    uint32_t ring_capacity;
    volatile uint32_t head;
    volatile uint32_t tail;
    wait_queue_t waitq;
    struct input_id id;
    char name[64];
    uint8_t ev_bits[4];
    uint8_t key_bits[(KEY_MAX + 7) / 8];
    uint8_t rel_bits[4];
    uint8_t abs_bits[4];
    uint8_t prop_bits[4];
};

void evdev_init(void);
void evdev_push_event(struct evdev_device *dev, uint16_t type, uint16_t code, int32_t value);
void evdev_push_sync(struct evdev_device *dev);
int evdev_has_data(struct evdev_device *dev);
int evdev_node_has_data(vfs_node_t *node);

struct evdev_device *evdev_get_kbd(void);
struct evdev_device *evdev_get_mouse(void);

vfs_node_t *evdev_get_input_dir(void);
vfs_node_t *evdev_get_event_node(int index);

uint64_t evdev_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
uint64_t evdev_read_nonblocking(vfs_node_t *node, uint64_t size, uint8_t *buffer);
uint64_t evdev_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
int evdev_ioctl(vfs_node_t *node, unsigned long request, void *arg);

#endif

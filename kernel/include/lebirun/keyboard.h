#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <lebirun/registers.h>
#include <stdint.h>

#define SCANCODE_F1  0x3B
#define SCANCODE_F2  0x3C
#define SCANCODE_F3  0x3D
#define SCANCODE_F4  0x3E
#define SCANCODE_F5  0x3F
#define SCANCODE_F6  0x40
#define SCANCODE_F7  0x41
#define SCANCODE_F8  0x42
#define SCANCODE_F9  0x43
#define SCANCODE_F10 0x44
#define SCANCODE_F11 0x57
#define SCANCODE_F12 0x58

struct keyboard_event {
    uint8_t scancode;
    uint8_t is_release;
    uint8_t ctrl_held;
    uint8_t alt_held;
    uint8_t shift_held;
};

typedef void (*keyboard_observer_t)(struct keyboard_event event);

void keyboard_handler(registers_t* regs); 
void keyboard_init(void);

int keyboard_has_data(void);
int keyboard_getchar_nb(void);

int keyboard_has_data_for(int console_id);
int keyboard_getchar_nb_for(int console_id);
void keyboard_flush_for(int console_id);

#include <lebirun/task.h>
wait_queue_t* keyboard_get_waitq(void);
wait_queue_t* keyboard_get_waitq_for(int console_id);

int getchar(void);

void keyboard_register_observer(keyboard_observer_t observer);
void keyboard_unregister_observer(void);
int keyboard_get_modifier_state(void);
void keyboard_process_sigint(void);

#endif

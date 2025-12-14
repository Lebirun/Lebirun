#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <kernel/registers.h>

void keyboard_handler(registers_t* regs); 
void keyboard_init(void);

int keyboard_has_data(void);
int keyboard_getchar_nb(void);

#include <kernel/task.h>
wait_queue_t* keyboard_get_waitq(void);

int getchar(void);

#endif
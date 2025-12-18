#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <kernel/registers.h>

void keyboard_handler(registers_t* regs); 
void keyboard_init(void);

int keyboard_has_data(void);
int keyboard_getchar_nb(void);

int keyboard_has_data_for(int console_id);
int keyboard_getchar_nb_for(int console_id);

#include <kernel/task.h>
wait_queue_t* keyboard_get_waitq(void);
wait_queue_t* keyboard_get_waitq_for(int console_id);

int getchar(void);

#endif
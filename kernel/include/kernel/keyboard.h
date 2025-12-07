#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <kernel/registers.h>

void keyboard_handler(registers_t* regs); 
void keyboard_init(void);
char getchar(void);

#endif
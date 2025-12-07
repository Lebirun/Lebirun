#ifndef ARCH_I386_PIT_H
#define ARCH_I386_PIT_H

#include <stdint.h>

void pit_init(uint32_t freq);
void delay(uint32_t ms);
void calibrate_pit(void);

#endif
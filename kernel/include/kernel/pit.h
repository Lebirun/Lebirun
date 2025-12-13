#ifndef ARCH_I386_PIT_H
#define ARCH_I386_PIT_H

#include <stdint.h>

void pit_init(uint32_t freq);
void calibrate_pit(void);
extern uint32_t pit_freq;

void delay(uint32_t ms);
void delay_inMs(uint32_t ms);
void delay_inSecs(uint32_t secs);
void delay_inMins(uint32_t mins);

#endif
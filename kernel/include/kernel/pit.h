#ifndef ARCH_I386_PIT_H
#define ARCH_I386_PIT_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*pit_callback_t)(uint32_t ticks);

extern uint32_t pit_freq;

void pit_init(uint32_t freq);
void pit_set_frequency(uint32_t freq);
void calibrate_pit(void);

void delay(uint32_t ms);
void delay_us(uint32_t us);
void delay_inMs(uint32_t ms);
void delay_inSecs(uint32_t secs);
void delay_inMins(uint32_t mins);

uint32_t pit_get_ticks(void);
uint64_t pit_get_ticks64(void);

uint64_t pit_get_uptime_us(void);
uint64_t pit_get_uptime_ms(void);
uint32_t pit_get_uptime_secs(void);

uint32_t pit_get_frequency(void);

uint32_t pit_ticks_to_ms(uint32_t ticks);
uint32_t pit_ms_to_ticks(uint32_t ms);
uint64_t pit_ticks_to_us(uint32_t ticks);
uint32_t pit_us_to_ticks(uint32_t us);

int pit_register_callback(pit_callback_t callback, uint32_t interval_ticks, bool oneshot);
void pit_unregister_callback(int handle);
void pit_process_callbacks(void);

void speaker_play(uint32_t freq);
void speaker_stop(void);
void speaker_beep(uint32_t freq, uint32_t duration_ms);

#endif
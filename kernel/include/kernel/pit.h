#ifndef ARCH_X86_64_PIT_H
#define ARCH_X86_64_PIT_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*pit_callback_t)(uint64_t ticks);

extern uint64_t pit_freq;

void pit_init(uint64_t freq);
void pit_set_frequency(uint64_t freq);
void calibrate_pit(void);

void delay(uint64_t ms);
void delay_us(uint64_t us);
void delay_inMs(uint64_t ms);
void delay_inSecs(uint64_t secs);
void delay_inMins(uint64_t mins);

uint64_t pit_get_ticks(void);
uint64_t pit_get_ticks64(void);

uint64_t pit_get_uptime_us(void);
uint64_t pit_get_uptime_ms(void);
uint64_t pit_get_uptime_secs(void);

uint64_t pit_get_frequency(void);

uint64_t pit_ticks_to_ms(uint64_t ticks);
uint64_t pit_ms_to_ticks(uint64_t ms);
uint64_t pit_ticks_to_us(uint64_t ticks);
uint64_t pit_us_to_ticks(uint64_t us);

int pit_register_callback(pit_callback_t callback, uint64_t interval_ticks, bool oneshot);
void pit_unregister_callback(int handle);
void pit_process_callbacks(void);

void speaker_play(uint64_t freq);
void speaker_stop(void);
void speaker_beep(uint64_t freq, uint64_t duration_ms);

#endif
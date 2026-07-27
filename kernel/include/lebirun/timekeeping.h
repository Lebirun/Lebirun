#ifndef LEBIRUN_TIMEKEEPING_H
#define LEBIRUN_TIMEKEEPING_H

#include <stdint.h>

#define TIMEKEEPING_CLOCK_REALTIME 0
#define TIMEKEEPING_CLOCK_MONOTONIC 1
#define TIMEKEEPING_CLOCK_MONOTONIC_RAW 4
#define TIMEKEEPING_CLOCK_BOOTTIME 7

uint64_t timekeeping_monotonic_ns(void);
uint64_t timekeeping_realtime_ns(void);
int timekeeping_get_ns(int clock_id, uint64_t *value);
int timekeeping_set_realtime_ns(uint64_t value);

#endif

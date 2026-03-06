#ifndef _KERNEL_WATCHDOG_H
#define _KERNEL_WATCHDOG_H

#include <stdint.h>

#define WATCHDOG_INTERVAL_MS    5000
#define WATCHDOG_SCHED_TIMEOUT  30000
#define WATCHDOG_MAX_STRIKES    3

void watchdog_init(void);
void watchdog_kick(void);
uint32_t watchdog_get_last_kick(void);

#endif

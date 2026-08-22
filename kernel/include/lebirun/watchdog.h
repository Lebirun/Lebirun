#ifndef _LEBIRUN_WATCHDOG_H
#define _LEBIRUN_WATCHDOG_H

#include <stdint.h>

#define WATCHDOG_INTERVAL_MS    1000
#define WATCHDOG_SCHED_TIMEOUT  3000
#define WATCHDOG_MAX_STRIKES    3

void watchdog_init(void);
void watchdog_set_init_pid(int pid);
void watchdog_kick(void);
void watchdog_disable(void);

#endif

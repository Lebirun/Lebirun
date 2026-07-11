#ifndef RTC_H
#define RTC_H

#include <stdint.h>

void rtc_init(void);
uint64_t rtc_get_time(void);

#endif

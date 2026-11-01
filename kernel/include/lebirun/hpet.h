#ifndef LEBIRUN_HPET_H
#define LEBIRUN_HPET_H

#include <stdint.h>

void hpet_init(uint64_t table_phys);
int hpet_available(void);
uint64_t hpet_get_ns(void);

#endif

#ifndef _LEBIRUN_POWER_H
#define _LEBIRUN_POWER_H

#include <stdint.h>

#define POWER_CMD_SHUTDOWN  0x4321
#define POWER_CMD_REBOOT    0x1234
#define POWER_CMD_SOFTRESET 0x5678

void power_acpi_init(uint64_t fadt_phys);
void power_shutdown(void);
void power_reboot(void);

#endif

#ifndef _KERNEL_POWER_H
#define _KERNEL_POWER_H

#define POWER_CMD_SHUTDOWN  0x4321
#define POWER_CMD_REBOOT    0x1234
#define POWER_CMD_SOFTRESET 0x5678

void power_init(void);
void power_shutdown(void);
void power_reboot(void);

#endif

#ifndef _KERNEL_CMDLINE_H
#define _KERNEL_CMDLINE_H

#include <stdint.h>

#define CMDLINE_MAX 512
#define CMDLINE_INIT_PATH_MAX 128

void cmdline_parse(uint32_t multiboot_flags, uint32_t cmdline_phys);
const char *cmdline_get(void);
const char *cmdline_get_init(void);
int cmdline_get_consoles(void);

#endif

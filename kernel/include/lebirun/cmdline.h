#ifndef _LEBIRUN_CMDLINE_H
#define _LEBIRUN_CMDLINE_H

#include <stdint.h>

void cmdline_parse(const char *cmdline_str);
const char *cmdline_get(void);
const char *cmdline_get_init(void);
int cmdline_get_consoles(void);
const char *cmdline_get_root(void);
int cmdline_get_text_mode(void);
int cmdline_get_lke(void);
void cmdline_reclaim_boot_values(void);

#endif

#ifndef _LEBIRUN_CMDLINE_H
#define _LEBIRUN_CMDLINE_H

#include <stdint.h>

#define CMDLINE_MAX 512
#define CMDLINE_INIT_PATH_MAX 128

void cmdline_parse(const char *cmdline_str);
const char *cmdline_get(void);
const char *cmdline_get_init(void);
int cmdline_get_consoles(void);
const char *cmdline_get_root(void);
int cmdline_get_text_mode(void);
int cmdline_get_lke(void);
int cmdline_get_vringtest(void);

#endif

#ifndef _LEBIRUN_BGA_H
#define _LEBIRUN_BGA_H

#include <stdint.h>

int bga_is_available(void);
uint64_t bga_get_vram_bytes(void);
uint64_t bga_get_lfb_base(void);
int bga_set_mode(uint16_t width, uint16_t height, uint16_t bpp, uint64_t *out_pitch);

#endif

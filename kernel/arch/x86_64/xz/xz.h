#ifndef LEBIRUN_XZ_H
#define LEBIRUN_XZ_H

#include <stdint.h>

int xz_decompress_xz(const uint8_t *source, uint64_t source_size,
                      uint8_t *output, uint64_t output_size,
                      uint64_t *output_length);

#endif

#ifndef LEBIRUN_FSNAME_H
#define LEBIRUN_FSNAME_H

#include <stdint.h>

#define FSNAME_MAX 255

int fsname_to_utf16(const char *s, uint16_t *o);
void fsname_to_utf8(const uint16_t *s, int n, char *o);

#endif

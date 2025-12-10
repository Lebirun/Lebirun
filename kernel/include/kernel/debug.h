#ifndef DEBUG_H
#define DEBUG_H

#include <kernel/common.h>

extern bool debugMode;

#define DPRINTF(fmt, ...) do { if (debugMode) printf(fmt, ##__VA_ARGS__); } while (0)

#endif
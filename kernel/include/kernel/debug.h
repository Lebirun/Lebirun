#ifndef DEBUG_H
#define DEBUG_H

#include <kernel/common.h>
#include <kernel/vring.h>

extern bool debugMode;
extern int debugLevel;

#define DPRINTF1(fmt, ...) do { if (debugMode && debugLevel >= 1) klog_printf(1, fmt, ##__VA_ARGS__); } while (0)
#define DPRINTF2(fmt, ...) do { if (debugMode && debugLevel >= 2) klog_printf(2, fmt, ##__VA_ARGS__); } while (0)
#define DPRINTF3(fmt, ...) do { if (debugMode && debugLevel >= 3) klog_printf(3, fmt, ##__VA_ARGS__); } while (0)
#define DPRINTF4(fmt, ...) do { if (debugMode && debugLevel >= 4) klog_printf(4, fmt, ##__VA_ARGS__); } while (0)
#define DPRINTF5(fmt, ...) do { if (debugMode && debugLevel >= 5) klog_printf(5, fmt, ##__VA_ARGS__); } while (0)

#define DPRINTF(fmt, ...) DPRINTF1(fmt, ##__VA_ARGS__)

#define DEBUG_HEX1(val) do { if (debugMode && debugLevel >= 1) print_hex(val); } while (0)
#define DEBUG_HEX2(val) do { if (debugMode && debugLevel >= 2) print_hex(val); } while (0)
#define DEBUG_HEX3(val) do { if (debugMode && debugLevel >= 3) print_hex(val); } while (0)
#define DEBUG_HEX4(val) do { if (debugMode && debugLevel >= 4) print_hex(val); } while (0)
#define DEBUG_HEX5(val) do { if (debugMode && debugLevel >= 5) print_hex(val); } while (0)

#endif
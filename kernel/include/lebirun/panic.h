#ifndef _LEBIRUN_PANIC_H
#define _LEBIRUN_PANIC_H

#include <lebirun/registers.h>

void kernel_panic(const char *reason, registers_t *regs);
void kernel_panic_msg(const char *fmt, ...);
void kernel_panic_custom(const char *category, const char *fmt, ...);

#define KERNEL_ASSERT(cond) \
    do { if (!(cond)) kernel_panic_msg("ASSERT FAILED: %s at %s:%d", #cond, __FILE__, __LINE__); } while(0)

#define KERNEL_BUG() \
    kernel_panic_msg("BUG: unreachable code at %s:%d", __FILE__, __LINE__)

#define KERNEL_PANIC_OOM(msg) \
    kernel_panic_custom("OUT OF MEMORY", "%s at %s:%d", (msg), __FILE__, __LINE__)

#define KERNEL_PANIC_HEAP(msg) \
    kernel_panic_custom("HEAP CORRUPTION", "%s at %s:%d", (msg), __FILE__, __LINE__)

#define KERNEL_PANIC_SCHED(msg) \
    kernel_panic_custom("SCHEDULER", "%s at %s:%d", (msg), __FILE__, __LINE__)

#define KERNEL_PANIC_FS(msg) \
    kernel_panic_custom("FILESYSTEM", "%s at %s:%d", (msg), __FILE__, __LINE__)

#define KERNEL_PANIC_VMM(msg) \
    kernel_panic_custom("VMM", "%s at %s:%d", (msg), __FILE__, __LINE__)

#define KERNEL_PANIC_DRIVER(msg) \
    kernel_panic_custom("DRIVER", "%s at %s:%d", (msg), __FILE__, __LINE__)

#define KERNEL_PANIC_CUSTOM(category, msg) \
    kernel_panic_custom((category), "%s at %s:%d", (msg), __FILE__, __LINE__)

#endif

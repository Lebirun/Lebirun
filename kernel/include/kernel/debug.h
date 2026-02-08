#ifndef DEBUG_H
#define DEBUG_H

#include <kernel/common.h>
#include <kernel/vring.h>

#ifndef CONFIG_DEBUG_MEMORY
#define CONFIG_DEBUG_MEMORY 0
#endif
#ifndef CONFIG_DEBUG_TASK
#define CONFIG_DEBUG_TASK 0
#endif
#ifndef CONFIG_DEBUG_VFS
#define CONFIG_DEBUG_VFS 0
#endif
#ifndef CONFIG_DEBUG_RAMFS
#define CONFIG_DEBUG_RAMFS 0
#endif
#ifndef CONFIG_DEBUG_INITRD
#define CONFIG_DEBUG_INITRD 0
#endif
#ifndef CONFIG_DEBUG_ELF
#define CONFIG_DEBUG_ELF 0
#endif
#ifndef CONFIG_DEBUG_SYSCALL
#define CONFIG_DEBUG_SYSCALL 0
#endif
#ifndef CONFIG_DEBUG_IDT
#define CONFIG_DEBUG_IDT 0
#endif
#ifndef CONFIG_DEBUG_DRIVER
#define CONFIG_DEBUG_DRIVER 0
#endif
#ifndef CONFIG_DEBUG_FS_EXT4
#define CONFIG_DEBUG_FS_EXT4 0
#endif
#ifndef CONFIG_DEBUG_FS_OTHER
#define CONFIG_DEBUG_FS_OTHER 0
#endif

extern bool debug_memory;
extern bool debug_task;
extern bool debug_vfs;
extern bool debug_ramfs;
extern bool debug_initrd;
extern bool debug_elf;
extern bool debug_syscall;
extern bool debug_idt;
extern bool debug_driver;
extern bool debug_fs_ext4;
extern bool debug_fs_other;

#define DEBUG_MEMORY(fmt, ...) do { if (debug_memory) klog_printf(1, "[MEM] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_TASK(fmt, ...) do { if (debug_task) klog_printf(1, "[TASK] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_VFS(fmt, ...) do { if (debug_vfs) klog_printf(1, "[VFS] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_RAMFS(fmt, ...) do { if (debug_ramfs) klog_printf(1, "[RAMFS] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_INITRD(fmt, ...) do { if (debug_initrd) klog_printf(1, "[INITRD] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_ELF(fmt, ...) do { if (debug_elf) klog_printf(1, "[ELF] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_SYSCALL(fmt, ...) do { if (debug_syscall) klog_printf(1, "[SYSCALL] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_IDT(fmt, ...) do { if (debug_idt) klog_printf(1, "[IDT] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_DRIVER(fmt, ...) do { if (debug_driver) klog_printf(1, "[DRIVER] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_FS_EXT4(fmt, ...) do { if (debug_fs_ext4) klog_printf(1, "[EXT4] " fmt, ##__VA_ARGS__); } while (0)
#define DEBUG_FS_OTHER(fmt, ...) do { if (debug_fs_other) klog_printf(1, "[FS] " fmt, ##__VA_ARGS__); } while (0)

#endif
#ifndef _SYSCALL_DEFS_H
#define _SYSCALL_DEFS_H

#include <kernel/registers.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>
#include <kernel/keyboard.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/mem_map.h>
#include <kernel/initrd.h>
#include <kernel/framebuffer.h>
#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/drivers/net/net.h>
#include <kernel/drivers/net/http.h>

#define SYSCALL_EXIT 0
#define SYSCALL_WRITE 1
#define SYSCALL_GETPID 2
#define SYSCALL_READ 3
#define SYSCALL_YIELD 4
#define SYSCALL_SLEEP 5
#define SYSCALL_WAITPID 6
#define SYSCALL_SBRK 7
#define SYSCALL_MMAP 8
#define SYSCALL_KILL 9
#define SYSCALL_GETTICKS 10
#define SYSCALL_TIME 11
#define SYSCALL_ISATTY 12
#define SYSCALL_FORK 13
#define SYSCALL_EXEC 14
#define SYSCALL_INITRD_COUNT 15
#define SYSCALL_INITRD_STAT 16
#define SYSCALL_INITRD_READ 17
#define SYSCALL_OPEN 18
#define SYSCALL_CLOSE 19
#define SYSCALL_FSTAT 20
#define SYSCALL_FB_PUTPIXEL 21
#define SYSCALL_FB_SETCOLORS 22
#define SYSCALL_FB_GETINFO 23
#define SYSCALL_FB_CLEAR 24
#define SYSCALL_CONSOLE_SWITCH 25
#define SYSCALL_CONSOLE_GETCUR 26
#define SYSCALL_CONSOLE_CLEAR 27
#define SYSCALL_VFS_OPEN 28
#define SYSCALL_VFS_CLOSE 29
#define SYSCALL_VFS_READ 30
#define SYSCALL_VFS_READDIR 31
#define SYSCALL_VFS_STAT 32
#define SYSCALL_VFS_MOUNTS 33
#define SYSCALL_VFS_WRITE 34
#define SYSCALL_VFS_CREATE 35
#define SYSCALL_VFS_MKDIR 36
#define SYSCALL_VFS_UNLINK 37
#define SYSCALL_CONSOLE_SETCURSOR 38
#define SYSCALL_READ_NB 39
#define SYSCALL_SATA_TEST 40
#define SYSCALL_SATA_INFO 41
#define SYSCALL_SATA_SMART 42
#define SYSCALL_SATA_IRQ 43
#define SYSCALL_NET_IFCONFIG 44
#define SYSCALL_NET_PING 45
#define SYSCALL_NET_ARP 46
#define SYSCALL_NET_DNS 47
#define SYSCALL_NET_DHCP 48
#define SYSCALL_NET_GETINFO 49
#define SYSCALL_NET_ARP_GET 50
#define SYSCALL_NET_PING_ONE 51
#define SYSCALL_NET_DNS_RESOLVE 52
#define SYSCALL_NET_HTTP_GET 53

#define NR_SYSCALLS 54

extern void *syscall_table[NR_SYSCALLS];
extern registers_t *fork_regs_ptr;

void syscalls_core_init(void);
void syscalls_process_init(void);
void syscalls_mem_init(void);
void syscalls_time_init(void);
void syscalls_initrd_init(void);
void syscalls_fb_init(void);
void syscalls_console_init(void);
void syscalls_vfs_init(void);
void syscalls_sata_init(void);
void syscalls_net_init(void);

int sys_vfs_readdir(registers_t *regs);

#endif

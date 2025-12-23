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
#define SYSCALL_TCGETATTR 54
#define SYSCALL_TCSETATTR 55
#define SYSCALL_IOCTL 56
#define SYSCALL_TCFLUSH 57
#define SYSCALL_TCFLOW 58
#define SYSCALL_TCDRAIN 59
#define SYSCALL_TCGETPGRP 60
#define SYSCALL_TCSETPGRP 61

#define NR_SYSCALLS 62

#define NCCS 32

#define VEOF     0
#define VEOL     1
#define VERASE   3
#define VKILL    5
#define VINTR    8
#define VQUIT    9
#define VSUSP    10
#define VSTART   12
#define VSTOP    13
#define VMIN     16
#define VTIME    17

#define IGNBRK  0x00001
#define BRKINT  0x00002
#define IGNPAR  0x00004
#define INPCK   0x00010
#define ISTRIP  0x00020
#define INLCR   0x00040
#define IGNCR   0x00080
#define ICRNL   0x00100
#define IXON    0x00200
#define IXOFF   0x00400
#define IXANY   0x00800

#define OPOST   0x00001
#define ONLCR   0x00002

#define CSIZE   0x00300
#define CS5     0x00000
#define CS6     0x00100
#define CS7     0x00200
#define CS8     0x00300
#define CSTOPB  0x00400
#define CREAD   0x00800
#define PARENB  0x01000
#define PARODD  0x02000
#define HUPCL   0x04000
#define CLOCAL  0x08000

#define ECHOKE  0x00001
#define ECHOE   0x00002
#define ECHOK   0x00004
#define ECHO    0x00008
#define ECHONL  0x00010
#define ECHOCTL 0x00040
#define ISIG    0x00080
#define ICANON  0x00100
#define IEXTEN  0x00400
#define NOFLSH  0x80000000

#define TIOCGETA    0x5401
#define TIOCSETA    0x5402
#define TIOCSETAW   0x5403
#define TIOCSETAF   0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define FIONREAD    0x541B
#define FIONBIO     0x5421

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

struct kernel_termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

struct kernel_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

extern struct kernel_termios tty_termios[NUM_CONSOLES];
extern struct kernel_winsize tty_winsize[NUM_CONSOLES];
extern int tty_pgrp[NUM_CONSOLES];

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
void syscalls_termios_init(void);

int sys_vfs_readdir(registers_t *regs);

#endif

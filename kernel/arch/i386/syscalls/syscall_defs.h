#ifndef _SYSCALL_DEFS_H
#define _SYSCALL_DEFS_H

#include <kernel/registers.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
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
#define SYSCALL_WRITEV 62
#define SYSCALL_LSEEK 63
#define SYSCALL_EXECVE 64
#define SYSCALL_DUP 65
#define SYSCALL_DUP2 66
#define SYSCALL_PIPE 67
#define SYSCALL_SIGACTION 68
#define SYSCALL_SIGPROCMASK 69
#define SYSCALL_STAT 71
#define SYSCALL_GETCWD 72
#define SYSCALL_CHDIR 73
#define SYSCALL_CHDIR 73
#define SYSCALL_ACCESS 74
#define SYSCALL_CLOCK_GETTIME 75
#define SYSCALL_GETTIMEOFDAY 76
#define SYSCALL_MUNMAP 77
#define SYSCALL_MPROTECT 78
#define SYSCALL_FCNTL 79
#define SYSCALL_GETDENTS 80
#define SYSCALL_TRUNCATE 81
#define SYSCALL_FTRUNCATE 82
#define SYSCALL_RENAME 83
#define SYSCALL_LINK 84
#define SYSCALL_SYMLINK 85
#define SYSCALL_READLINK 86
#define SYSCALL_UMASK 87

#define NR_SYSCALLS 88

struct kernel_stat {
    unsigned long long st_dev;
    int __st_dev_padding;
    long __st_ino_truncated;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long long st_rdev;
    int __st_rdev_padding;
    long long st_size;
    long st_blksize;
    long long st_blocks;
    struct {
        long tv_sec;
        long tv_nsec;
    } __st_atim32, __st_mtim32, __st_ctim32;
    unsigned long long st_ino;
    struct {
        long tv_sec;
        long tv_nsec;
    } st_atim, st_mtim, st_ctim;
};

struct kernel_sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask;
};

#define S_IFMT  00170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

struct kernel_timespec {
    long tv_sec;
    long tv_nsec;
};

struct kernel_timeval {
    long tv_sec;
    long tv_usec;
};

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
void syscalls_posix_init(void);

int sys_vfs_readdir(registers_t *regs);

#endif

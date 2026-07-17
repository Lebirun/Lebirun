#ifndef _SYSCALL_DEFS_H
#define _SYSCALL_DEFS_H

#include <lebirun/registers.h>
#include <lebirun/kernel_syscall.h>
#include <lebirun/tty.h>
#include <string.h>
#include <lebirun/keyboard.h>
#include <lebirun/mutex.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/initrd.h>
#include <lebirun/framebuffer.h>
#include <lebirun/console.h>
#include <lebirun/cmdline.h>
#include <lebirun/vring.h>
#include <lebirun/vfs.h>
#include <lebirun/pipe.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/http.h>

void syscall_set_exec_completed(void);
int syscall_check_exec_completed(void);
void syscall_clear_exec_completed(void);
void syscall_core_flush_tty_input(int con_id);

#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define ENOTBLK  15
#define EBUSY    16
#define EEXIST   17
#define EXDEV    18
#define ENODEV   19
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define EMFILE   24
#define ENOTTY   25
#define ETXTBSY  26
#define EFBIG    27
#define ENOSPC   28
#define ESPIPE   29
#define EROFS    30
#define EMLINK   31
#define EPIPE    32
#define EDOM     33
#define ERANGE   34
#define EDEADLK  35
#define ENAMETOOLONG 36
#define ENOLCK   37
#define ENOSYS   38
#define ENOTEMPTY 39
#define ELOOP    40
#define EWOULDBLOCK EAGAIN
#define ENOMSG   42
#define EIDRM    43
#define ENOSTR   60
#define ENODATA  61
#define ETIME    62
#define ENOSR    63
#define ENOLINK  67
#define EPROTO   71
#define EBADMSG  74
#define EOVERFLOW 75
#define EILSEQ   84
#define ERESTART 85
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP 95
#define ENOTSUP  EOPNOTSUPP
#define EPFNOSUPPORT 96
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ENETRESET 102
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS  105
#define EISCONN  106
#define ENOTCONN 107
#define ESHUTDOWN 108
#define ETOOMANYREFS 109
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTDOWN 112
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define ESTALE   116

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

#define SYSCALL_SELECT 88
#define SYSCALL_POLL 89
#define SYSCALL_PPOLL 90

#define SYSCALL_SOCKET 91
#define SYSCALL_SOCKETPAIR 92
#define SYSCALL_BIND 93
#define SYSCALL_CONNECT 94
#define SYSCALL_LISTEN 95
#define SYSCALL_ACCEPT 96
#define SYSCALL_ACCEPT4 97
#define SYSCALL_GETSOCKOPT 98
#define SYSCALL_SETSOCKOPT 99
#define SYSCALL_GETSOCKNAME 100
#define SYSCALL_GETPEERNAME 101
#define SYSCALL_SENDTO 102
#define SYSCALL_SENDMSG 103
#define SYSCALL_RECVFROM 104
#define SYSCALL_RECVMSG 105
#define SYSCALL_SHUTDOWN 106

#define SYSCALL_OPENAT 107
#define SYSCALL_MKDIRAT 108
#define SYSCALL_MKNODAT 109
#define SYSCALL_FCHOWNAT 110
#define SYSCALL_UNLINKAT 111
#define SYSCALL_RENAMEAT 112
#define SYSCALL_LINKAT 113
#define SYSCALL_SYMLINKAT 114
#define SYSCALL_READLINKAT 115
#define SYSCALL_FCHMODAT 116
#define SYSCALL_FACCESSAT 117
#define SYSCALL_FSTATAT 118
#define SYSCALL_UTIMENSAT 119
#define SYSCALL_RENAMEAT2 120

#define SYSCALL_RT_SIGACTION 121
#define SYSCALL_RT_SIGPROCMASK 122
#define SYSCALL_RT_SIGPENDING 123
#define SYSCALL_RT_SIGSUSPEND 124
#define SYSCALL_RT_SIGRETURN 125
#define SYSCALL_RT_SIGTIMEDWAIT 126
#define SYSCALL_RT_SIGQUEUEINFO 127
#define SYSCALL_TGKILL 128
#define SYSCALL_TKILL 129
#define SYSCALL_SIGALTSTACK 130
#define SYSCALL_PAUSE 131
#define SYSCALL_ALARM 132

#define SYSCALL_GETUID 133
#define SYSCALL_GETGID 134
#define SYSCALL_GETEUID 135
#define SYSCALL_GETEGID 136
#define SYSCALL_SETUID 137
#define SYSCALL_SETGID 138
#define SYSCALL_SETEUID 139
#define SYSCALL_SETEGID 140
#define SYSCALL_SETREUID 141
#define SYSCALL_SETREGID 142
#define SYSCALL_SETRESUID 143
#define SYSCALL_SETRESGID 144
#define SYSCALL_GETRESUID 145
#define SYSCALL_GETRESGID 146
#define SYSCALL_SETFSUID 147
#define SYSCALL_SETFSGID 148
#define SYSCALL_GETGROUPS 149
#define SYSCALL_SETGROUPS 150
#define SYSCALL_GETPGID 151
#define SYSCALL_SETPGID 152
#define SYSCALL_GETPGRP 153
#define SYSCALL_SETSID 154
#define SYSCALL_GETSID 155
#define SYSCALL_GETPPID 156
#define SYSCALL_GETPID2 157
#define SYSCALL_GETTID 158

#define SYSCALL_UNAME 159
#define SYSCALL_SYSINFO 160
#define SYSCALL_GETRLIMIT 161
#define SYSCALL_SETRLIMIT 162
#define SYSCALL_GETRUSAGE 163
#define SYSCALL_PRLIMIT64 164

#define SYSCALL_MMAP2 165
#define SYSCALL_MREMAP 166
#define SYSCALL_MADVISE 167
#define SYSCALL_MINCORE 168

#define SYSCALL_PREAD64 169
#define SYSCALL_PWRITE64 170
#define SYSCALL_READV 171
#define SYSCALL_FCHDIR 172
#define SYSCALL_FCHMOD 173
#define SYSCALL_FCHOWN 174
#define SYSCALL_FSYNC 175
#define SYSCALL_FDATASYNC 176
#define SYSCALL_FLOCK 177
#define SYSCALL_GETDENTS64 178

#define SYSCALL_DUP3 179
#define SYSCALL_PIPE2 180
#define SYSCALL_EVENTFD 181
#define SYSCALL_EVENTFD2 182
#define SYSCALL_EPOLL_CREATE 183
#define SYSCALL_EPOLL_CREATE1 184
#define SYSCALL_EPOLL_CTL 185
#define SYSCALL_EPOLL_WAIT 186
#define SYSCALL_EPOLL_PWAIT 187

#define SYSCALL_SET_TID_ADDRESS 188
#define SYSCALL_FUTEX 189
#define SYSCALL_SET_ROBUST_LIST 190
#define SYSCALL_GET_ROBUST_LIST 191
#define SYSCALL_CLONE 192
#define SYSCALL_VFORK 193
#define SYSCALL_WAIT4 194
#define SYSCALL_WAITID 195

#define SYSCALL_GETRANDOM 196
#define SYSCALL_PRCTL 197
#define SYSCALL_ARCH_PRCTL 198

#define SYSCALL_TIMERFD_CREATE 199
#define SYSCALL_TIMERFD_SETTIME 200
#define SYSCALL_TIMERFD_GETTIME 201
#define SYSCALL_SIGNALFD 202
#define SYSCALL_SIGNALFD4 203
#define SYSCALL_INOTIFY_INIT 204
#define SYSCALL_INOTIFY_INIT1 205
#define SYSCALL_INOTIFY_ADD_WATCH 206
#define SYSCALL_INOTIFY_RM_WATCH 207
#define SYSCALL_POSIX_OPENPT 208
#define SYSCALL_GRANTPT 209
#define SYSCALL_UNLOCKPT 210
#define SYSCALL_PTSNAME 211

#define SYSCALL_SETITIMER 212
#define SYSCALL_GETITIMER 213
#define SYSCALL_CHMOD 214
#define SYSCALL_CHOWN 215
#define SYSCALL_LCHOWN 216
#define SYSCALL_NANOSLEEP 217
#define SYSCALL_SIGRETURN 218
#define SYSCALL_SETENV 219
#define SYSCALL_GETENV 220
#define SYSCALL_UNSETENV 221
#define SYSCALL_CLEARENV 222

#define SYSCALL_PTHREAD_CREATE 223
#define SYSCALL_PTHREAD_EXIT 224
#define SYSCALL_PTHREAD_JOIN 225
#define SYSCALL_PTHREAD_DETACH 226
#define SYSCALL_PTHREAD_SELF 227
#define SYSCALL_PTHREAD_MUTEX_INIT 228
#define SYSCALL_PTHREAD_MUTEX_DESTROY 229
#define SYSCALL_PTHREAD_MUTEX_LOCK 230
#define SYSCALL_PTHREAD_MUTEX_TRYLOCK 231
#define SYSCALL_PTHREAD_MUTEX_UNLOCK 232
#define SYSCALL_PTHREAD_COND_INIT 233
#define SYSCALL_PTHREAD_COND_DESTROY 234
#define SYSCALL_PTHREAD_COND_WAIT 235
#define SYSCALL_PTHREAD_COND_SIGNAL 236
#define SYSCALL_PTHREAD_COND_BROADCAST 237

#define SYSCALL_SHMGET 238
#define SYSCALL_SHMAT 239
#define SYSCALL_SHMDT 240
#define SYSCALL_SHMCTL 241
#define SYSCALL_SHM_OPEN 242
#define SYSCALL_SHM_UNLINK 243

#define SYSCALL_DLOPEN 244
#define SYSCALL_DLSYM 245
#define SYSCALL_DLCLOSE 246
#define SYSCALL_DLERROR 247

#define SYSCALL_REGCOMP 248
#define SYSCALL_REGEXEC 249
#define SYSCALL_REGFREE 250
#define SYSCALL_REGERROR 251
#define SYSCALL_FNMATCH 252
#define SYSCALL_GLOB 253
#define SYSCALL_GLOBFREE 254
#define SYSCALL_SSCANF 255
#define SYSCALL_SCANF_GETCHAR 256
#define SYSCALL_REGSUB 257
#define SYSCALL_REGEXEC_EX 258

#define SYSCALL_FB_SET_MODE 259
#define SYSCALL_FB_GET_DETAILED_INFO 260
#define SYSCALL_FB_GET_CAPS 261

#define SYSCALL_STATFS 262
#define SYSCALL_FSTATFS 263
#define SYSCALL_NET_HTTP_POST 264

#define SYSCALL_PSELECT6 265
#define SYSCALL_GETPRIORITY 266
#define SYSCALL_SETPRIORITY 267
#define SYSCALL_PIVOT_ROOT 268
#define SYSCALL_REBOOT 269
#define SYSCALL_CONSOLE_SETID 270
#define SYSCALL_CRYPTO 271
#define SYSCALL_VFS_MOUNT 272
#define SYSCALL_VFS_UMOUNT 273
#define SYSCALL_PANIC 274
#define SYSCALL_BLOCKDEV_RESCAN 275
#define SYSCALL_NET_HTTP_GET_ALLOC 276
#define SYSCALL_LKE_LOAD 277
#define SYSCALL_LKE_UNLOAD 278
#define SYSCALL_LKE_LIST 279
#define SYSCALL_IPV67 280

#define NR_SYSCALLS 284

struct kernel_stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int __pad0;
    unsigned long long st_rdev;
    long long st_size;
    long st_blksize;
    long long st_blocks;
    struct {
        long tv_sec;
        long tv_nsec;
    } st_atim, st_mtim, st_ctim;
    long __unused[3];
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

#define TIOCGETA    TCGETS
#define TIOCSETA    TCSETS
#define TIOCSETAW   TCSETSW
#define TIOCSETAF   TCSETSF

#define kernel_termios termios

#define kernel_winsize winsize

extern struct kernel_termios *tty_termios;
extern struct kernel_winsize *tty_winsize;
extern int *tty_pgrp;
extern int tty_count;

extern void **syscall_table;

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
void syscalls_select_init(void);
void syscalls_socket_init(void);
void syscalls_at_init(void);
void syscalls_signal_init(void);
int sys_kill_impl(int pid, const char *sig_ptr, int unused);
void syscalls_ids_init(void);
void syscalls_misc_init(void);
void syscalls_epoll_init(void);
void syscalls_pthread_init(void);
void syscalls_shm_init(void);
void syscalls_dl_init(void);
void syscalls_regex_init(void);
void syscalls_power_init(void);
void syscalls_crypto_init(void);

int sys_vfs_readdir(registers_t *regs);

void procfs_init(void);
void devfs_init(void);
int devfs_register_blockdev(const char *name, uint32_t port_index);
int devfs_register_partition(const char *name, uint32_t port_index,
                             uint64_t start_lba, uint64_t sector_count);
uint64_t devfs_get_partition_start(vfs_node_t *node);
int devfs_is_partition(vfs_node_t *node);
int devfs_rescan_partitions(const char *devname);
void devfs_register_initrd(void);

#endif

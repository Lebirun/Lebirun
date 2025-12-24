#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>

typedef long ssize_t;
typedef unsigned int mode_t;
typedef long off_t;

#include <errno.h>

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOTTY  25
#define ENOSPC  28
#define EROFS   30
#define ENOSYS  38
#define ENOTEMPTY 39

#define LEBIRUN_SYSCALL_FLAG 0x80000000

#define SYS_EXIT     (0 | LEBIRUN_SYSCALL_FLAG)
#define SYS_WRITE    (1 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETPID   (2 | LEBIRUN_SYSCALL_FLAG)
#define SYS_READ     (3 | LEBIRUN_SYSCALL_FLAG)
#define SYS_YIELD    (4 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SLEEP    (5 | LEBIRUN_SYSCALL_FLAG)
#define SYS_WAITPID  (6 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SBRK     (7 | LEBIRUN_SYSCALL_FLAG)
#define SYS_MMAP     (8 | LEBIRUN_SYSCALL_FLAG)
#define SYS_KILL     (9 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETTICKS (10 | LEBIRUN_SYSCALL_FLAG)
#define SYS_TIME     (11 | LEBIRUN_SYSCALL_FLAG)
#define SYS_ISATTY   (12 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FORK     (13 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EXEC     (14 | LEBIRUN_SYSCALL_FLAG)
#define SYS_INITRD_COUNT (15 | LEBIRUN_SYSCALL_FLAG)
#define SYS_INITRD_STAT (16 | LEBIRUN_SYSCALL_FLAG)
#define SYS_INITRD_READ (17 | LEBIRUN_SYSCALL_FLAG)
#define SYS_OPEN (18 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CLOSE (19 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FSTAT (20 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_OPEN (28 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_CLOSE (29 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_READ (30 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_READDIR (31 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_STAT (32 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_MOUNTS (33 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_WRITE (34 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_CREATE (35 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_MKDIR (36 | LEBIRUN_SYSCALL_FLAG)
#define SYS_VFS_UNLINK (37 | LEBIRUN_SYSCALL_FLAG)
#define SYS_READ_NB (39 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_SWITCH (25 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_GETCUR (26 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_CLEAR (27 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONSOLE_SETCURSOR (38 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_PUTPIXEL (21 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_SETCOLORS (22 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_GETINFO (23 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FB_CLEAR (24 | LEBIRUN_SYSCALL_FLAG)
#define SYS_DUP (65 | LEBIRUN_SYSCALL_FLAG)
#define SYS_DUP2 (66 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PIPE (67 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SIGACTION (68 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SIGPROCMASK (69 | LEBIRUN_SYSCALL_FLAG)
#define SYS_STAT (71 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETCWD (72 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CHDIR (73 | LEBIRUN_SYSCALL_FLAG)
#define SYS_ACCESS (74 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CLOCK_GETTIME (75 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETTIMEOFDAY (76 | LEBIRUN_SYSCALL_FLAG)
#define SYS_MUNMAP (77 | LEBIRUN_SYSCALL_FLAG)
#define SYS_MPROTECT (78 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EXECVE (64 | LEBIRUN_SYSCALL_FLAG)
#define SYS_LSEEK (63 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FCNTL (79 | LEBIRUN_SYSCALL_FLAG)
#define SYS_TRUNCATE (81 | LEBIRUN_SYSCALL_FLAG)
#define SYS_FTRUNCATE (82 | LEBIRUN_SYSCALL_FLAG)
#define SYS_RENAME (83 | LEBIRUN_SYSCALL_FLAG)
#define SYS_LINK (84 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SYMLINK (85 | LEBIRUN_SYSCALL_FLAG)
#define SYS_READLINK (86 | LEBIRUN_SYSCALL_FLAG)
#define SYS_UMASK (87 | LEBIRUN_SYSCALL_FLAG)

static inline int syscall0(int num) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline int syscall1(int num, int arg1) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1)
        : "memory"
    );
    return ret;
}

static inline int syscall2(int num, int arg1, int arg2) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2)
        : "memory"
    );
    return ret;
}

static inline int syscall3(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int syscall4(int num, int arg1, int arg2, int arg3, int arg4) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4)
        : "memory"
    );
    return ret;
}

static uintptr_t __brk_cur = 0;

int brk(void *addr) {
    uintptr_t result = (uintptr_t)syscall1(SYS_SBRK, (int)(uintptr_t)addr);
    if (result == (uintptr_t)-1) {
        return -1;
    }
    __brk_cur = result;
    if (addr == 0 || result >= (uintptr_t)addr) {
        return 0; 
    }
    return -1; 
}

void *sbrk(intptr_t inc) {
    if (__brk_cur == 0) {
        __brk_cur = (uintptr_t)syscall1(SYS_SBRK, 0);
        if (__brk_cur == (uintptr_t)-1) {
            return (void *)-1;
        }
    }
    
    if (inc == 0) {
        return (void *)__brk_cur;
    }
    
    uintptr_t old_brk = __brk_cur;
    uintptr_t new_brk = old_brk + (uintptr_t)inc;
    
    uintptr_t result = (uintptr_t)syscall1(SYS_SBRK, (int)new_brk);
    if (result == (uintptr_t)-1 || result < new_brk) {
        return (void *)-1;
    }
    
    __brk_cur = result;
    return (void *)old_brk;
}

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

void exit(int status) {
    _exit(status);
}

int read(int fd, void *buf, size_t count) {
    int ret = syscall3(SYS_READ, fd, (int)buf, (int)count);
    if (ret < 0) { errno = EBADF; return -1; }
    return ret;
}

int write(int fd, const void *buf, size_t count) {
    int ret = syscall3(SYS_WRITE, fd, (int)buf, (int)count);
    if (ret < 0) { errno = EBADF; return -1; }
    return ret;
}

struct iovec {
    void *iov_base;
    size_t iov_len;
};

#define SYS_WRITEV (62 | LEBIRUN_SYSCALL_FLAG)

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return syscall3(SYS_WRITEV, fd, (int)iov, iovcnt);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (total > 0) ? total : n;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

int getpid(void) {
    return syscall0(SYS_GETPID);
}

int exec(const void *bin, unsigned int size) {
    return syscall2(SYS_EXEC, (int)bin, (int)size);
}

int fork(void) {
    return syscall0(SYS_FORK);
}

int waitpid(int pid, int *status, int options) {
    (void)options;
    return syscall2(SYS_WAITPID, pid, (int)status);
}

unsigned int getticks(void) {
    return (unsigned int)syscall0(SYS_GETTICKS);
}

int initrd_count(void) {
    return syscall0(SYS_INITRD_COUNT);
}

int initrd_stat(int index, char *name, unsigned int *length) {
    return syscall3(SYS_INITRD_STAT, index, (int)name, (int)length);
}

int initrd_read(int index, void *buf, unsigned int maxlen) {
    return syscall3(SYS_INITRD_READ, index, (int)buf, (int)maxlen);
}

int vfs_open(const char *path, int flags) {
    return syscall2(SYS_VFS_OPEN, (int)path, flags);
}

int vfs_close_fd(int fd) {
    return syscall1(SYS_VFS_CLOSE, fd);
}

int vfs_read_fd(int fd, void *buf, unsigned int count) {
    return syscall3(SYS_VFS_READ, fd, (int)buf, (int)count);
}

int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index) {
    return syscall4(SYS_VFS_READDIR, fd, (int)name, (int)type, (int)index);
}

int vfs_stat(int fd, unsigned int *size, unsigned int *type) {
    return syscall3(SYS_VFS_STAT, fd, (int)size, (int)type);
}

int vfs_mounts(void) {
    return syscall0(SYS_VFS_MOUNTS);
}

int vfs_write_fd(int fd, const void *buf, unsigned int count) {
    return syscall3(SYS_VFS_WRITE, fd, (int)buf, (int)count);
}

int vfs_create(const char *path, unsigned int perms) {
    return syscall2(SYS_VFS_CREATE, (int)path, (int)perms);
}

int vfs_mkdir(const char *path, unsigned int perms) {
    return syscall2(SYS_VFS_MKDIR, (int)path, (int)perms);
}

int vfs_unlink(const char *path) {
    return syscall1(SYS_VFS_UNLINK, (int)path);
}

int read_nb(int fd, void *buf, size_t count) {
    return syscall3(SYS_READ_NB, fd, (int)buf, (int)count);
}

int sleep_ms(int ms) {
    return syscall1(SYS_SLEEP, ms);
}

int console_switch(int console_num) {
    return syscall1(SYS_CONSOLE_SWITCH, console_num);
}

int console_getcur(void) {
    return syscall0(SYS_CONSOLE_GETCUR);
}

int console_clear(int console_num) {
    return syscall1(SYS_CONSOLE_CLEAR, console_num);
}

int console_setcursor(int x, int y) {
    return syscall2(SYS_CONSOLE_SETCURSOR, x, y);
}

int fb_setcolors(unsigned int fg, unsigned int bg) {
    return syscall2(SYS_FB_SETCOLORS, (int)fg, (int)bg);
}

int fb_getinfo(unsigned int *width, unsigned int *height, unsigned int *bpp, 
               unsigned int *font_height, unsigned int *rows, unsigned int *cursor_row) {
    unsigned int info[6];
    int ret = syscall1(SYS_FB_GETINFO, (int)info);
    if (ret == 0) {
        if (width) *width = info[0];
        if (height) *height = info[1];
        if (bpp) *bpp = info[2];
        if (font_height) *font_height = info[3];
        if (rows) *rows = info[4];
        if (cursor_row) *cursor_row = info[5];
    }
    return ret;
}

int fb_clear(void) {
    return syscall0(SYS_FB_CLEAR);
}

int dup(int oldfd) {
    return syscall1(SYS_DUP, oldfd);
}

int dup2(int oldfd, int newfd) {
    return syscall2(SYS_DUP2, oldfd, newfd);
}

int pipe(int pipefd[2]) {
    return syscall1(SYS_PIPE, (int)pipefd);
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    return syscall3(SYS_EXECVE, (int)pathname, (int)argv, (int)envp);
}

char *getcwd(char *buf, size_t size) {
    int ret = syscall2(SYS_GETCWD, (int)buf, (int)size);
    if (ret < 0) return (void*)0;
    return buf;
}

int chdir(const char *path) {
    return syscall1(SYS_CHDIR, (int)path);
}

int access(const char *pathname, int mode) {
    return syscall2(SYS_ACCESS, (int)pathname, mode);
}

struct stat;
int stat(const char *pathname, struct stat *statbuf) {
    int ret = syscall2(SYS_STAT, (int)pathname, (int)statbuf);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}

int fstat(int fd, struct stat *statbuf) {
    int ret = syscall2(SYS_FSTAT, fd, (int)statbuf);
    if (ret < 0) { errno = EBADF; return -1; }
    return 0;
}

int lstat(const char *pathname, struct stat *statbuf) {
    int ret = syscall2(SYS_STAT, (int)pathname, (int)statbuf);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    return syscall3(SYS_LSEEK, fd, (int)offset, whence);
}

struct sigaction;
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return syscall3(SYS_SIGACTION, signum, (int)act, (int)oldact);
}

typedef unsigned long sigset_t;
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return syscall3(SYS_SIGPROCMASK, how, (int)set, (int)oldset);
}

int clock_gettime(int clockid, struct timespec *tp) {
    return syscall2(SYS_CLOCK_GETTIME, clockid, (int)tp);
}

struct timeval;
struct timezone;
int gettimeofday(struct timeval *tv, struct timezone *tz) {
    return syscall2(SYS_GETTIMEOFDAY, (int)tv, (int)tz);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)flags; (void)fd; (void)offset;
    return (void*)syscall2(SYS_MMAP, (int)length, prot);
}

int munmap(void *addr, size_t length) {
    return syscall2(SYS_MUNMAP, (int)addr, (int)length);
}

int mprotect(void *addr, size_t len, int prot) {
    return syscall3(SYS_MPROTECT, (int)addr, (int)len, prot);
}

int isatty(int fd) {
    return syscall1(SYS_ISATTY, fd);
}

int close(int fd) {
    int ret = syscall1(SYS_VFS_CLOSE, fd);
    if (ret < 0) { errno = EBADF; return -1; }
    return 0;
}

int open(const char *pathname, int flags, ...) {
    int ret = syscall2(SYS_VFS_OPEN, (int)pathname, flags);
    if (ret < 0) { errno = ENOENT; return -1; }
    return ret;
}

int creat(const char *pathname, mode_t mode) {
    (void)mode;
    return open(pathname, 0x241);
}

int kill(int pid, int sig) {
    return syscall2(SYS_KILL, pid, sig);
}

int fcntl(int fd, int cmd, ...) {
    return syscall2(SYS_FCNTL, fd, cmd);
}

int truncate(const char *path, off_t length) {
    return syscall2(SYS_TRUNCATE, (int)path, (int)length);
}

int ftruncate(int fd, off_t length) {
    return syscall2(SYS_FTRUNCATE, fd, (int)length);
}

int rename(const char *oldpath, const char *newpath) {
    return syscall2(SYS_RENAME, (int)oldpath, (int)newpath);
}

int link(const char *oldpath, const char *newpath) {
    return syscall2(SYS_LINK, (int)oldpath, (int)newpath);
}

int symlink(const char *target, const char *linkpath) {
    return syscall2(SYS_SYMLINK, (int)target, (int)linkpath);
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    return syscall3(SYS_READLINK, (int)pathname, (int)buf, (int)bufsiz);
}

mode_t umask(mode_t mask) {
    return syscall1(SYS_UMASK, mask);
}

int rmdir(const char *pathname) {
    return syscall1(SYS_VFS_UNLINK, (int)pathname);
}

int unlink(const char *pathname) {
    return syscall1(SYS_VFS_UNLINK, (int)pathname);
}

int mkdir(const char *pathname, mode_t mode) {
    return syscall2(SYS_VFS_MKDIR, (int)pathname, mode);
}

time_t time(time_t *tloc) {
    return (time_t)syscall1(SYS_TIME, (int)tloc);
}

unsigned int sleep(unsigned int seconds) {
    syscall1(SYS_SLEEP, seconds * 1000);
    return 0;
}

int usleep(unsigned int usec) {
    int ms = (usec + 999) / 1000;
    if (ms < 1) ms = 1;
    return syscall1(SYS_SLEEP, ms);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    if (!req) return -1;
    long long msll = (long long)req->tv_sec * 1000LL + (long long)(req->tv_nsec / 1000000L);
    if (msll < 1) msll = 1;
    int ms = (msll > INT_MAX) ? INT_MAX : (int)msll;
    return syscall1(SYS_SLEEP, ms);
}

static char *env_vars[64];
static int env_count = 0;
static int env_initialized = 0;

static void init_default_env(void) {
    if (env_initialized) return;
    env_initialized = 1;
    env_vars[0] = "PATH=/bin:/usr/bin";
    env_vars[1] = "HOME=/";
    env_vars[2] = "TERM=vt100";
    env_vars[3] = "USER=root";
    env_vars[4] = "SHELL=/bin/sh";
    env_count = 5;
}

char *getenv(const char *name) {
    init_default_env();
    if (!name) return (void*)0;
    int nlen = 0;
    while (name[nlen]) nlen++;
    for (int i = 0; i < env_count; i++) {
        if (!env_vars[i]) continue;
        int j = 0;
        while (env_vars[i][j] && env_vars[i][j] != '=' && name[j] && env_vars[i][j] == name[j]) j++;
        if (j == nlen && env_vars[i][j] == '=') return &env_vars[i][j + 1];
    }
    return (void*)0;
}

int setenv(const char *name, const char *value, int overwrite) {
    init_default_env();
    if (!name || !value) return -1;
    char *existing = getenv(name);
    if (existing && !overwrite) return 0;
    int nlen = 0, vlen = 0;
    while (name[nlen]) nlen++;
    while (value[vlen]) vlen++;
    if (env_count >= 63) return -1;
    static char env_buf[64][128];
    int i = 0;
    while (i < nlen && i < 60) { env_buf[env_count][i] = name[i]; i++; }
    env_buf[env_count][i++] = '=';
    int j = 0;
    while (j < vlen && i < 127) { env_buf[env_count][i++] = value[j++]; }
    env_buf[env_count][i] = '\0';
    env_vars[env_count] = env_buf[env_count];
    env_count++;
    return 0;
}

int unsetenv(const char *name) {
    init_default_env();
    if (!name) return -1;
    return 0;
}

char **environ = (char**)0;

int getuid(void) { return 0; }
int geteuid(void) { return 0; }
int getgid(void) { return 0; }
int getegid(void) { return 0; }

int setuid(int uid) { (void)uid; return 0; }
int setgid(int gid) { (void)gid; return 0; }
int seteuid(int uid) { (void)uid; return 0; }
int setegid(int gid) { (void)gid; return 0; }

int getppid(void) { return 1; }

long sysconf(int name) {
    switch (name) {
        case 30: return 4096;
        case 0: return 4096;
        case 4: return 1024;
        default: return -1;
    }
}

int ioctl(int fd, unsigned long request, ...) {
    return syscall2(56 | 0x80000000, fd, (int)request);
}

int tcgetattr(int fd, void *termios_p) {
    return syscall2(54 | 0x80000000, fd, (int)termios_p);
}

int tcsetattr(int fd, int optional_actions, const void *termios_p) {
    return syscall3(55 | 0x80000000, fd, optional_actions, (int)termios_p);
}

int tcflush(int fd, int queue_selector) {
    return syscall2(57 | 0x80000000, fd, queue_selector);
}

int tcdrain(int fd) {
    return syscall1(59 | 0x80000000, fd);
}

int tcflow(int fd, int action) {
    return syscall2(58 | 0x80000000, fd, action);
}

void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

int raise(int sig) {
    return kill(getpid(), sig);
}

typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler) {
    (void)signum; (void)handler;
    return (sighandler_t)0;
}

int sigemptyset(void *set) {
    if (set) memset(set, 0, sizeof(unsigned long));
    return 0;
}

int sigfillset(void *set) {
    if (set) memset(set, 0xff, sizeof(unsigned long));
    return 0;
}

int sigaddset(void *set, int signum) {
    if (set && signum > 0 && signum < 32) {
        *(unsigned long *)set |= (1UL << signum);
    }
    return 0;
}

int sigdelset(void *set, int signum) {
    if (set && signum > 0 && signum < 32) {
        *(unsigned long *)set &= ~(1UL << signum);
    }
    return 0;
}

int sigismember(const void *set, int signum) {
    if (set && signum > 0 && signum < 32) {
        return (*(const unsigned long *)set >> signum) & 1;
    }
    return 0;
}
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
    return syscall3(SYS_WAITPID, pid, (int)status, options);
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

char *getenv(const char *name) {
    static char env_buf[256];
    int ret = syscall3(220 | LEBIRUN_SYSCALL_FLAG, (int)name, (int)env_buf, 256);
    if (ret < 0) return (char *)0;
    return env_buf;
}

int setenv(const char *name, const char *value, int overwrite) {
    return syscall3(219 | LEBIRUN_SYSCALL_FLAG, (int)name, (int)value, overwrite);
}

int unsetenv(const char *name) {
    return syscall1(221 | LEBIRUN_SYSCALL_FLAG, (int)name);
}

int clearenv(void) {
    return syscall0(222 | LEBIRUN_SYSCALL_FLAG);
}

int putenv(char *string) {
    if (!string) return -1;
    char *eq = string;
    while (*eq && *eq != '=') eq++;
    if (!*eq) return -1;
    *eq = '\0';
    int ret = setenv(string, eq + 1, 1);
    *eq = '=';
    return ret;
}

char **environ = (char**)0;

int getuid(void) { return syscall0(133 | LEBIRUN_SYSCALL_FLAG); }
int geteuid(void) { return syscall0(135 | LEBIRUN_SYSCALL_FLAG); }
int getgid(void) { return syscall0(134 | LEBIRUN_SYSCALL_FLAG); }
int getegid(void) { return syscall0(136 | LEBIRUN_SYSCALL_FLAG); }

int setuid(int uid) { return syscall1(137 | LEBIRUN_SYSCALL_FLAG, uid); }
int setgid(int gid) { return syscall1(138 | LEBIRUN_SYSCALL_FLAG, gid); }
int seteuid(int uid) { return syscall1(139 | LEBIRUN_SYSCALL_FLAG, uid); }
int setegid(int gid) { return syscall1(140 | LEBIRUN_SYSCALL_FLAG, gid); }

int getppid(void) { return syscall0(156 | LEBIRUN_SYSCALL_FLAG); }
int gettid(void) { return syscall0(158 | LEBIRUN_SYSCALL_FLAG); }

int getpgid(int pid) { return syscall1(151 | LEBIRUN_SYSCALL_FLAG, pid); }
int setpgid(int pid, int pgid) { return syscall2(152 | LEBIRUN_SYSCALL_FLAG, pid, pgid); }
int getpgrp(void) { return syscall0(153 | LEBIRUN_SYSCALL_FLAG); }
int setsid(void) { return syscall0(154 | LEBIRUN_SYSCALL_FLAG); }
int getsid(int pid) { return syscall1(155 | LEBIRUN_SYSCALL_FLAG, pid); }

int dup3(int oldfd, int newfd, int flags) {
    return syscall3(179 | LEBIRUN_SYSCALL_FLAG, oldfd, newfd, flags);
}

int pipe2(int pipefd[2], int flags) {
    return syscall2(180 | LEBIRUN_SYSCALL_FLAG, (int)pipefd, flags);
}

int fchdir(int fd) {
    return syscall1(172 | LEBIRUN_SYSCALL_FLAG, fd);
}

int fchmod(int fd, mode_t mode) {
    return syscall2(173 | LEBIRUN_SYSCALL_FLAG, fd, mode);
}

int fchown(int fd, int owner, int group) {
    return syscall3(174 | LEBIRUN_SYSCALL_FLAG, fd, owner, group);
}

int fsync(int fd) {
    return syscall1(175 | LEBIRUN_SYSCALL_FLAG, fd);
}

int fdatasync(int fd) {
    return syscall1(176 | LEBIRUN_SYSCALL_FLAG, fd);
}

int flock(int fd, int operation) {
    return syscall2(177 | LEBIRUN_SYSCALL_FLAG, fd, operation);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return syscall4(169 | LEBIRUN_SYSCALL_FLAG, fd, (int)buf, (int)count, (int)offset);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return syscall4(170 | LEBIRUN_SYSCALL_FLAG, fd, (int)buf, (int)count, (int)offset);
}

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int uname(struct utsname *buf) {
    return syscall1(159 | LEBIRUN_SYSCALL_FLAG, (int)buf);
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    return syscall3(196 | LEBIRUN_SYSCALL_FLAG, (int)buf, (int)buflen, flags);
}

int pause(void) {
    return syscall0(131 | LEBIRUN_SYSCALL_FLAG);
}

unsigned int alarm(unsigned int seconds) {
    return syscall1(132 | LEBIRUN_SYSCALL_FLAG, seconds);
}

int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    return syscall3(212 | LEBIRUN_SYSCALL_FLAG, which, (int)new_value, (int)old_value);
}

int getitimer(int which, struct itimerval *curr_value) {
    return syscall2(213 | LEBIRUN_SYSCALL_FLAG, which, (int)curr_value);
}

int chmod(const char *pathname, int mode) {
    return syscall2(214 | LEBIRUN_SYSCALL_FLAG, (int)pathname, mode);
}

int chown(const char *pathname, int owner, int group) {
    return syscall3(215 | LEBIRUN_SYSCALL_FLAG, (int)pathname, owner, group);
}

int lchown(const char *pathname, int owner, int group) {
    return syscall3(216 | LEBIRUN_SYSCALL_FLAG, (int)pathname, owner, group);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return syscall2(217 | LEBIRUN_SYSCALL_FLAG, (int)req, (int)rem);
}

int wait4(int pid, int *wstatus, int options, void *rusage) {
    (void)rusage;
    return syscall3(194 | LEBIRUN_SYSCALL_FLAG, pid, (int)wstatus, options);
}

int waitid(int idtype, int id, void *infop, int options) {
    return syscall4(195 | LEBIRUN_SYSCALL_FLAG, idtype, id, (int)infop, options);
}

int getdents(int fd, void *dirp, unsigned int count) {
    return syscall3(80 | LEBIRUN_SYSCALL_FLAG, fd, (int)dirp, count);
}

int getdents64(int fd, void *dirp, unsigned int count) {
    return syscall3(178 | LEBIRUN_SYSCALL_FLAG, fd, (int)dirp, count);
}

int sigreturn(void) {
    return syscall0(218 | LEBIRUN_SYSCALL_FLAG);
}

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

static inline int syscall5(int num, int arg1, int arg2, int arg3, int arg4, int arg5) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4), "D"(arg5)
        : "memory"
    );
    return ret;
}

static inline int syscall6(int num, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6) {
    int ret;
    __asm__ volatile (
        "push %%ebp\n\t"
        "mov %7, %%ebp\n\t"
        "int $0x80\n\t"
        "pop %%ebp"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4), "D"(arg5), "m"(arg6)
        : "memory"
    );
    return ret;
}

#define SYS_SOCKET (181 | LEBIRUN_SYSCALL_FLAG)
#define SYS_BIND (182 | LEBIRUN_SYSCALL_FLAG)
#define SYS_LISTEN (183 | LEBIRUN_SYSCALL_FLAG)
#define SYS_ACCEPT (184 | LEBIRUN_SYSCALL_FLAG)
#define SYS_CONNECT (185 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SEND (186 | LEBIRUN_SYSCALL_FLAG)
#define SYS_RECV (187 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SENDTO (188 | LEBIRUN_SYSCALL_FLAG)
#define SYS_RECVFROM (189 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SHUTDOWN (190 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SETSOCKOPT (191 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETSOCKOPT (192 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETPEERNAME (193 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GETSOCKNAME (194 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SOCKETPAIR (195 | LEBIRUN_SYSCALL_FLAG)

#define SYS_EPOLL_CREATE (88 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EPOLL_CTL (89 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EPOLL_WAIT (90 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EPOLL_CREATE1 (91 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EPOLL_PWAIT (92 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SELECT (93 | LEBIRUN_SYSCALL_FLAG)
#define SYS_POLL (94 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PPOLL (95 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PSELECT6 (96 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EVENTFD (97 | LEBIRUN_SYSCALL_FLAG)
#define SYS_EVENTFD2 (98 | LEBIRUN_SYSCALL_FLAG)

#define SYS_FUTEX (99 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SET_ROBUST_LIST (100 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GET_ROBUST_LIST (101 | LEBIRUN_SYSCALL_FLAG)

#define SYS_CLONE (102 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SET_TID_ADDRESS (103 | LEBIRUN_SYSCALL_FLAG)

#define SYS_POSIX_OPENPT (208 | LEBIRUN_SYSCALL_FLAG)
#define SYS_GRANTPT (209 | LEBIRUN_SYSCALL_FLAG)
#define SYS_UNLOCKPT (210 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTSNAME (211 | LEBIRUN_SYSCALL_FLAG)

struct sockaddr;
typedef unsigned int socklen_t;

int socket(int domain, int type, int protocol) {
    return syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return syscall3(SYS_BIND, sockfd, (int)addr, addrlen);
}

int listen(int sockfd, int backlog) {
    return syscall2(SYS_LISTEN, sockfd, backlog);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return syscall3(SYS_ACCEPT, sockfd, (int)addr, (int)addrlen);
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    return syscall4(SYS_ACCEPT, sockfd, (int)addr, (int)addrlen, flags);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return syscall3(SYS_CONNECT, sockfd, (int)addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return syscall4(SYS_SEND, sockfd, (int)buf, (int)len, flags);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return syscall4(SYS_RECV, sockfd, (int)buf, (int)len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return syscall6(SYS_SENDTO, sockfd, (int)buf, (int)len, flags, (int)dest_addr, addrlen);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return syscall6(SYS_RECVFROM, sockfd, (int)buf, (int)len, flags, (int)src_addr, (int)addrlen);
}

int shutdown(int sockfd, int how) {
    return syscall2(SYS_SHUTDOWN, sockfd, how);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    return syscall5(SYS_SETSOCKOPT, sockfd, level, optname, (int)optval, optlen);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    return syscall5(SYS_GETSOCKOPT, sockfd, level, optname, (int)optval, (int)optlen);
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return syscall3(SYS_GETPEERNAME, sockfd, (int)addr, (int)addrlen);
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return syscall3(SYS_GETSOCKNAME, sockfd, (int)addr, (int)addrlen);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    return syscall4(SYS_SOCKETPAIR, domain, type, protocol, (int)sv);
}

struct epoll_event;

int epoll_create(int size) {
    return syscall1(SYS_EPOLL_CREATE, size);
}

int epoll_create1(int flags) {
    return syscall1(SYS_EPOLL_CREATE1, flags);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    return syscall4(SYS_EPOLL_CTL, epfd, op, fd, (int)event);
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    return syscall4(SYS_EPOLL_WAIT, epfd, (int)events, maxevents, timeout);
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask) {
    return syscall5(SYS_EPOLL_PWAIT, epfd, (int)events, maxevents, timeout, (int)sigmask);
}

typedef struct {
    unsigned long fds_bits[32];
} fd_set_internal;

int select(int nfds, void *readfds, void *writefds, void *exceptfds, struct timeval *timeout) {
    return syscall5(SYS_SELECT, nfds, (int)readfds, (int)writefds, (int)exceptfds, (int)timeout);
}

int pselect(int nfds, void *readfds, void *writefds, void *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask) {
    return syscall6(SYS_PSELECT6, nfds, (int)readfds, (int)writefds, (int)exceptfds, (int)timeout, (int)sigmask);
}

struct pollfd;

int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    return syscall3(SYS_POLL, (int)fds, (int)nfds, timeout);
}

int ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *tmo_p, const sigset_t *sigmask) {
    return syscall4(SYS_PPOLL, (int)fds, (int)nfds, (int)tmo_p, (int)sigmask);
}

int eventfd(unsigned int initval, int flags) {
    if (flags) {
        return syscall2(SYS_EVENTFD2, initval, flags);
    }
    return syscall1(SYS_EVENTFD, initval);
}

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout, int *uaddr2, int val3) {
    return syscall6(SYS_FUTEX, (int)uaddr, futex_op, val, (int)timeout, (int)uaddr2, val3);
}

int set_robust_list(void *head, size_t len) {
    return syscall2(SYS_SET_ROBUST_LIST, (int)head, (int)len);
}

int get_robust_list(int pid, void **head_ptr, size_t *len_ptr) {
    return syscall3(SYS_GET_ROBUST_LIST, pid, (int)head_ptr, (int)len_ptr);
}

int clone(int (*fn)(void *), void *stack, int flags, void *arg) {
    return syscall4(SYS_CLONE, (int)fn, (int)stack, flags, (int)arg);
}

int set_tid_address(int *tidptr) {
    return syscall1(SYS_SET_TID_ADDRESS, (int)tidptr);
}

int posix_openpt(int flags) {
    return syscall1(SYS_POSIX_OPENPT, flags);
}

int grantpt(int fd) {
    return syscall1(SYS_GRANTPT, fd);
}

int unlockpt(int fd) {
    return syscall1(SYS_UNLOCKPT, fd);
}

char *ptsname(int fd) {
    static char pts_name[32];
    int ret = syscall2(SYS_PTSNAME, fd, (int)pts_name);
    if (ret < 0) return (void*)0;
    return pts_name;
}

int ptsname_r(int fd, char *buf, size_t buflen) {
    if (!buf || buflen < 16) return EINVAL;
    int ret = syscall2(SYS_PTSNAME, fd, (int)buf);
    return ret < 0 ? ENOTTY : 0;
}

int openpty(int *amaster, int *aslave, char *name, void *termp, void *winp) {
    (void)termp; (void)winp;
    int master = posix_openpt(2);
    if (master < 0) return -1;
    if (grantpt(master) < 0) { close(master); return -1; }
    if (unlockpt(master) < 0) { close(master); return -1; }
    char *sname = ptsname(master);
    if (!sname) { close(master); return -1; }
    int slave = open(sname, 2);
    if (slave < 0) { close(master); return -1; }
    *amaster = master;
    *aslave = slave;
    if (name) {
        int i = 0;
        while (sname[i]) { name[i] = sname[i]; i++; }
        name[i] = '\0';
    }
    return 0;
}

int forkpty(int *amaster, char *name, void *termp, void *winp) {
    int master, slave;
    if (openpty(&master, &slave, name, termp, winp) < 0) return -1;
    int pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }
    if (pid == 0) {
        close(master);
        setsid();
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        return 0;
    }
    close(slave);
    *amaster = master;
    return pid;
}

int tcsendbreak(int fd, int duration) {
    (void)fd; (void)duration;
    return 0;
}

int cfgetispeed(const void *termios_p) {
    if (!termios_p) return 0;
    return 38400;
}

int cfgetospeed(const void *termios_p) {
    if (!termios_p) return 0;
    return 38400;
}

int cfsetispeed(void *termios_p, int speed) {
    (void)termios_p; (void)speed;
    return 0;
}

int cfsetospeed(void *termios_p, int speed) {
    (void)termios_p; (void)speed;
    return 0;
}

void cfmakeraw(void *termios_p) {
    if (!termios_p) return;
}

#define SYS_PTHREAD_CREATE (223 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_EXIT (224 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_JOIN (225 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_DETACH (226 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_SELF (227 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_MUTEX_INIT (228 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_MUTEX_DESTROY (229 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_MUTEX_LOCK (230 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_MUTEX_TRYLOCK (231 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_MUTEX_UNLOCK (232 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_COND_INIT (233 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_COND_DESTROY (234 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_COND_WAIT (235 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_COND_SIGNAL (236 | LEBIRUN_SYSCALL_FLAG)
#define SYS_PTHREAD_COND_BROADCAST (237 | LEBIRUN_SYSCALL_FLAG)

#define SYS_SHMGET (238 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SHMAT (239 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SHMDT (240 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SHMCTL (241 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SHM_OPEN (242 | LEBIRUN_SYSCALL_FLAG)
#define SYS_SHM_UNLINK (243 | LEBIRUN_SYSCALL_FLAG)

#define SYS_DLOPEN (244 | LEBIRUN_SYSCALL_FLAG)
#define SYS_DLSYM (245 | LEBIRUN_SYSCALL_FLAG)
#define SYS_DLCLOSE (246 | LEBIRUN_SYSCALL_FLAG)
#define SYS_DLERROR (247 | LEBIRUN_SYSCALL_FLAG)

typedef unsigned long pthread_t;
typedef struct { int __data; } pthread_attr_t;
typedef struct { int __data; } pthread_mutex_t;
typedef struct { int __data; } pthread_mutexattr_t;
typedef struct { int __data; } pthread_cond_t;
typedef struct { int __data; } pthread_condattr_t;

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;
    return syscall3(SYS_PTHREAD_CREATE, (int)thread, (int)start_routine, (int)arg);
}

void pthread_exit(void *retval) {
    syscall1(SYS_PTHREAD_EXIT, (int)retval);
    __builtin_unreachable();
}

int pthread_join(pthread_t thread, void **retval) {
    return syscall2(SYS_PTHREAD_JOIN, (int)thread, (int)retval);
}

int pthread_detach(pthread_t thread) {
    return syscall1(SYS_PTHREAD_DETACH, (int)thread);
}

pthread_t pthread_self(void) {
    return (pthread_t)syscall0(SYS_PTHREAD_SELF);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    return syscall1(SYS_PTHREAD_MUTEX_INIT, (int)mutex);
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    return syscall1(SYS_PTHREAD_MUTEX_DESTROY, (int)mutex);
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return syscall1(SYS_PTHREAD_MUTEX_LOCK, (int)mutex);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    return syscall1(SYS_PTHREAD_MUTEX_TRYLOCK, (int)mutex);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    return syscall1(SYS_PTHREAD_MUTEX_UNLOCK, (int)mutex);
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    return syscall1(SYS_PTHREAD_COND_INIT, (int)cond);
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    return syscall1(SYS_PTHREAD_COND_DESTROY, (int)cond);
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    return syscall2(SYS_PTHREAD_COND_WAIT, (int)cond, (int)mutex);
}

int pthread_cond_signal(pthread_cond_t *cond) {
    return syscall1(SYS_PTHREAD_COND_SIGNAL, (int)cond);
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    return syscall1(SYS_PTHREAD_COND_BROADCAST, (int)cond);
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

int pthread_attr_init(pthread_attr_t *attr) {
    if (attr) attr->__data = 0;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (attr) attr->__data = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
    if (detachstate) *detachstate = attr ? attr->__data : 0;
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (attr) attr->__data = 0;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    if (attr) attr->__data = 0;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define IPC_RMID   0
#define IPC_SET    1
#define IPC_STAT   2
#define IPC_PRIVATE 0

typedef int key_t;

int shmget(key_t key, size_t size, int shmflg) {
    return syscall3(SYS_SHMGET, key, (int)size, shmflg);
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
    return (void *)syscall3(SYS_SHMAT, shmid, (int)shmaddr, shmflg);
}

int shmdt(const void *shmaddr) {
    return syscall1(SYS_SHMDT, (int)shmaddr);
}

struct shmid_ds;
int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    return syscall3(SYS_SHMCTL, shmid, cmd, (int)buf);
}

int shm_open(const char *name, int oflag, mode_t mode) {
    return syscall3(SYS_SHM_OPEN, (int)name, oflag, mode);
}

int shm_unlink(const char *name) {
    return syscall1(SYS_SHM_UNLINK, (int)name);
}

void *dlopen(const char *filename, int flags) {
    return (void *)syscall2(SYS_DLOPEN, (int)filename, flags);
}

void *dlsym(void *handle, const char *symbol) {
    return (void *)syscall2(SYS_DLSYM, (int)handle, (int)symbol);
}

int dlclose(void *handle) {
    return syscall1(SYS_DLCLOSE, (int)handle);
}

char *dlerror(void) {
    static char err_buf[128];
    int ret = syscall2(SYS_DLERROR, (int)err_buf, 128);
    if (ret <= 0) return (void *)0;
    return err_buf;
}
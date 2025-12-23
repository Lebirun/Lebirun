#if !defined(__is_libk)

#include <unistd.h>
#include <syscall.h>
#include <sys/utsname.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

static int fd_table[256];
static int fd_table_init = 0;

static void init_fd_table(void) {
    if (fd_table_init) return;
    for (int i = 0; i < 256; i++) {
        fd_table[i] = -1;
    }
    fd_table[0] = 0;
    fd_table[1] = 1;
    fd_table[2] = 2;
    fd_table_init = 1;
}

off_t lseek(int fd, off_t offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    errno = ESPIPE;
    return -1;
}

int dup(int oldfd) {
    init_fd_table();
    if (oldfd < 0 || oldfd >= 256 || fd_table[oldfd] < 0) {
        errno = EBADF;
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        if (fd_table[i] < 0) {
            fd_table[i] = fd_table[oldfd];
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

int dup2(int oldfd, int newfd) {
    init_fd_table();
    if (oldfd < 0 || oldfd >= 256 || fd_table[oldfd] < 0) {
        errno = EBADF;
        return -1;
    }
    if (newfd < 0 || newfd >= 256) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd) return newfd;
    if (fd_table[newfd] >= 0) {
        close(newfd);
    }
    fd_table[newfd] = fd_table[oldfd];
    return newfd;
}

int pipe(int pipefd[2]) {
    (void)pipefd;
    errno = ENOSYS;
    return -1;
}

int getppid(void) {
    return 1;
}

int getpgrp(void) {
    return getpid();
}

int setpgrp(void) {
    return 0;
}

int getpgid(pid_t pid) {
    (void)pid;
    return getpid();
}

int setpgid(pid_t pid, pid_t pgid) {
    (void)pid; (void)pgid;
    return 0;
}

pid_t getsid(pid_t pid) {
    (void)pid;
    return getpid();
}

pid_t setsid(void) {
    return getpid();
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

int setuid(uid_t uid) {
    (void)uid;
    return 0;
}

int seteuid(uid_t euid) {
    (void)euid;
    return 0;
}

int setgid(gid_t gid) {
    (void)gid;
    return 0;
}

int setegid(gid_t egid) {
    (void)egid;
    return 0;
}

int setreuid(uid_t ruid, uid_t euid) {
    (void)ruid; (void)euid;
    return 0;
}

int setregid(gid_t rgid, gid_t egid) {
    (void)rgid; (void)egid;
    return 0;
}

int getgroups(int size, gid_t list[]) {
    if (size == 0) return 0;
    if (size > 0) list[0] = 0;
    return 0;
}

int setgroups(size_t size, const gid_t *list) {
    (void)size; (void)list;
    return 0;
}

char *getlogin(void) {
    return "root";
}

int getlogin_r(char *buf, size_t bufsize) {
    if (bufsize < 5) {
        errno = ERANGE;
        return ERANGE;
    }
    buf[0] = 'r'; buf[1] = 'o'; buf[2] = 'o'; buf[3] = 't'; buf[4] = '\0';
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    sleep_ms(seconds * 1000);
    return 0;
}

int usleep(useconds_t usec) {
    sleep_ms((usec + 999) / 1000);
    return 0;
}

int pause(void) {
    errno = EINTR;
    return -1;
}

int munmap(void *addr, size_t len) {
    (void)addr; (void)len;
    return 0;
}

char *ttyname(int fd) {
    static char buf[32];
    if (!isatty(fd)) return NULL;
    buf[0] = '/'; buf[1] = 'd'; buf[2] = 'e'; buf[3] = 'v';
    buf[4] = '/'; buf[5] = 't'; buf[6] = 't'; buf[7] = 'y';
    buf[8] = '0' + fd; buf[9] = '\0';
    return buf;
}

int ttyname_r(int fd, char *buf, size_t buflen) {
    if (!isatty(fd)) {
        errno = ENOTTY;
        return ENOTTY;
    }
    if (buflen < 10) {
        errno = ERANGE;
        return ERANGE;
    }
    char *name = ttyname(fd);
    size_t i = 0;
    while (name[i] && i < buflen - 1) {
        buf[i] = name[i];
        i++;
    }
    buf[i] = '\0';
    return 0;
}

int execl(const char *path, const char *arg, ...) {
    (void)path; (void)arg;
    errno = ENOSYS;
    return -1;
}

int execv(const char *path, char *const argv[]) {
    (void)path; (void)argv;
    errno = ENOSYS;
    return -1;
}

int execle(const char *path, const char *arg, ...) {
    (void)path; (void)arg;
    errno = ENOSYS;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    (void)path; (void)argv; (void)envp;
    errno = ENOSYS;
    return -1;
}

int execlp(const char *file, const char *arg, ...) {
    (void)file; (void)arg;
    errno = ENOSYS;
    return -1;
}

int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    errno = ENOSYS;
    return -1;
}

int access(const char *pathname, int mode) {
    (void)pathname; (void)mode;
    return 0;
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    (void)dirfd; (void)pathname; (void)mode; (void)flags;
    return 0;
}

int rmdir(const char *pathname) {
    return vfs_unlink(pathname);
}

int unlink(const char *pathname) {
    return vfs_unlink(pathname);
}

int chdir(const char *path) {
    (void)path;
    return 0;
}

int fchdir(int fd) {
    (void)fd;
    return 0;
}

static char cwd_buf[256] = "/";

char *getcwd(char *buf, size_t size) {
    if (!buf) {
        errno = EINVAL;
        return NULL;
    }
    size_t len = 0;
    while (cwd_buf[len]) len++;
    if (len + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    for (size_t i = 0; i <= len; i++) buf[i] = cwd_buf[i];
    return buf;
}

char *getwd(char *buf) {
    if (!buf) return NULL;
    return getcwd(buf, PATH_MAX);
}

int link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    errno = ENOSYS;
    return -1;
}

int symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    (void)pathname; (void)buf; (void)bufsiz;
    errno = EINVAL;
    return -1;
}

int chown(const char *pathname, uid_t owner, gid_t group) {
    (void)pathname; (void)owner; (void)group;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd; (void)owner; (void)group;
    return 0;
}

int lchown(const char *pathname, uid_t owner, gid_t group) {
    (void)pathname; (void)owner; (void)group;
    return 0;
}

int truncate(const char *path, off_t length) {
    (void)path; (void)length;
    errno = ENOSYS;
    return -1;
}

int ftruncate(int fd, off_t length) {
    (void)fd; (void)length;
    errno = ENOSYS;
    return -1;
}

int fsync(int fd) {
    (void)fd;
    return 0;
}

int fdatasync(int fd) {
    (void)fd;
    return 0;
}

void sync(void) {
}

long sysconf(int name) {
    switch (name) {
        case _SC_CLK_TCK: return 100;
        case _SC_PAGESIZE: return 4096;
        case _SC_OPEN_MAX: return 256;
        case _SC_NPROCESSORS_ONLN: return 1;
        case _SC_NPROCESSORS_CONF: return 1;
        case _SC_LINE_MAX: return 2048;
        case _SC_ARG_MAX: return 131072;
        case _SC_CHILD_MAX: return 256;
        case _SC_HOST_NAME_MAX: return 64;
        default: errno = EINVAL; return -1;
    }
}

long pathconf(const char *path, int name) {
    (void)path; (void)name;
    errno = ENOSYS;
    return -1;
}

long fpathconf(int fd, int name) {
    (void)fd; (void)name;
    errno = ENOSYS;
    return -1;
}

size_t confstr(int name, char *buf, size_t len) {
    (void)name; (void)buf; (void)len;
    return 0;
}

int gethostname(char *name, size_t len) {
    const char *hostname = "localhost";
    size_t i = 0;
    while (hostname[i] && i < len - 1) {
        name[i] = hostname[i];
        i++;
    }
    name[i] = '\0';
    return 0;
}

int sethostname(const char *name, size_t len) {
    (void)name; (void)len;
    errno = EPERM;
    return -1;
}

int nice(int inc) {
    (void)inc;
    return 0;
}

int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}

int uname(struct utsname *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    strcpy(buf->sysname, "LEB");
    strcpy(buf->nodename, "localhost");
    strcpy(buf->release, "0.1.0");
    strcpy(buf->version, "0.1.0");
    strcpy(buf->machine, "i386");
    return 0;
}

#endif

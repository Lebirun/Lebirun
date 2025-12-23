#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _SC_CLK_TCK           2
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         30
#define _SC_OPEN_MAX          4
#define _SC_NPROCESSORS_ONLN  84
#define _SC_NPROCESSORS_CONF  83
#define _SC_LINE_MAX          43
#define _SC_ARG_MAX           0
#define _SC_CHILD_MAX         1
#define _SC_HOST_NAME_MAX     180

struct termios;
struct winsize;

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((__noreturn__))
void _exit(int status);

ssize_t read(int fd, void* buf, size_t count);
int read_nb(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);

off_t lseek(int fd, off_t offset, int whence);

int dup(int oldfd);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);

int getpid(void);
int getppid(void);
int getpgrp(void);
int setpgrp(void);
int getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t getsid(pid_t pid);
pid_t setsid(void);

uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int seteuid(uid_t euid);
int setgid(gid_t gid);
int setegid(gid_t egid);
int setreuid(uid_t ruid, uid_t euid);
int setregid(gid_t rgid, gid_t egid);

int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t *list);

char *getlogin(void);
int getlogin_r(char *buf, size_t bufsize);

int yield(void);
int sleep_ms(int ms);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
int pause(void);

int waitpid(int pid, int *status, int options);
void *sbrk(intptr_t inc);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
int kill(int pid, int sig);
unsigned int getticks(void);
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t buflen);

int fork(void);
int exec(const void *bin, unsigned int size);
int execl(const char *path, const char *arg, ...);
int execv(const char *path, char *const argv[]);
int execle(const char *path, const char *arg, ...);
int execve(const char *path, char *const argv[], char *const envp[]);
int execlp(const char *file, const char *arg, ...);
int execvp(const char *file, char *const argv[]);

int initrd_count(void);
int initrd_stat(int index, char *name, unsigned int *length);
int initrd_read(int index, void *buf, unsigned int maxlen);

int open(const char *path, int flags, ...);
int close(int fd);
int fstat_simple(int fd, unsigned int *size, unsigned char *type);
int access(const char *pathname, int mode);
int faccessat(int dirfd, const char *pathname, int mode, int flags);

int unlink(const char *pathname);
int rmdir(const char *pathname);
int chdir(const char *path);
int fchdir(int fd);
char *getcwd(char *buf, size_t size);
char *getwd(char *buf);
int link(const char *oldpath, const char *newpath);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);

int chown(const char *pathname, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int lchown(const char *pathname, uid_t owner, gid_t group);

int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);

int fsync(int fd);
int fdatasync(int fd);
void sync(void);

long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);

#define _PC_PIPE_BUF  1

size_t confstr(int name, char *buf, size_t len);

int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);

int nice(int inc);

int alarm(unsigned int seconds);

int fb_putpixel(int x, int y, unsigned int color);
int fb_setcolors(unsigned int fg, unsigned int bg);
int fb_getinfo(unsigned int *width, unsigned int *height, unsigned int *bpp, unsigned int *font_height, unsigned int *rows, unsigned int *cursor_row);
int fb_clear(void);

int console_switch(int console_num);
int console_getcur(void);
int console_clear(int console_num);
int console_setcursor(int x, int y);

int vfs_open(const char *path, int flags);
int vfs_close_fd(int fd);
int vfs_read_fd(int fd, void *buf, unsigned int count);
int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index);
int vfs_stat(int fd, unsigned int *size, unsigned int *type);
int vfs_mounts(void);
int vfs_write_fd(int fd, const void *buf, unsigned int count);
int vfs_create(const char *path, unsigned int perms);
int vfs_mkdir(const char *path, unsigned int perms);
int vfs_unlink(const char *path);

extern char **environ;

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char * const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif

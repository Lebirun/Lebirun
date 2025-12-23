#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG   1
#define WUNTRACED 2

#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WSTOPSIG(status)    WEXITSTATUS(status)
#define WIFEXITED(status)   (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) (((signed char)(((status) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(status)  (((status) & 0xff) == 0x7f)
#define WIFCONTINUED(status) ((status) == 0xffff)
#define WCOREDUMP(status)   ((status) & 0x80)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
int waitid(int idtype, id_t id, siginfo_t *infop, int options);

#ifdef __cplusplus
}
#endif

#endif

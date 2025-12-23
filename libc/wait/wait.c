#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

int waitid(int idtype, id_t id, siginfo_t *infop, int options) {
    (void)idtype;
    (void)id;
    (void)infop;
    (void)options;
    errno = ENOSYS;
    return -1;
}

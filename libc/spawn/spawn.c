#include <spawn.h>
#include <errno.h>
#include <string.h>

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
    (void)pid;
    (void)path;
    (void)file_actions;
    (void)attrp;
    (void)argv;
    (void)envp;
    errno = ENOSYS;
    return -1;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    (void)pid;
    (void)file;
    (void)file_actions;
    (void)attrp;
    (void)argv;
    (void)envp;
    errno = ENOSYS;
    return -1;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *file_actions) {
    if (!file_actions) return EINVAL;
    memset(file_actions, 0, sizeof(*file_actions));
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *file_actions) {
    (void)file_actions;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *file_actions, int fd) {
    (void)file_actions;
    (void)fd;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *file_actions, int fd,
                                     const char *path, int oflag, mode_t mode) {
    (void)file_actions;
    (void)fd;
    (void)path;
    (void)oflag;
    (void)mode;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *file_actions,
                                     int fd, int newfd) {
    (void)file_actions;
    (void)fd;
    (void)newfd;
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *attr) {
    if (!attr) return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr) {
    (void)attr;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags) {
    if (!attr || !flags) return EINVAL;
    *flags = attr->__flags;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags) {
    if (!attr) return EINVAL;
    attr->__flags = flags;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup) {
    if (!attr || !pgroup) return EINVAL;
    *pgroup = attr->__pgrp;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup) {
    if (!attr) return EINVAL;
    attr->__pgrp = pgroup;
    return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *sigdefault) {
    if (!attr || !sigdefault) return EINVAL;
    *sigdefault = attr->__sd;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sigdefault) {
    if (!attr || !sigdefault) return EINVAL;
    attr->__sd = *sigdefault;
    return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sigmask) {
    if (!attr || !sigmask) return EINVAL;
    *sigmask = attr->__ss;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask) {
    if (!attr || !sigmask) return EINVAL;
    attr->__ss = *sigmask;
    return 0;
}

int posix_spawnattr_getschedparam(const posix_spawnattr_t *attr, struct sched_param *schedparam) {
    if (!attr || !schedparam) return EINVAL;
    *schedparam = attr->__sp;
    return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *attr, const struct sched_param *schedparam) {
    if (!attr || !schedparam) return EINVAL;
    attr->__sp = *schedparam;
    return 0;
}

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *schedpolicy) {
    if (!attr || !schedpolicy) return EINVAL;
    *schedpolicy = attr->__policy;
    return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int schedpolicy) {
    if (!attr) return EINVAL;
    attr->__policy = schedpolicy;
    return 0;
}

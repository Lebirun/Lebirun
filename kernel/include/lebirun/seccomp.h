#ifndef LEBIRUN_SECCOMP_BPF_H
#define LEBIRUN_SECCOMP_BPF_H
#include <stdint.h>
struct task;
int seccomp_install(struct task *task, const void *uprog);
int64_t seccomp_check(struct task *task, int nr, uint64_t ip,
                          uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);
int seccomp_fork(struct task *parent, struct task *child);
void seccomp_release_task(struct task *task);
#endif

#ifndef KERNEL_CREDS_H
#define KERNEL_CREDS_H

#include <lebirun/task.h>

void creds_init_task(struct task *task);
int creds_copy_task(struct task *parent, struct task *child);
void creds_apply_exec_ids(struct task *task, uint64_t euid, uint64_t egid);
void creds_release_task(struct task *task);
int creds_syscall_allowed(struct task *task, int syscall_number);
int creds_set_syscall_mask(struct task *task, const uint64_t *mask,
                           size_t word_count);
int creds_set_strict_syscalls(struct task *task);
int creds_set_no_new_privs(struct task *task);
int creds_get_no_new_privs(struct task *task);
int creds_get_syscall_filter_mode(struct task *task);
int creds_set_dumpable(struct task *task, int dumpable);
int creds_get_dumpable(struct task *task);
int creds_has_capability(struct task *task, int capability);

pid_t creds_get_pgid(pid_t pid);
pid_t creds_get_sid(pid_t pid);

#endif

#ifndef KERNEL_CREDS_H
#define KERNEL_CREDS_H

#include <kernel/task.h>

void creds_init_task(struct task *task);
void creds_copy_task(struct task *parent, struct task *child);

pid_t creds_get_pgid(pid_t pid);
pid_t creds_get_sid(pid_t pid);

#endif

#ifndef KERNEL_CREDS_H
#define KERNEL_CREDS_H

#include <kernel/task.h>

void creds_init_task(pid_t pid);
void creds_copy_task(pid_t parent_pid, pid_t child_pid);

pid_t creds_get_pgid(pid_t pid);
pid_t creds_get_sid(pid_t pid);

#endif

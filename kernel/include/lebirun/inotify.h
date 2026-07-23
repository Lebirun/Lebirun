#ifndef LEBIRUN_INOTIFY_H
#define LEBIRUN_INOTIFY_H

#include <stdint.h>

struct vfs_node;

int inotify_is_fd(int fd);
int inotify_read_fd(int fd, void *buffer, int length);
int inotify_close_fd(int fd);
int inotify_poll_fd(int fd);
void inotify_close_task(pid_t pid);
void inotify_notify(struct vfs_node *node, uint32_t mask, const char *name);
void syscalls_inotify_init(void);

#endif

#ifndef LEBIRUN_INOTIFY_H
#define LEBIRUN_INOTIFY_H

#include <stdint.h>

struct vfs_node;

int inotify_is_fd(int fd);
int inotify_read_fd(int fd, void *buffer, int length);
int inotify_close_fd(int fd);
void inotify_close_range(unsigned int first, unsigned int last, int cloexec);
void inotify_close_cloexec(pid_t pid);
int inotify_poll_fd(int fd);
void inotify_close_task(pid_t pid);
void inotify_notify(struct vfs_node *node, uint32_t mask, const char *name);
void inotify_invalidate_node(struct vfs_node *node);
void inotify_invalidate_mount(void *mount);
uint64_t inotify_get_max_instances(void);
uint64_t inotify_get_max_watches(void);
uint64_t inotify_get_max_queued(void);
int inotify_set_max_instances(uint64_t v);
int inotify_set_max_watches(uint64_t v);
int inotify_set_max_queued(uint64_t v);
void syscalls_inotify_init(void);

#endif

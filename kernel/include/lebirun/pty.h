#ifndef _LEBIRUN_PTY_H
#define _LEBIRUN_PTY_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/tty.h>

typedef long ssize_t;

int pty_open_master(void);
int pty_open_slave(int master_fd);
int pty_grant(int master_fd);
int pty_unlock(int master_fd);
char *pty_name(int master_fd);

ssize_t pty_master_read(int fd, void *buf, size_t count);
ssize_t pty_master_write(int fd, const void *buf, size_t count);
ssize_t pty_slave_read(int fd, void *buf, size_t count);
ssize_t pty_slave_write(int fd, const void *buf, size_t count);
int pty_ioctl(int fd, unsigned long request, void *arg);

int pty_close_master(int fd);
int pty_close_slave(int fd);

int is_pty_master(int fd);
int is_pty_slave(int fd);
int pty_has_data_for_master(int fd);
int pty_has_data_for_slave(int fd);

void pty_init(void);

#endif

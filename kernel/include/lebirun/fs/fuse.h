#ifndef LEBIRUN_FUSE_H
#define LEBIRUN_FUSE_H

#include <lebirun/vfs.h>

void fuse_vfs_register(void);
vfs_node_t *fuse_dev_node(void);

#endif

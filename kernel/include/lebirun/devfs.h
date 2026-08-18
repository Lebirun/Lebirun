#ifndef LEBIRUN_DEVFS_H
#define LEBIRUN_DEVFS_H

#include <stdint.h>
#include <lebirun/vfs.h>

void devfs_init(void);
int devfs_register_blockdev(const char *name, uint64_t port_index);
int devfs_register_cdrom(const char *name, uint64_t port_index);
int devfs_register_partition(const char *name, uint64_t port_index,
                             uint64_t start_lba, uint64_t sector_count);
uint64_t devfs_get_partition_start(vfs_node_t *node);
int devfs_is_partition(vfs_node_t *node);
int devfs_rescan_partitions(const char *devname);
void devfs_register_initrd(void);

#endif

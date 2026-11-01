#ifndef LEBIRUN_BLKDEV_H
#define LEBIRUN_BLKDEV_H

#include <stdint.h>
#include <lebirun/common.h>

#define BLKDEV_SECTOR_SIZE 512

#if CONFIG_DRIVER_USB_HOTPLUG && CONFIG_DRIVER_USB_STORAGE
#define BLKDEV_HOTPLUG
#define BLKDEV_LOG(...) printf(__VA_ARGS__)
#else
#define BLKDEV_HOTPLUG KERNEL_INIT
#define BLKDEV_LOG KERNEL_INIT_LOG
#endif

enum {
    BLKDEV_DISK,
    BLKDEV_CDROM,
};

typedef struct blkdev blkdev_t;

typedef struct {
    int (*read)(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf);
    int (*write)(blkdev_t *dev, uint64_t lba, uint64_t count, const void *buf);
    int (*writev)(blkdev_t *dev, uint64_t lba, uint64_t sectors,
                  const void *const *bufs, uint32_t count, uint64_t bytes);
    int (*flush)(blkdev_t *dev);
} blkdev_ops_t;

struct blkdev {
    const blkdev_ops_t *ops;
    void *priv;
    uint64_t sectors;
    uint32_t block_size;
    uint8_t type;
    char name[16];
};

int blkdev_register(blkdev_t *dev);
void blkdev_unregister(uint64_t index);
blkdev_t *blkdev_get(uint64_t index);
uint64_t blkdev_count(void);
uint64_t blkdev_phys(const void *addr);
int blkdev_read(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf);
int blkdev_write(blkdev_t *dev, uint64_t lba, uint64_t count, const void *buf);
int blkdev_writev(blkdev_t *dev, uint64_t lba, uint64_t sectors,
                  const void *const *bufs, uint32_t count, uint64_t bytes);
int blkdev_flush(blkdev_t *dev);

#endif

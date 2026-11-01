#include <lebirun/blkdev.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

static blkdev_t **blkdevs;
static uint64_t blkdev_total;
static uint64_t blkdev_capacity;

static int BLKDEV_HOTPLUG blkdev_name_used(const char *name) {
    uint64_t i;

    for (i = 0; i < blkdev_total; i++) {
        if (blkdevs[i] && !strcmp(blkdevs[i]->name, name)) return 1;
    }
    return 0;
}

int BLKDEV_HOTPLUG blkdev_register(blkdev_t *dev) {
    blkdev_t **list;
    uint32_t n;

    if (!dev || !dev->ops || dev->block_size < BLKDEV_SECTOR_SIZE ||
        (dev->block_size & (dev->block_size - 1)))
        return -1;
    if (blkdev_total == blkdev_capacity) {
        n = blkdev_capacity ? (uint32_t)blkdev_capacity * 2 : 4;
        list = (blkdev_t **)kcalloc(n, sizeof(*list));
        if (!list) return -1;
        if (blkdev_total) memcpy(list, blkdevs, blkdev_total * sizeof(*list));
        list = __atomic_exchange_n(&blkdevs, list, __ATOMIC_RELEASE);
        if (!CONFIG_DRIVER_USB_HOTPLUG) kfree(list);
        blkdev_capacity = n;
    }
    for (n = 0; !dev->name[0] || blkdev_name_used(dev->name); n++) {
        if (dev->type == BLKDEV_CDROM)
            snprintf(dev->name, sizeof(dev->name), "sr%u", n);
        else if (n < 26)
            snprintf(dev->name, sizeof(dev->name), "sd%c", 'a' + n);
        else
            snprintf(dev->name, sizeof(dev->name), "sd%c%c",
                     'a' + n / 26 - 1, 'a' + n % 26);
    }
    blkdevs[blkdev_total] = dev;
    __atomic_store_n(&blkdev_total, blkdev_total + 1, __ATOMIC_RELEASE);
    return (int)blkdev_total - 1;
}

void blkdev_unregister(uint64_t index) {
    if (index < blkdev_total)
        __atomic_store_n(&blkdevs[index], NULL, __ATOMIC_RELEASE);
}

blkdev_t *blkdev_get(uint64_t index) {
    if (index >= __atomic_load_n(&blkdev_total, __ATOMIC_ACQUIRE)) return NULL;
    return __atomic_load_n(&__atomic_load_n(&blkdevs, __ATOMIC_ACQUIRE)[index],
                           __ATOMIC_ACQUIRE);
}

uint64_t blkdev_count(void) {
    return blkdev_total;
}

uint64_t blkdev_phys(const void *addr) {
    uint64_t va = (uint64_t)(uintptr_t)addr;
    uint64_t pml4 = vmm_get_kernel_cr3();
    uint64_t phys;

    if (va < KERNEL_VMA || !pml4) return 0;
    phys = vmm_get_phys_in_pml4(pml4, va & ~(PAGE_SIZE - 1));
    if (!phys && demand_commit_page(va) == 0)
        phys = vmm_get_phys_in_pml4(pml4, va & ~(PAGE_SIZE - 1));
    return phys ? phys + (va & (PAGE_SIZE - 1)) : 0;
}

static int blkdev_direct(const void *buf) {
    uint64_t va = (uint64_t)(uintptr_t)buf;

    return va >= KERNEL_VMA && !(va & 3);
}

static int blkdev_io(blkdev_t *dev, uint64_t lba, uint64_t count,
                     void *buf, int wr) {
    uint64_t f;
    uint64_t first;
    uint64_t n;
    uint64_t off;
    uint8_t *tmp;
    int r;

    if (!dev || !buf || !count || count > UINT64_MAX / BLKDEV_SECTOR_SIZE)
        return -1;
    if (wr ? !dev->ops->write : !dev->ops->read) return -1;
    if (dev->sectors && (lba >= dev->sectors || count > dev->sectors - lba))
        return -1;
    f = dev->block_size / BLKDEV_SECTOR_SIZE;
    if (!(lba % f) && !(count % f) && blkdev_direct(buf))
        return wr ? dev->ops->write(dev, lba / f, count / f, buf) :
                    dev->ops->read(dev, lba / f, count / f, buf);
    first = lba / f;
    n = (lba + count + f - 1) / f - first;
    if (n > UINT64_MAX / dev->block_size) return -1;
    tmp = (uint8_t *)kmalloc(n * dev->block_size);
    if (!tmp) return -1;
    off = (lba % f) * BLKDEV_SECTOR_SIZE;
    r = 0;
    if (!wr) {
        r = dev->ops->read(dev, first, n, tmp);
        if (!r) memcpy(buf, tmp + off, count * BLKDEV_SECTOR_SIZE);
        kfree(tmp);
        return r;
    }
    if (off)
        r = dev->ops->read(dev, first, 1, tmp);
    if (!r && ((lba + count) % f) && (n > 1 || !off))
        r = dev->ops->read(dev, first + n - 1, 1,
                           tmp + (n - 1) * dev->block_size);
    if (!r) {
        memcpy(tmp + off, buf, count * BLKDEV_SECTOR_SIZE);
        r = dev->ops->write(dev, first, n, tmp);
    }
    kfree(tmp);
    return r;
}

int blkdev_read(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf) {
    return blkdev_io(dev, lba, count, buf, 0);
}

int blkdev_write(blkdev_t *dev, uint64_t lba, uint64_t count,
                 const void *buf) {
    return blkdev_io(dev, lba, count, (void *)buf, 1);
}

int blkdev_writev(blkdev_t *dev, uint64_t lba, uint64_t sectors,
                  const void *const *bufs, uint32_t count, uint64_t bytes) {
    uint64_t per;
    uint32_t i;

    if (!dev || !bufs || !count || !bytes || bytes % BLKDEV_SECTOR_SIZE ||
        count > UINT64_MAX / bytes ||
        (uint64_t)count * bytes != sectors * BLKDEV_SECTOR_SIZE)
        return -1;
    if (dev->ops->writev && dev->block_size == BLKDEV_SECTOR_SIZE)
        return dev->ops->writev(dev, lba, sectors, bufs, count, bytes);
    per = bytes / BLKDEV_SECTOR_SIZE;
    for (i = 0; i < count; i++) {
        if (blkdev_write(dev, lba + i * per, per, bufs[i]) != 0) return -1;
    }
    return 0;
}

int blkdev_flush(blkdev_t *dev) {
    if (!dev || !dev->ops->flush) return -1;
    return dev->ops->flush(dev);
}

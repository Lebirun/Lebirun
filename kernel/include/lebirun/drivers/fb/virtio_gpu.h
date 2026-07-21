#ifndef _LEBIRUN_VIRTIO_GPU_H
#define _LEBIRUN_VIRTIO_GPU_H

#include <stdint.h>

#define VIRTIO_GPU_PCI_BOOT_WIDTH 1024
#define VIRTIO_GPU_PCI_BOOT_HEIGHT 768

int virtio_gpu_init(uint64_t preferred_width, uint64_t preferred_height);
int virtio_gpu_is_available(void);
int virtio_gpu_set_mode(uint64_t width, uint64_t height, uint64_t *framebuffer_phys);
void virtio_gpu_mark_dirty(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
void virtio_gpu_flush(void);

#endif

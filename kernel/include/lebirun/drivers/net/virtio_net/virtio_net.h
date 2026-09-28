#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <lebirun/drivers/net/net_types.h>

#define VIRTIO_NET_VENDOR_ID 0x1AF4
#define VIRTIO_NET_DEV_LEGACY 0x1000
#define VIRTIO_NET_DEV_MODERN 0x1041

int virtio_net_init(void);
int virtio_net_init_at(uint8_t bus, uint8_t slot, uint8_t func);
int virtio_net_send(netif_t *netif, uint8_t *data, uint64_t len);
int virtio_net_poll(netif_t *netif);
void virtio_net_irq_handler(void *regs);

#endif

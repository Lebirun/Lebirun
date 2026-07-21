#include <lebirun/drivers/fb/virtio_gpu.h>
#include <lebirun/framebuffer.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/io.h>
#include <lebirun/tty.h>
#include <lebirun/console.h>
#include <string.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_VENDOR_VIRTIO 0x1AF4
#define PCI_DEVICE_GPU_MODERN 0x1050
#define PCI_CLASS_DISPLAY 0x03
#define PCI_SUBCLASS_VGA 0x00
#define PCI_SUBCLASS_OTHER_DISPLAY 0x80
#define PCI_CAP_ID_VENDOR 0x09
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128
#define VIRTIO_F_VERSION_1 32
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_MMIO_BASE (KERNEL_VMA + 0x3D000000ULL)
#define VIRTIO_GPU_BAR_STRIDE 0x200000ULL
#define VIRTIO_GPU_TIMEOUT 100000000ULL

typedef struct __attribute__((packed)) {
    volatile uint32_t device_feature_select;
    volatile uint32_t device_feature;
    volatile uint32_t driver_feature_select;
    volatile uint32_t driver_feature;
    volatile uint16_t config_msix_vector;
    volatile uint16_t num_queues;
    volatile uint8_t device_status;
    volatile uint8_t config_generation;
    volatile uint16_t queue_select;
    volatile uint16_t queue_size;
    volatile uint16_t queue_msix_vector;
    volatile uint16_t queue_enable;
    volatile uint16_t queue_notify_off;
    volatile uint64_t queue_desc;
    volatile uint64_t queue_driver;
    volatile uint64_t queue_device;
} virtio_pci_common_cfg_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} virtio_gpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t pmodes[16];
    uint32_t enabled[16];
    uint32_t flags[16];
} virtio_gpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_resource_attach_backing_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_flush_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_id_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    virtio_pci_common_cfg_t *common;
    volatile uint8_t *notify;
    uint32_t notify_multiplier;
    uint16_t queue_size;
    uint16_t queue_notify_off;
    uint64_t queue_phys;
    uint64_t queue_pages;
    virtq_desc_t *desc;
    volatile uint16_t *avail_flags;
    volatile uint16_t *avail_idx;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_flags;
    volatile uint16_t *used_idx;
    volatile virtq_used_elem_t *used_ring;
    uint16_t last_used_idx;
    uint64_t command_phys;
    uint8_t *command;
    uint32_t command_capacity;
    uint64_t response_phys;
    uint8_t *response;
    uint32_t response_capacity;
    uint64_t framebuffer_phys;
    uint64_t framebuffer_pages;
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    volatile int command_lock;
    volatile int dirty_lock;
    uint32_t dirty_x1;
    uint32_t dirty_y1;
    uint32_t dirty_x2;
    uint32_t dirty_y2;
    int dirty_valid;
    int vga_compatible;
    int active;
} virtio_gpu_device_t;

static virtio_gpu_device_t gpu;

extern int pt_ensure_phys_mapped(uint64_t phys_addr);

static uint64_t gpu_lock_acquire(volatile int *lock) {
    uint64_t flags;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(lock, 1)) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    return flags;
}

static void gpu_lock_release(volatile int *lock, uint64_t flags) {
    __sync_lock_release(lock);
    if (flags & (1ULL << 9)) __asm__ __volatile__("sti" ::: "memory");
}

static int release_failed_device(void) {
    if (gpu.common) gpu.common->device_status = 0;
    if (gpu.framebuffer_phys && gpu.framebuffer_pages) {
        pfa_free_contiguous(gpu.framebuffer_phys, gpu.framebuffer_pages);
    }
    if (gpu.queue_phys && gpu.queue_pages) {
        pfa_free_contiguous(gpu.queue_phys, gpu.queue_pages);
    }
    gpu.framebuffer_phys = 0;
    gpu.framebuffer_pages = 0;
    gpu.queue_phys = 0;
    gpu.queue_pages = 0;
    gpu.command_phys = 0;
    gpu.command = NULL;
    gpu.command_capacity = 0;
    gpu.response_phys = 0;
    gpu.response = NULL;
    gpu.response_capacity = 0;
    gpu.active = 0;
    return -1;
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t address;
    uint32_t value;

    address = 0x80000000U | ((uint32_t)bus << 16) |
              ((uint32_t)slot << 11) | ((uint32_t)function << 8) |
              (offset & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(address), "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"((uint16_t)PCI_CONFIG_DATA));
    return value;
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address;

    address = 0x80000000U | ((uint32_t)bus << 16) |
              ((uint32_t)slot << 11) | ((uint32_t)function << 8) |
              (offset & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(address), "Nd"((uint16_t)PCI_CONFIG_ADDRESS));
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"((uint16_t)PCI_CONFIG_DATA));
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t value;

    value = pci_read32(bus, slot, function, offset);
    return (uint8_t)(value >> ((offset & 3) * 8));
}

static uint64_t pci_bar_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t bar) {
    uint32_t low;
    uint32_t high;
    uint64_t address;

    if (bar >= 6) return 0;
    low = pci_read32(bus, slot, function, (uint8_t)(0x10 + bar * 4));
    if (low & 1) return 0;
    address = (uint64_t)(low & 0xFFFFFFF0U);
    if ((low & 6) == 4 && bar < 5) {
        high = pci_read32(bus, slot, function, (uint8_t)(0x14 + bar * 4));
        address |= (uint64_t)high << 32;
    }
    return address;
}

static volatile uint8_t *map_capability(uint8_t bar, uint32_t offset, uint32_t length) {
    uint64_t bar_phys;
    uint64_t first_page;
    uint64_t last_page;
    uint64_t page;
    uint64_t virt_base;

    bar_phys = pci_bar_address(gpu.bus, gpu.slot, gpu.function, bar);
    if (!bar_phys || bar >= 6 || length == 0 || offset >= VIRTIO_GPU_BAR_STRIDE) return NULL;
    if ((uint64_t)offset + length > VIRTIO_GPU_BAR_STRIDE) return NULL;
    first_page = offset & ~0xFFFULL;
    last_page = ((uint64_t)offset + length + 0xFFFULL) & ~0xFFFULL;
    virt_base = VIRTIO_GPU_MMIO_BASE + (uint64_t)bar * VIRTIO_GPU_BAR_STRIDE;
    for (page = first_page; page < last_page; page += PAGE_SIZE) {
        vmm_map_page(virt_base + page, bar_phys + page, 0x003);
    }
    return (volatile uint8_t *)(virt_base + offset);
}

static int find_device(void) {
    uint16_t bus;
    uint8_t slot;
    uint8_t function;
    uint32_t id;
    uint32_t class_code;
    uint8_t base_class;
    uint8_t subclass;
    int permitted;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (function = 0; function < 8; function++) {
                id = pci_read32((uint8_t)bus, slot, function, 0);
                if ((id & 0xFFFF) == PCI_VENDOR_VIRTIO &&
                    (id >> 16) == PCI_DEVICE_GPU_MODERN) {
                    class_code = pci_read32((uint8_t)bus, slot, function, 0x08);
                    base_class = (uint8_t)(class_code >> 24);
                    subclass = (uint8_t)(class_code >> 16);
                    permitted = 0;
#if CONFIG_DRIVER_VIRTIO_VGA
                    if (base_class == PCI_CLASS_DISPLAY && subclass == PCI_SUBCLASS_VGA)
                        permitted = 1;
#endif
#if CONFIG_DRIVER_VIRTIO_GPU_PCI
                    if (base_class == PCI_CLASS_DISPLAY && subclass == PCI_SUBCLASS_OTHER_DISPLAY)
                        permitted = 1;
#endif
                    if (permitted) {
                        gpu.bus = (uint8_t)bus;
                        gpu.slot = slot;
                        gpu.function = function;
                        gpu.vga_compatible = subclass == PCI_SUBCLASS_VGA;
                        return 0;
                    }
                }
                if (function == 0 && id == 0xFFFFFFFFU) break;
            }
        }
    }
    return -1;
}

static int map_pci_capabilities(void) {
    uint8_t pointer;
    uint8_t id;
    uint8_t next;
    uint8_t type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t command;
    uint32_t guard;
    volatile uint8_t *mapped;

    command = pci_read32(gpu.bus, gpu.slot, gpu.function, 0x04);
    command |= 0x00000006U;
    pci_write32(gpu.bus, gpu.slot, gpu.function, 0x04, command);
    pointer = pci_read8(gpu.bus, gpu.slot, gpu.function, 0x34) & 0xFC;
    guard = 0;
    while (pointer >= 0x40 && guard++ < 48) {
        id = pci_read8(gpu.bus, gpu.slot, gpu.function, pointer);
        next = pci_read8(gpu.bus, gpu.slot, gpu.function, (uint8_t)(pointer + 1)) & 0xFC;
        if (id == PCI_CAP_ID_VENDOR) {
            type = pci_read8(gpu.bus, gpu.slot, gpu.function, (uint8_t)(pointer + 3));
            bar = pci_read8(gpu.bus, gpu.slot, gpu.function, (uint8_t)(pointer + 4));
            offset = pci_read32(gpu.bus, gpu.slot, gpu.function, (uint8_t)(pointer + 8));
            length = pci_read32(gpu.bus, gpu.slot, gpu.function, (uint8_t)(pointer + 12));
            mapped = map_capability(bar, offset, length);
            if (type == VIRTIO_PCI_CAP_COMMON_CFG) {
                gpu.common = (virtio_pci_common_cfg_t *)mapped;
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                gpu.notify = mapped;
                gpu.notify_multiplier = pci_read32(gpu.bus, gpu.slot, gpu.function,
                                                   (uint8_t)(pointer + 16));
            } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                (void)mapped;
            }
        }
        if (!next || next == pointer) break;
        pointer = next;
    }
    return gpu.common && gpu.notify && gpu.notify_multiplier ? 0 : -1;
}

static int map_physical_range(uint64_t phys, uint64_t pages) {
    uint64_t i;

    for (i = 0; i < pages; i++) {
        if (pt_ensure_phys_mapped(phys + i * PAGE_SIZE) < 0) return -1;
    }
    return 0;
}

static int setup_queue(void) {
    uint64_t desc_size;
    uint64_t avail_offset;
    uint64_t used_offset;
    uint64_t command_offset;
    uint64_t response_offset;
    uint64_t total_size;
    uint8_t *base;

    gpu.common->queue_select = 0;
    __asm__ __volatile__("mfence" ::: "memory");
    gpu.queue_size = gpu.common->queue_size;
    if (gpu.queue_size < 2) return -1;
    desc_size = (uint64_t)gpu.queue_size * sizeof(virtq_desc_t);
    avail_offset = (desc_size + 1) & ~1ULL;
    used_offset = (avail_offset + 6 + (uint64_t)gpu.queue_size * 2 + 3) & ~3ULL;
    command_offset = used_offset + 6 +
                     (uint64_t)gpu.queue_size * sizeof(virtq_used_elem_t);
    command_offset = (command_offset + 7) & ~7ULL;
    response_offset = command_offset + sizeof(virtio_gpu_transfer_to_host_2d_t);
    response_offset = (response_offset + 7) & ~7ULL;
    total_size = response_offset + sizeof(virtio_gpu_resp_display_info_t);
    gpu.queue_pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    gpu.queue_phys = pfa_alloc_contiguous(gpu.queue_pages);
    if (!gpu.queue_phys || map_physical_range(gpu.queue_phys, gpu.queue_pages) < 0) return -1;
    base = (uint8_t *)(gpu.queue_phys + KERNEL_VMA);
    memset(base, 0, gpu.queue_pages * PAGE_SIZE);
    gpu.desc = (virtq_desc_t *)base;
    gpu.avail_flags = (volatile uint16_t *)(base + avail_offset);
    gpu.avail_idx = gpu.avail_flags + 1;
    gpu.avail_ring = gpu.avail_flags + 2;
    gpu.used_flags = (volatile uint16_t *)(base + used_offset);
    gpu.used_idx = gpu.used_flags + 1;
    gpu.used_ring = (volatile virtq_used_elem_t *)(gpu.used_flags + 2);
    gpu.command_phys = gpu.queue_phys + command_offset;
    gpu.command = base + command_offset;
    gpu.command_capacity = sizeof(virtio_gpu_transfer_to_host_2d_t);
    gpu.response_phys = gpu.queue_phys + response_offset;
    gpu.response = base + response_offset;
    gpu.response_capacity = sizeof(virtio_gpu_resp_display_info_t);
    gpu.last_used_idx = 0;
    gpu.queue_notify_off = gpu.common->queue_notify_off;
    gpu.common->queue_desc = gpu.queue_phys;
    gpu.common->queue_driver = gpu.queue_phys + avail_offset;
    gpu.common->queue_device = gpu.queue_phys + used_offset;
    gpu.common->queue_enable = 1;
    return 0;
}

static int submit_command(const void *request, uint32_t request_size,
                          void *response, uint32_t response_size) {
    uint8_t *request_dma;
    uint8_t *response_dma;
    uint16_t avail_idx;
    uint64_t spins;
    uint64_t lock_flags;
    volatile uint16_t *notify_address;
    int result;

    if (!gpu.command || !gpu.response || request_size > gpu.command_capacity ||
        response_size > gpu.response_capacity) return -1;
    lock_flags = gpu_lock_acquire(&gpu.command_lock);
    result = -1;
    request_dma = gpu.command;
    response_dma = gpu.response;
    memcpy(request_dma, request, request_size);
    memset(response_dma, 0, response_size);
    gpu.desc[0].addr = gpu.command_phys;
    gpu.desc[0].len = request_size;
    gpu.desc[0].flags = VIRTQ_DESC_F_NEXT;
    gpu.desc[0].next = 1;
    gpu.desc[1].addr = gpu.response_phys;
    gpu.desc[1].len = response_size;
    gpu.desc[1].flags = VIRTQ_DESC_F_WRITE;
    gpu.desc[1].next = 0;
    avail_idx = *gpu.avail_idx;
    gpu.avail_ring[avail_idx % gpu.queue_size] = 0;
    __asm__ __volatile__("mfence" ::: "memory");
    *gpu.avail_idx = (uint16_t)(avail_idx + 1);
    __asm__ __volatile__("mfence" ::: "memory");
    notify_address = (volatile uint16_t *)(gpu.notify +
                     (uint64_t)gpu.queue_notify_off * gpu.notify_multiplier);
    *notify_address = 0;
    spins = 0;
    while (*gpu.used_idx == gpu.last_used_idx && spins++ < VIRTIO_GPU_TIMEOUT) {
        __asm__ __volatile__("pause");
    }
    if (*gpu.used_idx == gpu.last_used_idx) {
        gpu_lock_release(&gpu.command_lock, lock_flags);
        return -1;
    }
    gpu.last_used_idx++;
    __asm__ __volatile__("mfence" ::: "memory");
    memcpy(response, response_dma, response_size);
    result = 0;
    gpu_lock_release(&gpu.command_lock, lock_flags);
    return result;
}

static int command_ok(const void *request, uint32_t request_size) {
    virtio_gpu_ctrl_hdr_t response;
    int result;

    memset(&response, 0, sizeof(response));
    result = submit_command(request, request_size, &response, sizeof(response));
    if (result < 0) return result;
    return response.type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -1;
}

static int create_scanout(uint32_t width, uint32_t height, uint32_t resource_id) {
    virtio_gpu_resource_create_2d_t create;
    virtio_gpu_resource_attach_backing_t attach;
    virtio_gpu_set_scanout_t scanout;
    virtio_gpu_resource_id_t unref;
    uint64_t bytes;
    uint64_t framebuffer_pages;
    uint64_t framebuffer_phys;
    int resource_created;

    if (!width || !height) return -1;
    if ((uint64_t)width > UINT64_MAX / height / 4) return -1;
    bytes = (uint64_t)width * height * 4;
    if (bytes > UINT32_MAX) return -1;
    framebuffer_pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    framebuffer_phys = pfa_alloc_contiguous(framebuffer_pages);
    if (!framebuffer_phys) return -1;
    if (map_physical_range(framebuffer_phys, framebuffer_pages) < 0) {
        pfa_free_contiguous(framebuffer_phys, framebuffer_pages);
        return -1;
    }
    memset((void *)(framebuffer_phys + KERNEL_VMA), 0, framebuffer_pages * PAGE_SIZE);
    resource_created = 0;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = resource_id;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = width;
    create.height = height;
    if (command_ok(&create, sizeof(create)) < 0) goto fail;
    resource_created = 1;
    memset(&attach, 0, sizeof(attach));
    attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id = resource_id;
    attach.nr_entries = 1;
    attach.addr = framebuffer_phys;
    attach.length = (uint32_t)bytes;
    if (command_ok(&attach, sizeof(attach)) < 0) goto fail;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.rect.width = width;
    scanout.rect.height = height;
    scanout.resource_id = resource_id;
    if (command_ok(&scanout, sizeof(scanout)) < 0) goto fail;
    gpu.framebuffer_pages = framebuffer_pages;
    gpu.framebuffer_phys = framebuffer_phys;
    gpu.width = width;
    gpu.height = height;
    gpu.resource_id = resource_id;
    return 0;

fail:
    if (resource_created) {
        memset(&unref, 0, sizeof(unref));
        unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
        unref.resource_id = resource_id;
        if (command_ok(&unref, sizeof(unref)) < 0) return -1;
    }
    pfa_free_contiguous(framebuffer_phys, framebuffer_pages);
    return -1;
}

static int release_resource(uint32_t resource_id) {
    virtio_gpu_resource_id_t request;

    memset(&request, 0, sizeof(request));
    request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    request.resource_id = resource_id;
    if (command_ok(&request, sizeof(request)) < 0) return -1;
    memset(&request, 0, sizeof(request));
    request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    request.resource_id = resource_id;
    return command_ok(&request, sizeof(request));
}

int virtio_gpu_init(uint64_t preferred_width, uint64_t preferred_height) {
    virtio_gpu_ctrl_hdr_t request;
    virtio_gpu_resp_display_info_t display;
    uint32_t width;
    uint32_t height;
    uint32_t features;
    uint32_t command;

    if (gpu.active) return 0;
    memset(&gpu, 0, sizeof(gpu));
    if (find_device() < 0) return -1;
    if (map_pci_capabilities() < 0) return release_failed_device();
    gpu.common->device_status = 0;
    gpu.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    gpu.common->device_status |= VIRTIO_STATUS_DRIVER;
    gpu.common->device_feature_select = 1;
    features = gpu.common->device_feature;
    if (!(features & (1U << (VIRTIO_F_VERSION_1 - 32)))) {
        gpu.common->device_status |= VIRTIO_STATUS_FAILED;
        return release_failed_device();
    }
    gpu.common->driver_feature_select = 0;
    gpu.common->driver_feature = 0;
    gpu.common->driver_feature_select = 1;
    gpu.common->driver_feature = 1U << (VIRTIO_F_VERSION_1 - 32);
    gpu.common->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(gpu.common->device_status & VIRTIO_STATUS_FEATURES_OK))
        return release_failed_device();
    if (setup_queue() < 0) {
        gpu.common->device_status |= VIRTIO_STATUS_FAILED;
        return release_failed_device();
    }
    gpu.common->device_status |= VIRTIO_STATUS_DRIVER_OK;
    memset(&request, 0, sizeof(request));
    request.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    memset(&display, 0, sizeof(display));
    if (submit_command(&request, sizeof(request), &display, sizeof(display)) < 0 ||
        display.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
        return release_failed_device();
    width = display.pmodes[0].width;
    height = display.pmodes[0].height;
    if (gpu.vga_compatible) {
        if (preferred_width && preferred_width <= UINT32_MAX) width = (uint32_t)preferred_width;
        if (preferred_height && preferred_height <= UINT32_MAX) height = (uint32_t)preferred_height;
    } else {
        width = VIRTIO_GPU_PCI_BOOT_WIDTH;
        height = VIRTIO_GPU_PCI_BOOT_HEIGHT;
    }
    if (!width || !height) return release_failed_device();
    if (create_scanout(width, height, 1) < 0) return release_failed_device();
    gpu.active = 1;
    terminal_init_fb(gpu.framebuffer_phys, width, height, (uint64_t)width * 4, 32, 1);
    if (console_is_initialized()) console_force_redraw();
    virtio_gpu_flush();
    command = pci_read32(gpu.bus, gpu.slot, gpu.function, 0x04);
    printf("Virtio GPU: PCI %u:%u.%u, %ux%u, command=0x%04X\n",
           gpu.bus, gpu.slot, gpu.function, width, height, command & 0xFFFF);
    return 0;
}

int virtio_gpu_is_available(void) {
    return gpu.active;
}

int virtio_gpu_set_mode(uint64_t width, uint64_t height, uint64_t *framebuffer_phys) {
    uint64_t old_phys;
    uint64_t old_pages;
    uint32_t old_resource_id;
    uint32_t new_resource_id;
    uint32_t old_width;
    uint32_t old_height;

    if (!gpu.active || !framebuffer_phys || !width || !height) return -1;
    if (width > UINT32_MAX || height > UINT32_MAX) return -1;
    old_phys = gpu.framebuffer_phys;
    old_pages = gpu.framebuffer_pages;
    old_resource_id = gpu.resource_id;
    old_width = gpu.width;
    old_height = gpu.height;
    new_resource_id = old_resource_id + 1;
    if (!new_resource_id) new_resource_id = 1;
    gpu.active = 0;
    __asm__ __volatile__("mfence" ::: "memory");
    if (create_scanout((uint32_t)width, (uint32_t)height, new_resource_id) < 0) {
        gpu.framebuffer_phys = old_phys;
        gpu.framebuffer_pages = old_pages;
        gpu.resource_id = old_resource_id;
        gpu.width = old_width;
        gpu.height = old_height;
        gpu.active = 1;
        virtio_gpu_mark_dirty(0, 0, gpu.width, gpu.height);
        return -1;
    }
    if (release_resource(old_resource_id) == 0) {
        pfa_free_contiguous(old_phys, old_pages);
    }
    *framebuffer_phys = gpu.framebuffer_phys;
    gpu.active = 1;
    virtio_gpu_mark_dirty(0, 0, width, height);
    return 0;
}

void virtio_gpu_mark_dirty(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    uint64_t x2;
    uint64_t y2;
    uint64_t lock_flags;

    if (!gpu.active || !width || !height || x >= gpu.width || y >= gpu.height) return;
    x2 = width > gpu.width - x ? gpu.width : x + width;
    y2 = height > gpu.height - y ? gpu.height : y + height;
    lock_flags = gpu_lock_acquire(&gpu.dirty_lock);
    if (!gpu.dirty_valid) {
        gpu.dirty_x1 = (uint32_t)x;
        gpu.dirty_y1 = (uint32_t)y;
        gpu.dirty_x2 = (uint32_t)x2;
        gpu.dirty_y2 = (uint32_t)y2;
        gpu.dirty_valid = 1;
    } else {
        if (x < gpu.dirty_x1) gpu.dirty_x1 = (uint32_t)x;
        if (y < gpu.dirty_y1) gpu.dirty_y1 = (uint32_t)y;
        if (x2 > gpu.dirty_x2) gpu.dirty_x2 = (uint32_t)x2;
        if (y2 > gpu.dirty_y2) gpu.dirty_y2 = (uint32_t)y2;
    }
    gpu_lock_release(&gpu.dirty_lock, lock_flags);
}

void virtio_gpu_flush(void) {
    virtio_gpu_transfer_to_host_2d_t transfer;
    virtio_gpu_resource_flush_t flush;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint64_t lock_flags;

    if (!gpu.active) return;
    lock_flags = gpu_lock_acquire(&gpu.dirty_lock);
    if (!gpu.dirty_valid) {
        gpu_lock_release(&gpu.dirty_lock, lock_flags);
        return;
    }
    x = gpu.dirty_x1;
    y = gpu.dirty_y1;
    width = gpu.dirty_x2 - gpu.dirty_x1;
    height = gpu.dirty_y2 - gpu.dirty_y1;
    gpu.dirty_valid = 0;
    gpu_lock_release(&gpu.dirty_lock, lock_flags);
    memset(&transfer, 0, sizeof(transfer));
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.rect.x = x;
    transfer.rect.y = y;
    transfer.rect.width = width;
    transfer.rect.height = height;
    transfer.offset = ((uint64_t)y * gpu.width + x) * 4;
    transfer.resource_id = gpu.resource_id;
    if (command_ok(&transfer, sizeof(transfer)) < 0) {
        virtio_gpu_mark_dirty(x, y, width, height);
        return;
    }
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.rect.x = x;
    flush.rect.y = y;
    flush.rect.width = width;
    flush.rect.height = height;
    flush.resource_id = gpu.resource_id;
    if (command_ok(&flush, sizeof(flush)) < 0)
        virtio_gpu_mark_dirty(x, y, width, height);
}

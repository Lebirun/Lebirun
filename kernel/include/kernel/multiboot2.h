#ifndef _KERNEL_MULTIBOOT2_H
#define _KERNEL_MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_MAGIC 0x36d76289

#define MULTIBOOT2_TAG_END            0
#define MULTIBOOT2_TAG_CMDLINE        1
#define MULTIBOOT2_TAG_BOOT_LOADER    2
#define MULTIBOOT2_TAG_MODULE         3
#define MULTIBOOT2_TAG_BASIC_MEMINFO  4
#define MULTIBOOT2_TAG_BOOTDEV        5
#define MULTIBOOT2_TAG_MMAP           6
#define MULTIBOOT2_TAG_VBE            7
#define MULTIBOOT2_TAG_FRAMEBUFFER    8
#define MULTIBOOT2_TAG_ELF_SECTIONS   9
#define MULTIBOOT2_TAG_APM            10
#define MULTIBOOT2_TAG_EFI32          11
#define MULTIBOOT2_TAG_EFI64          12
#define MULTIBOOT2_TAG_SMBIOS         13
#define MULTIBOOT2_TAG_ACPI_OLD       14
#define MULTIBOOT2_TAG_ACPI_NEW       15

#define MULTIBOOT2_MMAP_AVAILABLE        1
#define MULTIBOOT2_MMAP_RESERVED         2
#define MULTIBOOT2_MMAP_ACPI_RECLAIMABLE 3
#define MULTIBOOT2_MMAP_NVS              4
#define MULTIBOOT2_MMAP_BADRAM           5

struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot2_tag_string {
    uint32_t type;
    uint32_t size;
    char string[];
};

struct multiboot2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
};

struct multiboot2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
};

struct multiboot2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot2_mmap_entry entries[];
};

struct multiboot2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    union {
        struct {
            uint32_t framebuffer_palette_num_colors;
        };
        struct {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        };
    };
};

static inline uint32_t multiboot2_total_size(void *mb_info) {
    return *(uint32_t *)mb_info;
}

static inline struct multiboot2_tag *multiboot2_first_tag(void *mb_info) {
    return (struct multiboot2_tag *)((uint8_t *)mb_info + 8);
}

static inline struct multiboot2_tag *multiboot2_next_tag(struct multiboot2_tag *tag) {
    uint32_t sz = (tag->size + 7) & ~7u;
    return (struct multiboot2_tag *)((uint8_t *)tag + sz);
}

#endif

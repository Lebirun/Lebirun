#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <lebirun/mem_map.h>

#include "xz.h"

#define XZ_DEC_SINGLE

#define GFP_KERNEL 0

void *xz_kmalloc_wrapper(unsigned long size);
void xz_kfree_wrapper(void *ptr);
#define kmalloc(size, flags) xz_kmalloc_wrapper(size)
#define kfree(ptr) xz_kfree_wrapper(ptr)
#define vmalloc(size) xz_kmalloc_wrapper(size)
#define vfree(ptr) xz_kfree_wrapper(ptr)

#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define min_t(type, x, y) min(x, y)

#define fallthrough __attribute__((__fallthrough__))

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif

static inline uint32_t get_unaligned_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0]
		| ((uint32_t)buf[1] << 8)
		| ((uint32_t)buf[2] << 16)
		| ((uint32_t)buf[3] << 24);
}

static inline uint32_t get_unaligned_be32(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24)
		| ((uint32_t)buf[1] << 16)
		| ((uint32_t)buf[2] << 8)
		| (uint32_t)buf[3];
}

#define get_le32 get_unaligned_le32
#define get_be32 get_unaligned_be32

#endif

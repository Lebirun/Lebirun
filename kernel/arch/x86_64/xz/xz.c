#include <stdint.h>
#include <stddef.h>
#include <lebirun/mem_map.h>
#include "xz.h"

static volatile int xz_init_state;

static void xz_initialize(void)
{
	uint64_t eflags;

	__asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
	if (__atomic_load_n(&xz_init_state, __ATOMIC_ACQUIRE) == 2) {
		if (eflags & (1ULL << 9))
			__asm__ volatile ("sti" ::: "memory");
		return;
	}
	if (__sync_bool_compare_and_swap(&xz_init_state, 0, 1)) {
		xz_crc32_init();
		__atomic_store_n(&xz_init_state, 2, __ATOMIC_RELEASE);
		if (eflags & (1ULL << 9))
			__asm__ volatile ("sti" ::: "memory");
		return;
	}
	if (eflags & (1ULL << 9))
		__asm__ volatile ("sti" ::: "memory");
	while (__atomic_load_n(&xz_init_state, __ATOMIC_ACQUIRE) != 2)
		__asm__ volatile ("pause" ::: "memory");
}

void *xz_kmalloc_wrapper(unsigned long size)
{
	return kmalloc((size_t)size);
}

void xz_kfree_wrapper(void *ptr)
{
	kfree(ptr);
}

int xz_decompress_xz(const uint8_t *src, uint64_t src_len,
		      uint8_t *dst, uint64_t dst_len, uint64_t *out_len)
{
	struct xz_dec *dec;
	struct xz_buf buf;
	enum xz_ret ret;

	xz_initialize();

	dec = xz_dec_init(XZ_SINGLE, 0);
	if (!dec)
		return -1;

	buf.in = src;
	buf.in_pos = 0;
	buf.in_size = (size_t)src_len;
	buf.out = dst;
	buf.out_pos = 0;
	buf.out_size = (size_t)dst_len;

	ret = xz_dec_run(dec, &buf);
	xz_dec_end(dec);

	if (ret == XZ_STREAM_END) {
		if (out_len)
			*out_len = buf.out_pos;
		return 0;
	}

	return -1;
}

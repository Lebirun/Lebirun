#include <stdint.h>
#include <stddef.h>
#include <kernel/mem_map.h>
#include "xz.h"

static int xz_inited;

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

	if (!xz_inited) {
		xz_crc32_init();
		xz_inited = 1;
	}

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

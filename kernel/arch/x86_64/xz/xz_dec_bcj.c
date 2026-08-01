#include "xz_private.h"

#ifdef XZ_DEC_BCJ

#ifndef XZ_DEC_SINGLE
#error XZ_DEC_SINGLE_required
#endif

struct xz_dec_bcj {
	uint8_t unused;
};

#define BCJ_X86 4

static inline int bcj_x86_test_msbyte(uint8_t value)
{
	return value == 0x00 || value == 0xFF;
}

static size_t bcj_x86(uint8_t *buf, size_t size)
{
	static const uint8_t mask_to_bit_num[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
	size_t i;
	size_t prev_pos;
	uint32_t prev_mask;
	uint32_t src;
	uint32_t dest;
	uint32_t shift;
	uint8_t value;

	prev_pos = (size_t)-1;
	prev_mask = 0;
	if (size <= 4)
		return 0;

	size -= 4;
	for (i = 0; i < size; ++i) {
		if ((buf[i] & 0xFE) != 0xE8)
			continue;

		prev_pos = i - prev_pos;
		if (prev_pos > 3) {
			prev_mask = 0;
		} else {
			prev_mask = (prev_mask << (prev_pos - 1)) & 7;
			if (prev_mask != 0) {
				value = buf[i + 4 - mask_to_bit_num[prev_mask]];
				if ((0x17u & (1u << prev_mask)) == 0
						|| bcj_x86_test_msbyte(value)) {
					prev_pos = i;
					prev_mask = (prev_mask << 1) | 1;
					continue;
				}
			}
		}

		prev_pos = i;
		if (bcj_x86_test_msbyte(buf[i + 4])) {
			src = get_unaligned_le32(buf + i + 1);
			for (;;) {
				dest = src - ((uint32_t)i + 5);
				if (prev_mask == 0)
					break;
				shift = mask_to_bit_num[prev_mask] * 8;
				value = (uint8_t)(dest >> (24 - shift));
				if (!bcj_x86_test_msbyte(value))
					break;
				src = dest ^ (((uint32_t)1 << (32 - shift)) - 1);
			}

			dest &= 0x01FFFFFF;
			dest |= (uint32_t)0 - (dest & 0x01000000);
			put_unaligned_le32(dest, buf + i + 1);
			i += 4;
		} else {
			prev_mask = (prev_mask << 1) | 1;
		}
	}

	prev_pos = i - prev_pos;
	return i;
}

static void bcj_apply(uint8_t *buf, size_t *pos, size_t size)
{
	size_t filtered;

	buf += *pos;
	size -= *pos;
	filtered = bcj_x86(buf, size);
	*pos += filtered;
}

XZ_EXTERN enum xz_ret xz_dec_bcj_run(struct xz_dec_bcj *state,
		struct xz_dec_lzma2 *lzma2, struct xz_buf *buf)
{
	size_t out_start;
	enum xz_ret result;

	out_start = buf->out_pos;
	result = xz_dec_lzma2_run(lzma2, buf);
	if (result != XZ_STREAM_END)
		return result;
	bcj_apply(buf->out, &out_start, buf->out_pos);
	return XZ_STREAM_END;
}

XZ_EXTERN struct xz_dec_bcj *xz_dec_bcj_create(bool single_call)
{
	struct xz_dec_bcj *state;

	(void)single_call;
	state = kmalloc(sizeof(*state), GFP_KERNEL);
	return state;
}

XZ_EXTERN enum xz_ret xz_dec_bcj_reset(struct xz_dec_bcj *state, uint8_t id)
{
	(void)state;
	if (id != BCJ_X86)
		return XZ_OPTIONS_ERROR;
	return XZ_OK;
}

#endif

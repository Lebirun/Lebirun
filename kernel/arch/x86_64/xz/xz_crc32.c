// SPDX-License-Identifier: 0BSD

/*
 * CRC32 using the polynomial from IEEE-802.3
 *
 * Authors: Lasse Collin <lasse.collin@tukaani.org>
 *          Igor Pavlov <https://7-zip.org/>
 */

/*
 * This is not the fastest implementation, but it is pretty compact.
 * The fastest versions of xz_crc32() on modern CPUs without hardware
 * accelerated CRC instruction are 3-5 times as fast as this version,
 * but they are bigger and use more memory for the lookup table.
 */

#include "xz_private.h"

/*
 * STATIC_RW_DATA is used in the pre-boot environment on some architectures.
 * See <linux/decompress/mm.h> for details.
 */
#ifndef STATIC_RW_DATA
#	define STATIC_RW_DATA static
#endif

XZ_EXTERN void xz_crc32_init(void)
{
	return;
}

XZ_EXTERN uint32_t xz_crc32(const uint8_t *buf, size_t size, uint32_t crc)
{
	const uint32_t poly = 0xEDB88320;
	uint32_t r;
	uint32_t i;

	crc = ~crc;

	while (size != 0) {
		r = *buf++ ^ (crc & 0xFF);
		for (i = 0; i < 8; ++i)
			r = (r >> 1) ^ (poly & ~((r & 1) - 1));
		crc = r ^ (crc >> 8);
		--size;
	}

	return ~crc;
}

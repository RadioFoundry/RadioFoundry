// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F codec descriptor: where the merge put the radio's stock codec in internal flash (docs/spec/codec-gd77.md).
// 128 bytes at 0x7B780, written with the codec by the merge tool (tools/odrflash/odrflash/gd77codec.py); keep the two
// in step. Same layout as the STM32 radios' descriptor (platform/stm32f4/codec_desc.h). The codec runs in place
// from flash, so the firmware only needs to know where its code is and that the bytes there are what the merge wrote.
// No hardware here: the host tests run it (tests/host/codecDescTest.c).
#ifndef ODR_K22F_CODEC_DESC_H
#define ODR_K22F_CODEC_DESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CODEC_DESC_ADDR		0x0007B780u
#define CODEC_DESC_MAGIC	0x43534544u	// "DESC" little-endian
#define CODEC_DESC_VERSION	1u
#define CODEC_DESC_WINDOWS	5u		// table windows (the layout allows five; the GD-77 uses one)

typedef struct {
	uint32_t addr, len, crc;		// CRC-32 of the bytes as merged
} codec_desc_window_t;

typedef struct {
	uint32_t magic, version;
	uint32_t tag;				// which stock build (the firmware's profile: codec_profile_tag())
	codec_desc_window_t code;
	uint32_t windows;			// table windows used
	codec_desc_window_t table[CODEC_DESC_WINDOWS];
	uint32_t reserved[9];
	uint32_t crc;				// CRC-32 of the 124 bytes before it
} codec_desc_t;
_Static_assert(sizeof(codec_desc_t) == 128, "codec descriptor layout");

static inline uint32_t codec_desc_crc32(const uint8_t *p, uint32_t len, uint32_t c)
{
	static const uint32_t nib[16] = {
		0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
		0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
	};
	c = ~c;
	while (len--) {
		c ^= *p++;
		c = (c >> 4) ^ nib[c & 15];
		c = (c >> 4) ^ nib[c & 15];
	}
	return ~c;
}

// The descriptor itself: magic, version, CRC, the build the firmware supports, every window inside [lo, hi) (the flash
// the board leaves to the codec), the code word aligned and long enough for the entries. The windows' own CRCs are
// checked by codec_desc_check_bytes(), which reads 140 KB and runs once.
static inline bool codec_desc_check(const codec_desc_t *d, uint32_t tag, uint32_t lo, uint32_t hi, uint32_t min_code)
{
	if (d->magic != CODEC_DESC_MAGIC || d->version != CODEC_DESC_VERSION || d->tag != tag ||
		d->windows > CODEC_DESC_WINDOWS ||
		codec_desc_crc32((const uint8_t *)d, offsetof(codec_desc_t, crc), 0) != d->crc) {
		return false;
	}
	if ((d->code.addr & 3u) != 0 || d->code.len < min_code ||
		d->code.addr < lo || d->code.addr > hi || d->code.len > hi - d->code.addr) {
		return false;
	}
	for (uint32_t i = 0; i < d->windows; i++) {
		const codec_desc_window_t *w = &d->table[i];
		if (w->addr < lo || w->addr > hi || w->len > hi - w->addr) {
			return false;
		}
	}
	return true;
}

// The bytes at each window against its CRC. base: where address lo is mapped (the flash itself on the radio, a buffer
// in the host test)
static inline bool codec_desc_check_bytes(const codec_desc_t *d, const uint8_t *base, uint32_t lo)
{
	if (codec_desc_crc32(base + (d->code.addr - lo), d->code.len, 0) != d->code.crc) {
		return false;
	}
	for (uint32_t i = 0; i < d->windows; i++) {
		const codec_desc_window_t *w = &d->table[i];
		if (codec_desc_crc32(base + (w->addr - lo), w->len, 0) != w->crc) {
			return false;
		}
	}
	return true;
}

#endif

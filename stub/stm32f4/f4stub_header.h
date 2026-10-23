// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 stub's image header (docs/spec/stub-stm32f4.md 3.1): 16 bytes at a fixed offset in the image, right
// after the entry code. The one definition of its layout and numbers, shared by the stub (f4stub_main.c), ODR
// (platform/stm32f4/stub_entry.c) and the host tests. f4stub.py and the web flasher read the same layout, and
// f4stub.py --self-test checks its copies of these numbers against this file. Little endian, as the image.
#ifndef F4STUB_HEADER_H
#define F4STUB_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#define F4STUB_HDR_OFFSET	0x10u		// f4stub.ld places .stub_header here
#define F4STUB_HDR_MAGIC	0x48533446u	// "F4SH" in image byte order
#define F4STUB_HDR_LAYOUT	1u		// this layout; a later one only appends fields
#define F4STUB_HDR_SIZE		16u

// The stub's version, in the header and in HELLO. 2: the external SPI flash (memory 1, XERASE, XPROGRAM). 3: the
// image header, and the stub feeds the IWDG. 4: CONFIG, the board record and HELLO's echo (the shared engine)
#define F4STUB_VERSION			4u
#define F4STUB_VERSION_FEEDS_IWDG	3u	// the first version that feeds the IWDG

#define F4STUB_FLAG_FEEDS_IWDG		0x00000001u	// feeds the IWDG in every loop that can take time

// The stub's RESET marks itself as asked for in ODR's restart-cause words (docs/spec/crash-log.md 1.3: crash_boot_t.soft
// in the backup SRAM, after the crash record), so the next start doesn't take it for the watchdog. ODR checks the
// address against its own layout (crash_stm32.c)
#define F4STUB_SOFT_MARK_ADDR		0x40024104u
#define F4STUB_SOFT_MARK		0x54464F53u	// "SOFT", CRASH_BOOT_SOFT

typedef struct {
	uint32_t magic;		// F4STUB_HDR_MAGIC
	uint16_t layout;	// F4STUB_HDR_LAYOUT
	uint16_t size;		// F4STUB_HDR_SIZE
	uint32_t version;	// F4STUB_VERSION
	uint32_t flags;		// F4STUB_FLAG_*
} f4stub_header_t;

static inline uint32_t f4stub_hdr_le(const uint8_t *p, unsigned n)
{
	uint32_t v = 0;
	while (n-- > 0) {
		v = (v << 8) | p[n];
	}
	return v;
}

// The image may run while the IWDG runs: its header has the magic, a version of at least F4STUB_VERSION_FEEDS_IWDG
// and the feed flag. All three are required; a missing or garbage header (every stub before version 3) fails. With the
// IWDG not running any image passes. Read byte by byte: the staging buffer need not be aligned
static inline bool f4stub_image_ok(const uint8_t *img, uint32_t len, bool iwdg_running)
{
	if (!iwdg_running) {
		return true;
	}
	if (img == 0 || len < F4STUB_HDR_OFFSET + F4STUB_HDR_SIZE) {
		return false;
	}
	const uint8_t *h = img + F4STUB_HDR_OFFSET;
	return f4stub_hdr_le(h, 4) == F4STUB_HDR_MAGIC && f4stub_hdr_le(h + 4, 2) >= F4STUB_HDR_LAYOUT &&
	       f4stub_hdr_le(h + 6, 2) >= F4STUB_HDR_SIZE && f4stub_hdr_le(h + 8, 4) >= F4STUB_VERSION_FEEDS_IWDG &&
	       (f4stub_hdr_le(h + 12, 4) & F4STUB_FLAG_FEEDS_IWDG) != 0;
}

#endif

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT32F43x flash stub's image header and entry registers (docs/spec/stub-at32f43x.md 3): the one definition shared
// by the stub (at32stub_main.c), RadioFoundry (platform/at32f43x/stub_entry.c) and the host tests. The layout is the
// other stubs' (stub/stm32f4/f4stub_header.h, stub/gd32f30x/gd32stub_header.h) with this target's own magic, so a host
// or a RadioFoundry can't take one target's image for another's. Little endian, as the image.
#ifndef AT32STUB_HEADER_H
#define AT32STUB_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#define AT32STUB_HDR_OFFSET	0x10u		// at32stub.ld places .stub_header here
#define AT32STUB_HDR_MAGIC	0x48535441u	// "ATSH" in image byte order
#define AT32STUB_HDR_LAYOUT	1u
#define AT32STUB_HDR_SIZE	16u

// The stub's version, in the header and in HELLO. 1: the first, with the shared engine's CONFIG and the WDT feed
#define AT32STUB_VERSION		1u
#define AT32STUB_FLAG_FEEDS_WDT		0x00000001u	// feeds the watchdog in every loop that can take time

#define AT32STUB_RUN_ADDR	0x20000000u	// the link address: RadioFoundry copies the image here before the jump
#define AT32STUB_IMAGE_MAX	(8u * 1024u)	// at32stub.ld refuses a bigger image

// The registers at entry (Thread mode, main stack, interrupts masked, every NVIC source off):
//   r0  AT32STUB_ENTRY_* flags
//   r1  the core clock in Hz, as RadioFoundry set it (the stub's timing; no crystal fact needed)
//   r2  where the stub's RESET writes AT32STUB_SOFT_MARK (RadioFoundry's restart-cause word, docs/spec/crash-log.md
//       1.3), 0 for none. The address is RadioFoundry's to give: the kept RAM sits at the top of each board's SRAM
#define AT32STUB_ENTRY_USB_ADOPT	0x1u	// the OTGFS1 core is RadioFoundry's configured CDC-ACM device
#define AT32STUB_SOFT_MARK		0x54464F53u	// "SOFT", CRASH_BOOT_SOFT

typedef struct {
	uint32_t magic;		// AT32STUB_HDR_MAGIC
	uint16_t layout;	// AT32STUB_HDR_LAYOUT
	uint16_t size;		// AT32STUB_HDR_SIZE
	uint32_t version;	// AT32STUB_VERSION
	uint32_t flags;		// AT32STUB_FLAG_*
} at32stub_header_t;

static inline uint32_t at32stub_hdr_le(const uint8_t *p, unsigned n)
{
	uint32_t v = 0;
	while (n-- > 0) {
		v = (v << 8) | p[n];
	}
	return v;
}

// The image may run here: it has this target's header, and with the watchdog running, the feed flag. The header is
// required either way: RadioFoundry copies the image to the run address before it jumps, and only an image built for
// that address and this chip may go there. Read byte by byte: the staging span need not be aligned
static inline bool at32stub_image_ok(const uint8_t *img, uint32_t len, bool wdt_running)
{
	if (img == 0 || len < AT32STUB_HDR_OFFSET + AT32STUB_HDR_SIZE || len > AT32STUB_IMAGE_MAX) {
		return false;
	}
	const uint8_t *h = img + AT32STUB_HDR_OFFSET;
	if (at32stub_hdr_le(h, 4) != AT32STUB_HDR_MAGIC || at32stub_hdr_le(h + 4, 2) < AT32STUB_HDR_LAYOUT ||
	    at32stub_hdr_le(h + 6, 2) < AT32STUB_HDR_SIZE || at32stub_hdr_le(h + 8, 4) < 1u) {
		return false;
	}
	return !wdt_running || (at32stub_hdr_le(h + 12, 4) & AT32STUB_FLAG_FEEDS_WDT) != 0;
}

#endif

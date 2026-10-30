// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SCREENSHOT reply in its packed form (docs/spec/debuglink.md 3.3): the frame buffer at the panel's own depth as one
// LZ4 block, a window of the stream per reply, with the palette and the frame count that tell the host the read was
// whole. No RTOS, no board: the host tests run it.
#ifndef DL_SHOT_H
#define DL_SHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DL_SHOT_LZ4		3u	// the request's and the reply's format
#define DL_SHOT_HDR		28u	// reply bytes before the palette, status included
#define DL_SHOT_CHUNK_MAX	1000u	// stream bytes per reply

typedef struct {
	const uint8_t *fb;	// rows of stride bytes: bpp 1 MSB left, bpp 4 high nibble left, bpp 16 RGB565
	uint32_t size;		// stride x height (at most 65535: one LZ4 block)
	uint16_t width, height;
	uint8_t bpp;
	uint8_t insets[4];	// top, bottom, left, right
	uint32_t frame;		// frames begun (gfx_frame_idle)
	bool drawing;		// a frame is being drawn
	uint16_t palette[16];	// bpp 4: RGB565 per index
} dl_shot_t;

// The reply (status OK first) with the stream from offset on, at most cap bytes in all; *last says it holds the
// stream's end. 0 when offset is past the end
size_t dl_shot_reply(const dl_shot_t *s, uint32_t offset, uint8_t *out, size_t cap, bool *last);

#endif

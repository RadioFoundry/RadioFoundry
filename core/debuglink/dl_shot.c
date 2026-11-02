// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The packed SCREENSHOT reply (dl_shot.h, docs/spec/debuglink.md 3.3)
#include "debuglink/dl_shot.h"
#include "debuglink/dl_frame.h"
#include "cps/cps_lz4.h"

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

size_t dl_shot_reply(const dl_shot_t *s, uint32_t offset, uint8_t *out, size_t cap, bool *last)
{
	size_t hdr = DL_SHOT_HDR + (s->bpp == 4 ? 32u : 0u);
	if (cap <= hdr) {
		return 0;
	}
	uint32_t room = (uint32_t)(cap - hdr) < DL_SHOT_CHUNK_MAX ? (uint32_t)(cap - hdr) : DL_SHOT_CHUNK_MAX;
	// The whole frame is encoded for every reply and only this window kept: no buffer, and a frame of 10 KB takes
	// well under a millisecond
	uint32_t total = cps_lz4_encode(s->fb, s->size, out + hdr, offset, room);
	if (offset > total) {
		return 0;
	}
	uint32_t n = total - offset < room ? total - offset : room;
	*last = offset + n == total;
	out[0] = DL_ST_OK;
	put16(out + 1, s->width);
	put16(out + 3, s->height);
	out[5] = s->bpp;
	out[6] = DL_SHOT_LZ4;
	for (int i = 0; i < 4; i++) {
		out[7 + i] = s->insets[i];
	}
	put32(out + 11, total);
	put32(out + 15, offset);
	put32(out + 19, s->size);
	put32(out + 23, s->frame);
	out[27] = s->drawing ? 1u : 0u;
	if (s->bpp == 4) {
		for (int i = 0; i < 16; i++) {
			put16(out + DL_SHOT_HDR + 2 * i, s->palette[i]);
		}
	}
	return hdr + n;
}

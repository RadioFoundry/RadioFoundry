// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LZ4 blocks (the public LZ4 block format) for the fast codeplug protocol (docs/spec/cps-fast.md 5): a greedy encoder
// with a 64-entry hash table on the stack, for blocks the radio sends, and a streaming decoder that takes the
// compressed bytes in any pieces and writes straight into the output block, for blocks the host sends. Neither needs a
// buffer of its own. No RTOS, no board: the host tests run both.
#ifndef ODR_CPS_LZ4_H
#define ODR_CPS_LZ4_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Encode src[0..n) (n at most 65535). The stream's bytes from index skip on, at most cap of them, go to out; the
// return is the whole stream's length, so a caller asks for the next window with skip += cap. out may be NULL with
// cap 0 to measure. Matches end 5 bytes before the end of the block and start at least 12 before it, as the format
// asks, so any LZ4 decoder takes the stream
uint32_t cps_lz4_encode(const uint8_t *src, uint32_t n, uint8_t *out, uint32_t skip, uint32_t cap);

typedef struct {
	uint8_t *out;		// the decoded block
	uint32_t size;		// its length: the stream must decode to exactly this
	uint32_t o;		// bytes decoded so far
	uint32_t lit;		// literals still to copy
	uint32_t ml;		// match length being assembled
	uint16_t off;
	uint8_t state;
	bool bad;		// corrupt: an offset before the start, a length past the end, bytes after the end
} cps_lz4_dec_t;

void cps_lz4_dec_init(cps_lz4_dec_t *d, uint8_t *out, uint32_t size);
// Take the next n bytes of the stream. False once the stream is known to be corrupt
bool cps_lz4_dec_feed(cps_lz4_dec_t *d, const uint8_t *in, uint32_t n);
// The stream ended: true when it was whole and decoded to exactly size bytes
bool cps_lz4_dec_done(const cps_lz4_dec_t *d);

#endif

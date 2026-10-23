// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Quadrature (rotary encoder) decoding, independent of the board's pins. The board samples both lines on every edge
// of either one (or polls them fast) and passes the levels in; the decoder turns that into detents.
//
// The two lines form a 2-bit Gray code, AB = 00 -> 01 -> 11 -> 10 -> 00 one way (+1 each) and the reverse the other
// way. Every sample is looked up against the previous one in a 16-entry table: no change, one step either way, or
// both lines changed at once (illegal: a sample was missed; it is counted and ignored, never guessed). Steps are summed
// and a detent is reported only when the lines reach a rest state (a mechanical detent position) with a whole
// detent's worth of steps in the same direction. So contact bounce (a line flicking back and forth, +1 -1 +1 ...)
// and a knob wobbling around a detent without reaching the next one never count, and a missed sample can lose at most
// the detent it fell in, never add one or count backward.
#ifndef QUADRATURE_H
#define QUADRATURE_H

#include <stdint.h>
#include <stdbool.h>

#define QUAD_AB(a, b)	((uint8_t)(((a) ? 2u : 0u) | ((b) ? 1u : 0u)))
// Rest-state masks: bit n set = AB state n is a detent position
#define QUAD_REST_ALL	0x0Fu	// one step per detent (every state)
#define QUAD_REST_EQUAL	0x09u	// two steps per detent, resting where A == B (00 and 11)
#define QUAD_REST_11	0x08u	// four steps per detent, resting at 11 (both lines released, with pull-ups)

typedef struct {
	uint8_t ab;		// the last sample (QUAD_AB)
	uint8_t steps;		// steps per detent: 1, 2 or 4
	uint8_t rest;		// rest-state mask
	int8_t acc;		// steps since the last rest state
	int8_t last_step;	// direction of the last valid step (for the reversal count)
	volatile int32_t detents;	// running total, +1 per detent forward
	volatile uint32_t transitions;	// valid single steps seen
	volatile uint32_t illegal;	// samples where both lines changed (ignored)
	volatile uint32_t reversals;	// valid steps opposite to the one before (bounce, or a turn back)
} quad_dec_t;

// steps per detent (1, 2 or 4) and its rest-state mask; ab = the lines' levels now (QUAD_AB)
void quad_init(quad_dec_t *q, uint8_t steps, uint8_t rest, uint8_t ab);
// One sample of both lines; returns the detents it completed (-1, 0 or +1). Safe to call from an interrupt; with a
// single writer the counters can be read from anywhere (32-bit reads)
int8_t quad_sample(quad_dec_t *q, uint8_t ab);
// For the consumer (another task): detents since the last call, clamped to -127..127; what doesn't fit stays for the
// next call, nothing is dropped. *taken is the consumer's own copy of the total it has taken so far
int8_t quad_take(const quad_dec_t *q, int32_t *taken);

#endif

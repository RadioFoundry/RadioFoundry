// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Rate conversion from a ring of samples at one rate to blocks at another: a board's ADC audio (e.g. a discriminator
// sampled at some rate the ADC clock allows) onto its 8 kHz PCM stream (hal_audio_tap.h). Linear interpolation with a
// Q16 phase; fine for voice when the input rate is a little above the output rate, as nothing filters the band
// between the two Nyquist frequencies.
//
// The producer (an interrupt) writes ring[head % cap] and counts head up; the reader keeps its own tail. The two
// clocks come from different crystals, so the reader's fill drifts. Before each block it checks the fill: fewer
// samples than the block needs, or more than twice the target (a stalled reader, or a ring overrun), and it sets the
// tail back to target samples behind head, a slip of a few ms (counted). Until the producer has written target
// samples the blocks are silence; a producer that stopped (head unmoved since the last block) gives silence too,
// never the last ms of the ring over and over.
//
// Header only and allocation free, so a PCM interrupt or audio task can call it.
#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t step;		// input samples per output sample, Q16
	uint32_t frac;		// position past prev towards the next input sample, Q16
	uint32_t tail;		// the next input sample (ring index count)
	uint32_t start;		// head when the reader started
	uint32_t last_head;	// head at the last block
	int16_t prev;		// the input sample at the current position
	uint8_t primed;		// the producer has written target samples since start
	uint32_t slips;		// times the tail was set back to target (diagnostics)
	uint32_t holds;		// output samples that repeated the last input: the ring ran dry within a block
} resample_t;

// in_hz, out_hz > 0. head: the producer's count now
static inline void resample_init(resample_t *r, uint32_t in_hz, uint32_t out_hz, uint32_t head)
{
	r->step = (uint32_t)(((uint64_t)in_hz << 16) / out_hz);
	r->frac = 0;
	r->tail = head;
	r->start = head;
	r->last_head = head;
	r->prev = 0;
	r->primed = 0;
	r->slips = 0;
	r->holds = 0;
}

// Input samples n outputs take, rounded up (what the fill must hold at a block's start)
static inline uint32_t resample_need(const resample_t *r, uint32_t n)
{
	return (uint32_t)(((uint64_t)n * r->step + r->frac + 0xFFFFu) >> 16);
}

// n output samples into out from ring (cap entries, a power of two) with the producer at head. target: the fill the
// reader keeps, at least resample_need(n) plus one producer burst and at most cap
static inline void resample_block(resample_t *r, const volatile int16_t *ring, uint32_t cap, uint32_t head,
				  int16_t *out, uint32_t n, uint32_t target)
{
	uint32_t mask = cap - 1u;
	bool stalled = head == r->last_head;
	r->last_head = head;
	if (!r->primed || (stalled && head - r->tail < resample_need(r, n))) {
		if (!r->primed && head - r->start >= target) {
			r->primed = 1;
		} else {
			for (uint32_t i = 0; i < n; i++) {
				out[i] = 0;
			}
			r->prev = 0;
			return;
		}
		r->tail = head - target;
		r->prev = ring[(r->tail - 1u) & mask];
		r->frac = 0;
	} else {
		uint32_t fill = head - r->tail;
		if (fill < resample_need(r, n) || fill > 2u * target) {
			r->tail = head - target;
			r->prev = ring[(r->tail - 1u) & mask];
			r->slips++;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		if (r->tail == head) {
			out[i] = r->prev;	// dry: hold until the next block's check
			r->holds++;
			continue;
		}
		int32_t a = r->prev, b = ring[r->tail & mask];
		out[i] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)r->frac) >> 16));
		r->frac += r->step;
		while (r->frac >= 0x10000u && r->tail != head) {
			r->frac -= 0x10000u;
			r->prev = ring[r->tail & mask];
			r->tail++;
		}
		if (r->frac >= 0x10000u) {
			r->frac = 0xFFFFu;	// dry mid-step: wait at the last sample
		}
	}
}

#endif

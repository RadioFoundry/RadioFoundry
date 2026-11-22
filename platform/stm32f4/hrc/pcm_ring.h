// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A single producer, single consumer ring of PCM samples between the audio task (whole blocks in) and the prompt FIFO
// feeder (chunks out), for i2s_pcm.c, and the choice of chip it feeds. Pure, so the host tests run it
// (tests/host/promptFifoTest.c).
//
// The indices run free and wrap at 2^32; the size is a power of two, so index & mask stays continuous across the wrap.
// Each side writes only its own index, after the samples (producer) or after reading them (consumer). Both stay
// multiples of PCM_RING_CHUNK (zero after pcm_ring_reset, whole blocks in), so a chunk never straddles the end of the array.
#ifndef PCM_RING_H
#define PCM_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PCM_RING_SAMPLES	512u	// 64 ms at 8 kHz: three 20 ms blocks fit
#define PCM_RING_CHUNK		32u	// what the feeder takes at a time (the prompt FIFO's write)
#define PCM_RING_MASK		(PCM_RING_SAMPLES - 1u)
_Static_assert((PCM_RING_SAMPLES & PCM_RING_MASK) == 0, "the ring size is a power of two");
_Static_assert(PCM_RING_SAMPLES % PCM_RING_CHUNK == 0, "a chunk never wraps");

typedef struct {
	int16_t s[PCM_RING_SAMPLES];
	volatile uint32_t head;		// producer: samples written
	volatile uint32_t tail;		// consumer: samples taken
} pcm_ring_t;

// Empty, both indices at 0, with the consumer stopped (from the producer's side). Not tail = head: a ring from the
// heap starts with garbage indices, and one off a chunk boundary makes chunks run past the end of s (the feeder then
// played the memory after the ring, and the producer wrote into it)
static inline void pcm_ring_reset(pcm_ring_t *r)
{
	r->head = 0;
	r->tail = 0;
}

static inline uint32_t pcm_ring_level(const pcm_ring_t *r)
{
	return r->head - r->tail;
}

static inline bool pcm_ring_fits(const pcm_ring_t *r, uint32_t n)
{
	return PCM_RING_SAMPLES - pcm_ring_level(r) >= n;
}

// n samples in (a whole block; a multiple of PCM_RING_CHUNK keeps every chunk in one piece), x NULL: silence. False: no
// room, nothing written
static inline bool pcm_ring_push(pcm_ring_t *r, const int16_t *x, uint32_t n)
{
	if (n > PCM_RING_SAMPLES || !pcm_ring_fits(r, n)) {
		return false;
	}
	uint32_t at = r->head & PCM_RING_MASK;
	uint32_t first = PCM_RING_SAMPLES - at < n ? PCM_RING_SAMPLES - at : n;
	if (x != NULL) {
		memcpy(&r->s[at], x, first * sizeof x[0]);
		memcpy(&r->s[0], x + first, (n - first) * sizeof x[0]);
	} else {
		memset(&r->s[at], 0, first * sizeof r->s[0]);
		memset(&r->s[0], 0, (n - first) * sizeof r->s[0]);
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);	// the samples before the index that shows them
	r->head += n;
	return true;
}

// One chunk out. False: less than a chunk was ready and out holds silence (an underrun)
static inline bool pcm_ring_pop(pcm_ring_t *r, int16_t out[PCM_RING_CHUNK])
{
	if (pcm_ring_level(r) < PCM_RING_CHUNK) {
		memset(out, 0, PCM_RING_CHUNK * sizeof out[0]);
		return false;
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	memcpy(out, &r->s[r->tail & PCM_RING_MASK], PCM_RING_CHUNK * sizeof out[0]);
	__atomic_thread_fence(__ATOMIC_RELEASE);	// read before the producer may reuse the space
	r->tail += PCM_RING_CHUNK;
	return true;
}

// The chip whose prompt FIFO the speaker blocks go to, NULL: none. The FM copy's chip (copy) takes every block, as
// a receive through the copy (NXDN) plays its voice there. A chip in FM receive (fm) takes them only while the speaker's
// PCM route is on (spk, a beep): FM audio and PCM take turns on the line out, and the FM audio stays untouched
static inline void *pcm_fifo_chip(void *copy, void *fm, bool spk)
{
	if (copy != NULL) {
		return copy;
	}
	return spk ? fm : NULL;
}

#endif

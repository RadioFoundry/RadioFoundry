// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A transmit tap's sample queue (hal_baseband_t tx_write, docs/spec/air-interface.md 6.2): a task writes samples, the
// board's interrupt that feeds the modulator pulls them. Single producer, single consumer; when it runs dry the last
// sample is held, as hal_baseband.h asks, so a late writer costs a flat stretch rather than a step to zero. Header
// only and allocation free: the board places it.
#ifndef RFOUNDRY_AIR_TX_RING_H
#define RFOUNDRY_AIR_TX_RING_H

#include <stdint.h>

// 64 ms at 8 kHz: the 60 ms or more hal_baseband.h asks a board to buffer. A power of two
#define AIR_TX_RING	512u

typedef struct {
	int16_t s[AIR_TX_RING];
	uint16_t head;		// the writer's
	uint16_t tail;		// the reader's
	int16_t last;		// the reader's: the sample held when dry
	uint32_t dry;		// samples the reader held (since the reset)
} air_tx_ring_t;

static inline void air_tx_ring_reset(air_tx_ring_t *r)
{
	r->head = r->tail = 0;
	r->last = 0;
	r->dry = 0;
}

// What a writer may queue now
static inline uint16_t air_tx_ring_space(const air_tx_ring_t *r)
{
	uint16_t h = r->head, t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
	return (uint16_t)(AIR_TX_RING - (uint16_t)(h - t));
}

// Task: up to n samples queued; returns how many were taken
static inline uint16_t air_tx_ring_write(air_tx_ring_t *r, const int16_t *buf, uint16_t n)
{
	uint16_t room = air_tx_ring_space(r), h = r->head;
	if (n > room)
		n = room;
	for (uint16_t i = 0; i < n; i++)
		r->s[(uint16_t)(h + i) & (AIR_TX_RING - 1u)] = buf[i];
	__atomic_store_n(&r->head, (uint16_t)(h + n), __ATOMIC_RELEASE);
	return n;
}

// The board's interrupt: n samples out, the last one held for those the writer hasn't queued
static inline void air_tx_ring_pull(air_tx_ring_t *r, int16_t *out, uint16_t n)
{
	uint16_t t = r->tail, have = (uint16_t)(__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - t);
	for (uint16_t i = 0; i < n; i++) {
		if (have) {
			r->last = r->s[t++ & (AIR_TX_RING - 1u)];
			have--;
		} else {
			r->dry++;
		}
		out[i] = r->last;
	}
	__atomic_store_n(&r->tail, t, __ATOMIC_RELEASE);
}

#endif

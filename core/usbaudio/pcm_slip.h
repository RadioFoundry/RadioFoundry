// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * A PCM jitter buffer between two free-running 8 kHz clocks (docs/spec/usb-audio.md 5.3): audio the host produces at
 * its clock (USB audio OUT, USRP datagrams, serial frames) and the radio consumes at its codec's clock (the HR_C6000's
 * I2S word clock on the DM-1701), or the other way round. The clocks differ by up to a few hundred ppm, so a plain
 * FIFO slowly runs dry or full. This one primes to a target fill, then keeps the smoothed fill near it by dropping or
 * repeating one sample per pull when it drifts more than one block away: at 100 ppm that's one sample about every
 * 1.25 s, inaudible on voice and harmless for AFSK. Overflow drops the oldest samples (latency stays bounded), an
 * underrun plays silence and primes again. Single producer, single consumer, no locks needed for the counters each
 * side owns; the caller serializes push and pull if they run in different tasks. Host tested (tests/host/usrpTest.c);
 * not built into the firmware yet.
 */
#ifndef ODR_PCM_SLIP_H
#define ODR_PCM_SLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCM_SLIP_CAP	1024u	// samples (128 ms at 8 kHz), power of two

typedef struct {
	uint32_t pushed;	// samples taken in
	uint32_t pulled;	// samples handed out (silence included)
	uint32_t overflow;	// samples dropped because the buffer was full
	uint32_t underruns;	// pulls that ran dry while playing
	uint32_t drops;		// samples dropped to catch up (producer fast)
	uint32_t repeats;	// samples repeated to wait (producer slow)
	uint32_t primes;	// times playback (re)started
} pcm_slip_stats_t;

typedef struct {
	int16_t buf[PCM_SLIP_CAP];
	uint32_t wr, rd;	// free-running sample counters
	uint16_t target;	// fill to prime to and hold, samples
	uint16_t block;		// the usual pull size, samples (the hysteresis)
	bool primed;
	int32_t avg_x16;	// smoothed fill at pull time, x16
	pcm_slip_stats_t stats;
} pcm_slip_t;

// target: latency to hold (e.g. 480 = 60 ms); block: the consumer's pull size (e.g. 160)
void pcm_slip_init(pcm_slip_t *p, uint16_t target, uint16_t block);
// Empty it and wait to prime again (end of a transmission)
void pcm_slip_reset(pcm_slip_t *p);
uint32_t pcm_slip_fill(const pcm_slip_t *p);
void pcm_slip_push(pcm_slip_t *p, const int16_t *pcm, size_t n);
// n samples out; false when they are (partly) silence because it isn't primed or ran dry
bool pcm_slip_pull(pcm_slip_t *p, int16_t *out, size_t n);

#endif

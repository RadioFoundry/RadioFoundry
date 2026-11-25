// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Microphone gain with a peak limiter, for the PCM a vocoder's encoder is given (DMR's AMBE+2 today; the gain is
// radio_mic_pcm_q8(), core/radio/mic_gain.h; docs/spec/radio-features.md 3.6).
//
// Why a limiter and not plain saturation: the gain exists because a board's microphone can reach the encoder about
// 13 dB under a commercial radio's level (one radio: -31 dBFS of loud speech against another maker's handheld's -18),
// and lifting that by 12 dB puts a shout's peaks past full scale. Clipped peaks reach the encoder as a harsh
// square edge it codes as a buzz; a gain that steps down for that block codes as the same speech a little quieter.
//
// One gain for a whole block, never more than the set gain and never so much that the block's largest sample passes
// MICLIMIT_CEILING. It falls at once (the block that needs less gets less: no look-ahead, so the step falls on a
// block edge) and recovers by MICLIMIT_RELEASE_Q8 a block, ramped sample by sample through the block so the rise has
// no step. The ramp ends at a gain the block's own peak allows, so no sample in it passes the ceiling either. With the
// set gain at or under unity and the input under the ceiling it is a plain multiply.
//
// Header only and allocation free: an audio callback runs it on its own blocks.
#ifndef MICLIMIT_H
#define MICLIMIT_H

#include <stdint.h>

#define MICLIMIT_UNITY		256u	// Q8
#define MICLIMIT_CEILING	28000	// the largest sample out, about -1.4 dBFS: a little room under the encoder's clip
// The recovery a block, Q8 of the gain: 271/256 is +0.49 dB, so a 12 dB cut recovers in about 25 blocks (0.5 s at
// 20 ms): slow enough that a syllable after a loud one isn't pumped up between them
#define MICLIMIT_RELEASE_Q8	271u
// dB range of miclimit_db_q8: wide enough for a board's trim plus the setting's -10..+10
#define MICLIMIT_DB_MIN		(-12)
#define MICLIMIT_DB_MAX		24

typedef struct {
	uint16_t gain_q8;	// the gain at the end of the last block
	uint16_t low_q8;	// the lowest gain since the reset: how hard the limiter worked in this transmission
	uint16_t peak_in;	// the largest input sample (absolute) since the reset
	uint16_t peak_out;	// the largest output sample (absolute) since the reset: never over MICLIMIT_CEILING
	uint32_t limited;	// blocks given less than the set gain since the reset
} miclimit_t;

// 256 x 10^(db/20), rounded, for db in MICLIMIT_DB_MIN..MAX (clamped): the gain of a dB setting
static inline uint16_t miclimit_db_q8(int db)
{
	static const uint16_t q8[MICLIMIT_DB_MAX - MICLIMIT_DB_MIN + 1] = {
		64, 72, 81, 91, 102, 114, 128, 144, 162, 181, 203, 228,			// -12..-1
		256, 287, 322, 362, 406, 455, 511, 573, 643, 722, 810, 908,		// 0..11
		1019, 1144, 1283, 1440, 1615, 1812, 2033, 2282, 2560, 2872, 3223, 3616,	// 12..23
		4057,									// 24
	};
	if (db < MICLIMIT_DB_MIN) {
		db = MICLIMIT_DB_MIN;
	} else if (db > MICLIMIT_DB_MAX) {
		db = MICLIMIT_DB_MAX;
	}
	return q8[db - MICLIMIT_DB_MIN];
}

// The nearest whole dB to a Q8 gain within the table's range (for the debug link: the limiter's lowest gain)
static inline int miclimit_q8_db(uint16_t q8)
{
	int best = MICLIMIT_DB_MIN;
	uint32_t best_err = 0xFFFFFFFFu;
	for (int db = MICLIMIT_DB_MIN; db <= MICLIMIT_DB_MAX; db++) {
		uint16_t g = miclimit_db_q8(db);
		uint32_t err = g > q8 ? (uint32_t)(g - q8) * 256u / g : (uint32_t)(q8 - g) * 256u / q8;	// relative
		if (err < best_err) {
			best_err = err;
			best = db;
		}
	}
	return best;
}

// A new transmission: starts at the set gain, the statistics cleared
static inline void miclimit_reset(miclimit_t *l, uint16_t set_q8)
{
	l->gain_q8 = set_q8;
	l->low_q8 = set_q8;
	l->peak_in = 0;
	l->peak_out = 0;
	l->limited = 0;
}

// out[i] = in[i] x the block's gain / 256 (in and out may be the same buffer). set_q8 is the gain wanted; the
// gain used never passes it, nor what the block's peak allows. Returns the gain at the block's end
static inline uint16_t miclimit_block(miclimit_t *l, uint16_t set_q8, const int16_t *in, int16_t *out, uint16_t n)
{
	uint32_t peak = 0;
	for (uint16_t i = 0; i < n; i++) {
		int32_t v = in[i];
		uint32_t a = (uint32_t)(v < 0 ? -v : v);
		if (a > peak) {
			peak = a;
		}
	}
	if (peak > l->peak_in) {
		l->peak_in = (uint16_t)(peak > 0xFFFFu ? 0xFFFFu : peak);
	}
	uint32_t prev = l->gain_q8;
	uint32_t target = (prev * MICLIMIT_RELEASE_Q8) / 256u;	// the most it may recover by in this block
	if (target <= prev) {
		target = prev + 1u;	// a gain too small for the ratio to move it still recovers
	}
	if (target > set_q8) {
		target = set_q8;
	}
	if (peak > 0) {
		uint32_t allowed = ((uint32_t)MICLIMIT_CEILING * 256u) / peak;	// rounded down: peak x allowed / 256 <= ceiling
		if (target > allowed) {
			target = allowed;
		}
	}
	uint32_t out_peak = 0;
	if (target < prev || n == 0) {
		// Falling (or the set gain lowered): the whole block at the new gain, which its peak allows
		for (uint16_t i = 0; i < n; i++) {
			int32_t v = ((int32_t)in[i] * (int32_t)target) / 256;
			uint32_t a = (uint32_t)(v < 0 ? -v : v);
			out_peak = a > out_peak ? a : out_peak;
			out[i] = (int16_t)v;
		}
	} else {
		// Rising or steady: prev to target across the block; every sample's gain is at most target
		int32_t span = (int32_t)target - (int32_t)prev;
		for (uint16_t i = 0; i < n; i++) {
			int32_t g = (int32_t)prev + (span * (int32_t)(i + 1u)) / (int32_t)n;
			int32_t v = ((int32_t)in[i] * g) / 256;
			uint32_t a = (uint32_t)(v < 0 ? -v : v);
			out_peak = a > out_peak ? a : out_peak;
			out[i] = (int16_t)v;
		}
	}
	if (out_peak > l->peak_out) {
		l->peak_out = (uint16_t)out_peak;
	}
	if (target < set_q8) {
		l->limited++;
	}
	if (target < l->low_q8) {
		l->low_q8 = (uint16_t)target;
	}
	l->gain_q8 = (uint16_t)target;
	return (uint16_t)target;
}

#endif

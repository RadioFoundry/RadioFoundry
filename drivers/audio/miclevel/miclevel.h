// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Microphone level for VOX (hal_power_t.mic_level, docs/spec/radio-features.md 3.4). miclevel_log() is the hal's
// scale for every board: a mic level tap on a 12-bit ADC passes its reading straight in. Radios whose microphone
// reaches the CPU only as PCM (HR_C7000: the codec ADC's 10 ms blocks, modem-hrc7000.md 3) use the block functions.
//
// The level is the hal's scale: 0..100, 100 / 12 steps per bit (about 0.72 dB per step), 100 = a full-scale 16-bit
// peak or 12-bit reading, 0 = 72 dB below it or silence. The peak of a block is measured from the block's mean, so a DC offset in the
// ADC doesn't read as sound. The level follows a louder block at once and falls by MICLEVEL_DECAY steps per block, so a
// reader polling slower than the blocks arrive still sees a syllable's peak.
//
// Header only and allocation free, so miclevel_block() can run in a PCM interrupt.
#ifndef MICLEVEL_H
#define MICLEVEL_H

#include <stdint.h>

#define MICLEVEL_DECAY		4u	// steps per block: about 3 dB per 10 ms block, 100 to 0 in 250 ms
#define MICLEVEL_PEAK_SHIFT	3u	// a 15-bit magnitude to the 12 bits the scale spans

typedef struct {
	volatile uint8_t level;		// 0..100, the envelope
	volatile uint16_t peak;		// the last block's peak from its mean, 0..32767 (diagnostics)
	volatile uint32_t blocks;	// blocks measured (diagnostics)
} miclevel_t;

// 0..100 for x over `bits` bits: 100 x log2(x) / bits, from an integer log2 (msb plus a linear mantissa, within 0.1
// bit). x = 0 gives 0
static inline uint8_t miclevel_log(uint32_t x, uint8_t bits)
{
	if (x == 0 || bits == 0) {
		return 0;
	}
	uint32_t msb = 31u - (uint32_t)__builtin_clz(x);
	uint32_t mant = msb >= 8u ? (x >> (msb - 8u)) : (x << (8u - msb));	// 256..511
	uint32_t log2_q8 = msb * 256u + mant - 256u;
	uint32_t level = log2_q8 * 100u / ((uint32_t)bits * 256u);
	return (uint8_t)(level > 100u ? 100u : level);
}

// The largest distance of a sample from the block's mean, 0..32767 (n = 0: 0)
static inline uint16_t miclevel_peak(const volatile int16_t *pcm, uint32_t n)
{
	if (n == 0) {
		return 0;
	}
	int32_t sum = 0, lo = INT16_MAX, hi = INT16_MIN;
	for (uint32_t i = 0; i < n; i++) {
		int32_t s = pcm[i];
		sum += s;
		lo = s < lo ? s : lo;
		hi = s > hi ? s : hi;
	}
	int32_t mean = sum / (int32_t)n;
	int32_t pk = hi - mean > mean - lo ? hi - mean : mean - lo;
	return (uint16_t)(pk > INT16_MAX ? INT16_MAX : pk);
}

// Level of one block's peak, on the hal's scale
static inline uint8_t miclevel_of_peak(uint16_t peak)
{
	return miclevel_log((uint32_t)peak >> MICLEVEL_PEAK_SHIFT, 12);
}

static inline void miclevel_reset(miclevel_t *m)
{
	m->level = 0;
	m->peak = 0;
}

// One PCM block in: the envelope rises to its level or falls by MICLEVEL_DECAY towards it
static inline void miclevel_block(miclevel_t *m, const volatile int16_t *pcm, uint32_t n)
{
	uint16_t pk = miclevel_peak(pcm, n);
	uint8_t now = miclevel_of_peak(pk), env = m->level;
	if (now >= env) {
		env = now;
	} else {
		env = (uint32_t)(env - now) > MICLEVEL_DECAY ? (uint8_t)(env - MICLEVEL_DECAY) : now;
	}
	m->level = env;
	m->peak = pk;
	m->blocks++;
}

#endif

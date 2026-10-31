// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV: the samples tone_d578uv.c writes to PB9's PWM, one per sample interrupt (docs/spec/hardware-d578uv.md
// 5.3). Pure, so the host tests run them through the core's AFSK demodulator; no division per sample, as the
// interrupt runs at about 26 kHz. Phases are 16 bits, the sine a quarter table of 0..255.
#ifndef D578UV_TONE_SYNTH_H
#define D578UV_TONE_SYNTH_H

#include <stdbool.h>
#include <stdint.h>
#include "aprs/hdlc.h"

// A quarter sine, 0..255 over 0..90 degrees in 64 steps
static const uint8_t tone_quarter[65] = {
	0, 6, 13, 19, 25, 31, 37, 44, 50, 56, 62, 68, 74, 80, 86, 92, 98, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152,
	157, 162, 167, 171, 176, 180, 185, 189, 193, 197, 201, 205, 208, 212, 215, 219, 222, 225, 228, 231, 233, 236, 238,
	240, 242, 244, 246, 247, 249, 250, 251, 252, 253, 254, 254, 255, 255, 255,
};

// Sine of the phase's top 8 bits times amp, amp / 255 units: -amp..amp
static inline int32_t tone_sine(uint16_t phase, int32_t amp)
{
	uint8_t i = (uint8_t)(phase >> 8);
	uint8_t j = i & 63u;
	int32_t v = (i & 64u) ? tone_quarter[64u - j] : tone_quarter[j];
	v = v * amp / 255;
	return (i & 128u) ? -v : v;
}

// Phase step per sample for hz, a sample every sample_clocks of a clock_hz clock: hz x 65536 / sample rate
static inline uint16_t tone_step(uint32_t hz, uint32_t sample_clocks, uint32_t clock_hz)
{
	return (uint16_t)(((uint64_t)hz * 65536u * sample_clocks + clock_hz / 2u) / clock_hz);
}

// --- AFSK: one tone per symbol, phase continuous. tones as hal_afsk.h gives them (LSB first, 1 = mark). A symbol
// lasts clock_hz / baud clocks; each sample adds sample_clocks, so the symbols keep the baud rate on average though a
// symbol is not a whole number of samples (21.8 at 26,132 Hz) ---
typedef struct {
	const uint8_t *tones;
	uint16_t nbits, bit;
	uint32_t clocks, symbol_clocks, sample_clocks;
	uint16_t phase, step, step_mark, step_space;
	uint8_t amp;		// peak, in duty counts (at most 127)
} afsk_tx_t;

static inline void afsk_tx_init(afsk_tx_t *t, const uint8_t *tones, uint16_t nbits, uint16_t baud, uint16_t mark_hz,
				uint16_t space_hz, uint8_t amp, uint32_t sample_clocks, uint32_t clock_hz)
{
	t->tones = tones;
	t->nbits = nbits;
	t->bit = 0;
	t->clocks = 0;
	t->symbol_clocks = clock_hz / baud;
	t->sample_clocks = sample_clocks;
	t->phase = 0;
	t->step_mark = tone_step(mark_hz, sample_clocks, clock_hz);
	t->step_space = tone_step(space_hz, sample_clocks, clock_hz);
	t->step = nbits != 0 && hdlc_tone_bit(tones, 0) ? t->step_mark : t->step_space;
	t->amp = amp > 127u ? 127u : amp;
}

// The next PWM duty (0..255, 128 the center), or -1 once every symbol is out
static inline int32_t afsk_tx_next(afsk_tx_t *t)
{
	if (t->bit >= t->nbits) {
		return -1;
	}
	t->phase = (uint16_t)(t->phase + t->step);
	int32_t duty = 128 + tone_sine(t->phase, t->amp);
	t->clocks += t->sample_clocks;
	if (t->clocks >= t->symbol_clocks) {
		t->clocks -= t->symbol_clocks;
		if (++t->bit < t->nbits) {
			t->step = hdlc_tone_bit(t->tones, t->bit) ? t->step_mark : t->step_space;
		}
	}
	return duty;
}

#endif

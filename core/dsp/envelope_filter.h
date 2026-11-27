// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Linear voice filtering at 8 kHz. No level detector or gate: weak samples receive the same response as strong ones.
#ifndef RFOUNDRY_ENVELOPE_FILTER_H
#define RFOUNDRY_ENVELOPE_FILTER_H
#include <stdint.h>

typedef struct { int32_t x1, x2, y1, y2; } envelope_section_t;
typedef struct { envelope_section_t section[3]; } envelope_filter_t;

static inline int16_t envelope_filter_sample(envelope_filter_t *f, int16_t sample)
{
	// Q28 normalized biquads: Butterworth high-pass 200 Hz (order 2), low-pass 2400 Hz (order 4).
	// States retain eight fractional bits so low-level voice is not rounded away between sections.
	static const int32_t c[3][5] = {
		{ 240211778, -480423556, 240211778, -477447832, 214963824 },
		{ 93520452, 187040904, 93520452, 88308736, 17337617 },
		{ 128811780, 257623559, 128811780, 121633345, 125178317 }
	};
	int32_t x = (int32_t)sample * 256;
	for (unsigned i = 0; i < 3; i++) {
		envelope_section_t *s = &f->section[i];
		int64_t a = (int64_t)c[i][0] * x + (int64_t)c[i][1] * s->x1 + (int64_t)c[i][2] * s->x2
			  - (int64_t)c[i][3] * s->y1 - (int64_t)c[i][4] * s->y2;
		int32_t y = (int32_t)(a / 268435456);
		s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
		x = y;
	}
	int32_t out = x / 128; // nominal 2x voice gain
	return (int16_t)(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
}
#endif

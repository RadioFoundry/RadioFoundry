// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CTCSS and DCS generators (subaudio_tx.h)
#include "subaudio_tx.h"
#include "subaudio.h"

#define DCS_BITS	23u
#define DCS_MASK	0x7FFFFFu
#define RAMP_MS		20u
#define SMOOTH_SHIFT	2	// DCS level smoothing: one pole, 1/4 per sample

// sin(x) for x = 0..pi/2 in 64 steps, Q15
static const int16_t quarter[65] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
	12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
	23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
	30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
	32767,
};

// Q15 sine of a 32-bit phase, with linear interpolation between the table's points
static int32_t sine_q15(uint32_t ph)
{
	uint32_t quad = ph >> 30;
	uint32_t pos = (ph >> 8) & 0x3FFFFFu;	// 22 bits within the quadrant
	if (quad & 1u) {
		pos = 0x400000u - pos;
	}
	uint32_t idx = pos >> 16, frac = pos & 0xFFFFu;
	int32_t v = quarter[idx];
	if (idx < 64u) {
		v += (int32_t)(((int64_t)(quarter[idx + 1] - quarter[idx]) * (int64_t)frac) >> 16);
	}
	return quad & 2u ? -v : v;
}

void subaudio_tx_init(subaudio_tx_t *t, uint32_t rate, uint16_t full, uint8_t percent)
{
	*t = (subaudio_tx_t){ 0 };
	t->rate = rate;
	t->full = full;
	t->mid = full / 2;
	if (percent > 100u) {
		percent = 100u;
	}
	t->peak = (int32_t)(full / 2u) * percent / 100;
}

void subaudio_tx_start(subaudio_tx_t *t, subaudio_kind_t kind, uint16_t value, bool invert)
{
	t->kind = kind;
	t->tail = false;
	t->phase = 0;
	t->bit = 0;
	t->lp = 0;
	t->gain = 0;
	t->gain_step = (int32_t)(32768u / (t->rate * RAMP_MS / 1000u + 1u)) + 1;
	if (kind == SUBAUDIO_CTCSS) {
		t->step = (uint32_t)(((uint64_t)value << 32) / ((uint64_t)t->rate * 10u));
	} else if (kind == SUBAUDIO_DCS || kind == SUBAUDIO_DCS_INV) {
		t->step = (uint32_t)((1344ull << 32) / ((uint64_t)t->rate * 10u));
		uint32_t w = subaudio_dcs_word(value & 0x1FFu);
		t->word = ((kind == SUBAUDIO_DCS_INV) != invert) ? ~w & DCS_MASK : w;
	}
}

bool subaudio_tx_tail(subaudio_tx_t *t, bool on)
{
	if (t->kind != SUBAUDIO_DCS && t->kind != SUBAUDIO_DCS_INV) {
		return false;
	}
	t->tail = on;
	t->phase = 0;		// the tail's sine, or the code's next bit, starts clean
	t->lp = 0;
	return true;
}

uint16_t subaudio_tx_next(subaudio_tx_t *t)
{
	int32_t level = 0;	// Q15 of the peak
	if (t->tail) {
		level = sine_q15(t->phase);	// step is the bit rate, 134.4 Hz
		t->phase += t->step;
	} else if (t->kind == SUBAUDIO_CTCSS) {
		level = sine_q15(t->phase);
		t->phase += t->step;
	} else if (t->kind == SUBAUDIO_DCS || t->kind == SUBAUDIO_DCS_INV) {
		int32_t target = (t->word >> t->bit) & 1u ? 32767 : -32767;
		t->lp += (target - t->lp) >> SMOOTH_SHIFT;
		level = t->lp;
		uint32_t prev = t->phase;
		t->phase += t->step;
		if (t->phase < prev && ++t->bit >= DCS_BITS) {
			t->bit = 0;
		}
	}
	if (t->gain < 32768) {
		t->gain += t->gain_step;
		if (t->gain > 32768) {
			t->gain = 32768;
		}
	}
	level = (int32_t)(((int64_t)level * t->gain) >> 15);
	int32_t duty = t->mid + (int32_t)(((int64_t)level * t->peak) >> 15);
	if (duty < 0) {
		duty = 0;
	}
	return (uint16_t)(duty > t->full ? t->full : duty);
}

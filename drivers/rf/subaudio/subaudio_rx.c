// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Software CTCSS and DCS detection (subaudio_rx.h)
#include <string.h>
#include "subaudio_rx.h"
#include "subaudio.h"

#define GUARD_HZ	3.0f	// guard bins either side of the tone
#define GUARD_RATIO	1.25f	// the tone bin must beat each guard bin by this much (power) to open
#define ON_SHARE	0.30f	// share of the window's AC power in the tone to open
#define OFF_SHARE	0.12f	// ... to stay open
#define ON_COUNT	2	// good results in a row to open
#define OFF_COUNT	3	// bad results in a row to close
#define WINDOW_S	0.2f
#define HP_SHIFT	256.0f	// tone path DC tracker time constant in samples

#define DCS_BAUD	134.4f
#define DCS_BITS	23
#define DCS_MASK	0x7FFFFFu
#define DCS_TOLERANCE	2	// bit errors accepted in a word
#define DCS_LOCK_GAIN	0.15f
#define DCS_PEAK_DECAY	512.0f	// slicer level tracker time constant in samples
#define DCS_MISSING	3	// words without a match before closing

// cos(x) for 0 <= x <= 0.7, Taylor series (error under 1e-9): no libm
static float cos_small(float x)
{
	float x2 = x * x;
	return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f * (1.0f - x2 / 56.0f * (1.0f - x2 / 90.0f))));
}

static void window_reset(subaudio_rx_t *d, int w)
{
	memset(&d->win[w], 0, sizeof(d->win[w]));
}

void subaudio_rx_init(subaudio_rx_t *d, uint32_t fs_hz, float min_amp)
{
	memset(d, 0, sizeof(*d));
	d->fs = (float)fs_hz;
	d->min_amp = min_amp > 0.0f ? min_amp : 8.0f;
}

static int popcount(uint32_t x)
{
	int n = 0;
	for (; x; x &= x - 1) {
		n++;
	}
	return n;
}

void subaudio_rx_set(subaudio_rx_t *d, subaudio_kind_t kind, uint16_t value, bool invert)
{
	float fs = d->fs, min_amp = d->min_amp;
	memset(d, 0, sizeof(*d));
	d->fs = fs;
	d->min_amp = min_amp;
	d->invert = invert;
	d->kind = kind;
	if (kind == SUBAUDIO_CTCSS) {
		float f = (float)value / 10.0f;
		const float freq[3] = { f, f - GUARD_HZ, f + GUARD_HZ };
		for (int i = 0; i < 3; i++) {
			d->coef[i] = 2.0f * cos_small(6.2831853f * freq[i] / fs);
		}
		d->win_len = (uint16_t)(WINDOW_S * fs);
		d->delay_b = d->win_len / 2;
		d->win[1].n = (uint16_t)(0u - d->delay_b);	// B waits: counts up through zero (see feed)
	} else if (kind == SUBAUDIO_DCS || kind == SUBAUDIO_DCS_INV) {
		d->bit_step = DCS_BAUD / fs;
		uint32_t w = subaudio_dcs_word(value & 0x1FFu);
		// An inverted code is the same bits inverted on the wire; a path that inverts flips it once more
		d->target = ((kind == SUBAUDIO_DCS_INV) != invert) ? ~w & DCS_MASK : w;
		d->since_match = 0xFFFF;
	}
}

// Close out a window: one decision per window
static void window_done(subaudio_rx_t *d, int w)
{
	float n = (float)d->win_len;
	float p[3];
	for (int i = 0; i < 3; i++) {
		p[i] = d->win[w].s1[i] * d->win[w].s1[i] + d->win[w].s2[i] * d->win[w].s2[i] -
		       d->coef[i] * d->win[w].s1[i] * d->win[w].s2[i];
	}
	float ac = d->win[w].sumsq;	// the input is already high-passed
	float amp2 = 4.0f * p[0] / (n * n);
	float share = ac > 0.0f ? (2.0f * p[0] / n) / ac : 0.0f;
	float guard = p[1] > p[2] ? p[1] : p[2];
	bool loud = amp2 >= d->min_amp * d->min_amp;
	bool good = loud && share >= ON_SHARE && p[0] >= GUARD_RATIO * guard;
	bool keep = loud && share >= OFF_SHARE && p[0] >= guard;
	if (d->tone_on ? keep : good) {
		d->bad = 0;
		if (d->good < 255) {
			d->good++;
		}
		if (d->good >= ON_COUNT) {
			d->tone_on = true;
		}
	} else {
		d->good = 0;
		if (d->bad < 255) {
			d->bad++;
		}
		if (d->bad >= OFF_COUNT) {
			d->tone_on = false;
		}
	}
	window_reset(d, w);
}

static void feed_ctcss(subaudio_rx_t *d, float x)
{
	d->dc += (x - d->dc) / HP_SHIFT;
	float y = x - d->dc;
	for (int w = 0; w < 2; w++) {
		if ((int16_t)d->win[w].n < 0) {	// window B not started yet
			d->win[w].n++;
			continue;
		}
		for (int i = 0; i < 3; i++) {
			float s = y + d->coef[i] * d->win[w].s1[i] - d->win[w].s2[i];
			d->win[w].s2[i] = d->win[w].s1[i];
			d->win[w].s1[i] = s;
		}
		d->win[w].sumsq += y * y;
		if (++d->win[w].n >= d->win_len) {
			window_done(d, w);
		}
	}
}

static void feed_dcs(subaudio_rx_t *d, float x)
{
	// Slicer level: midway between the tracked peaks
	if (!d->primed) {
		d->primed = true;
		d->hi = d->lo = x;
	}
	if (x > d->hi) {
		d->hi = x;
	} else {
		d->hi += (x - d->hi) / DCS_PEAK_DECAY;
	}
	if (x < d->lo) {
		d->lo = x;
	} else {
		d->lo += (x - d->lo) / DCS_PEAK_DECAY;
	}
	if (d->hi - d->lo < 2.0f * d->min_amp) {
		d->words = 0;
		d->dcs_on = false;
		return;
	}
	bool bit = x > (d->hi + d->lo) / 2.0f;

	// Clock recovery: an edge should fall on the bit boundary (phase 0)
	if (bit != d->last_bit) {
		float err = d->bit_phase < 0.5f ? d->bit_phase : d->bit_phase - 1.0f;
		d->bit_phase -= DCS_LOCK_GAIN * err;
	}
	d->last_bit = bit;

	d->bit_phase += d->bit_step;
	if (d->bit_phase >= 1.0f) {
		d->bit_phase -= 1.0f;
		d->bit_sampled = false;
	} else if (d->bit_phase < 0.0f) {
		d->bit_phase += 1.0f;
	}
	if (d->bit_phase >= 0.5f && !d->bit_sampled) {
		d->bit_sampled = true;
		d->reg = (d->reg >> 1) | ((uint32_t)bit << (DCS_BITS - 1));
		if (d->since_match < 0xFFFF) {
			d->since_match++;
		}
		if (popcount((d->reg ^ d->target) & DCS_MASK) <= DCS_TOLERANCE) {
			// A word every 23 bits; a bit of slip is tolerated
			if (d->since_match >= DCS_BITS - 1 && d->since_match <= DCS_BITS + 1) {
				if (d->words < 255) {
					d->words++;
				}
			} else if (d->since_match > DCS_BITS + 1) {
				d->words = 1;
			}
			d->since_match = 0;
			d->dcs_on = d->words >= 2;
		} else if (d->since_match > DCS_MISSING * DCS_BITS && d->since_match != 0xFFFF) {
			d->words = 0;
			d->dcs_on = false;
			d->since_match = 0xFFFF;
		}
	}
}

void subaudio_rx_feed(subaudio_rx_t *d, int16_t sample)
{
	if (d->kind == SUBAUDIO_CTCSS) {
		feed_ctcss(d, (float)sample);
	} else if (d->kind == SUBAUDIO_DCS || d->kind == SUBAUDIO_DCS_INV) {
		feed_dcs(d, (float)sample);
	}
}

bool subaudio_rx_match(const subaudio_rx_t *d)
{
	switch (d->kind) {
	case SUBAUDIO_CTCSS: return d->tone_on;
	case SUBAUDIO_DCS:
	case SUBAUDIO_DCS_INV: return d->dcs_on;
	default: return true;
	}
}

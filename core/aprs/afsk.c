// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Bell 202 AFSK on PCM (afsk.h)
#include <string.h>
#include "aprs/afsk.h"

// sin(2 pi i / 256) x 32767
static const int16_t sine[256] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
	12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
	23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
	30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
	32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
	30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
	23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
	12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
	0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
	-23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
	-30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
	-30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
	-23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

static uint32_t phase_inc(uint32_t hz, uint32_t rate)
{
	return (uint32_t)(((uint64_t)hz << 32) / rate);
}

void afsk_mod_init(afsk_mod_t *m, uint32_t rate)
{
	memset(m, 0, sizeof(*m));
	m->rate = rate;
}

size_t afsk_mod(afsk_mod_t *m, const uint8_t *tones, uint32_t nbits, int16_t amp, int16_t *out, size_t max)
{
	uint32_t inc_mark = phase_inc(AFSK_MARK_HZ, m->rate), inc_space = phase_inc(AFSK_SPACE_HZ, m->rate);
	size_t n = 0;
	while (n < max) {
		uint32_t bit = (uint32_t)(((uint64_t)m->sample * AFSK_BAUD) / m->rate);
		if (bit >= nbits) {
			break;
		}
		m->bit = bit;
		out[n++] = (int16_t)(((int32_t)amp * sine[m->phase >> 24]) / 32767);
		m->phase += hdlc_tone_bit(tones, bit) ? inc_mark : inc_space;
		m->sample++;
	}
	return n;
}

// Hysteresis on the normalized difference, in 1/256 (13 = 0.05)
#ifndef AFSK_HYST_Q8
#define AFSK_HYST_Q8		13
#endif
#ifndef AFSK_PLL_LOCKED
#define AFSK_PLL_LOCKED		12
#endif
#ifndef AFSK_PLL_SEARCH
#define AFSK_PLL_SEARCH		3
#endif
#define MIX_SHIFT		8	// sample x sine (Q15) >> 8: one bit's sum of 48 stays within int32

bool afsk_demod_init(afsk_demod_t *d, uint32_t rate, afsk_frame_fn fn, void *ctx)
{
	memset(d, 0, sizeof(*d));
	uint32_t win = (rate + AFSK_BAUD / 2u) / AFSK_BAUD;
	if (rate < 7200u || win > AFSK_WIN_MAX) {
		return false;
	}
	d->rate = rate;
	d->win = (uint16_t)win;
	d->inc_mark = phase_inc(AFSK_MARK_HZ, rate);
	d->inc_space = phase_inc(AFSK_SPACE_HZ, rate);
	d->pll_step = (int32_t)phase_inc(AFSK_BAUD, rate);
	// The peaks fall by 1/2^shift a sample: to 1/e in about 60 bits (the nearest power of two to 60 x win)
	uint32_t t = 60u * win;
	d->decay_shift = 0;
	while ((1u << (d->decay_shift + 1u)) <= t + t / 2u) {
		d->decay_shift++;
	}
	d->fn = fn;
	d->ctx = ctx;
	hdlc_dec_init(&d->hdlc);
	return true;
}

// |(i, q)| within about 3 %: max(M, 7/8 M + 1/2 m) of the larger and smaller part
static uint32_t mag(int32_t i, int32_t q)
{
	uint32_t a = (uint32_t)(i < 0 ? -i : i), b = (uint32_t)(q < 0 ? -q : q);
	uint32_t hi = a > b ? a : b, lo = a > b ? b : a;
	uint32_t m = hi - (hi >> 3) + (lo >> 1);
	return m > hi ? m : hi;
}

static uint32_t peak(uint32_t p, uint32_t m, uint8_t shift)
{
	p -= p >> shift;
	return m > p ? m : p;
}

static void sample(afsk_demod_t *d, int16_t s)
{
	int32_t x = s;
	int32_t v[4];
	v[0] = (x * sine[((d->ph_mark >> 24) + 64u) & 255u]) >> MIX_SHIFT;
	v[1] = (x * sine[d->ph_mark >> 24]) >> MIX_SHIFT;
	v[2] = (x * sine[((d->ph_space >> 24) + 64u) & 255u]) >> MIX_SHIFT;
	v[3] = (x * sine[d->ph_space >> 24]) >> MIX_SHIFT;
	d->ph_mark += d->inc_mark;
	d->ph_space += d->inc_space;
	for (int k = 0; k < 4; k++) {	// exact integer sums: no drift to correct
		d->sum[k] += v[k] - d->ring[k][d->pos];
		d->ring[k][d->pos] = v[k];
	}
	if (++d->pos >= d->win) {
		d->pos = 0;
	}
	uint32_t mm = mag(d->sum[0], d->sum[1]), ms = mag(d->sum[2], d->sum[3]);
	d->peak_mark = peak(d->peak_mark, mm, d->decay_shift);
	d->peak_space = peak(d->peak_space, ms, d->decay_shift);
	// mm / peak_mark - ms / peak_space against the hysteresis, cross multiplied (both peaks > 0 once audio arrives):
	// diff x pm x ps = mm x ps - ms x pm
	int64_t pm = d->peak_mark, ps = d->peak_space;
	int64_t diff = (int64_t)mm * ps - (int64_t)ms * pm;
	int64_t hyst = (int64_t)(((uint64_t)pm * (uint64_t)ps) >> 8) * AFSK_HYST_Q8;
	// a little hysteresis: noise near the crossing doesn't flip the tone back and forth (each flip moves the clock)
	bool mark = d->last_mark ? diff > -hyst : diff > hyst;

	// Clock recovery: the PLL counter wraps from +2^31 to -2^31 once a bit, which is where the bit is sampled (mid
	// bit); a tone change should come at 0, so each one pulls the counter a quarter of the way there
	int32_t prev = d->pll;
	d->pll = (int32_t)((uint32_t)d->pll + (uint32_t)d->pll_step);
	if (prev > 0 && d->pll < 0) {
		d->bits++;
		size_t n = hdlc_dec_bit(&d->hdlc, mark);
		if (n != 0 && d->fn != NULL) {
			d->fn(d->hdlc.buf, n, d->ctx);
		}
	}
	if (mark != d->last_mark) {
		// fast pull in on the preamble, gentle inside a frame where one noisy edge must not move the clock far
		d->pll -= d->hdlc.nbits > 16u ? d->pll / AFSK_PLL_LOCKED : d->pll / AFSK_PLL_SEARCH;
		d->last_mark = mark;
	}
}

void afsk_demod(afsk_demod_t *d, const int16_t *s, size_t n)
{
	d->samples += (uint32_t)n;
	while (n--) {
		sample(d, *s++);
	}
}

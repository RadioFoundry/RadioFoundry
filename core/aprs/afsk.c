// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
// The high passed path's corner, two first-order stages. A one-bit correlator is short and its passband wide, so
// audio under the tones (a receiver's hum, sub-audible tone residue) leaks into the mark correlator. A radio heard
// a flat transmitter with 320-400 Hz 1-10 dB under the tones and decoded about half the frames; high passed, all of
// them (BENCH, docs/spec/aprs.md 5). Alone the filter costs weak signals (its phase shift between the tones moves the
// edges), so the path as received runs beside it
#ifndef AFSK_HPF_HZ
#define AFSK_HPF_HZ		700
#endif
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
	// a = 1 / (1 + 2 pi fc / rate); 205887 = 2 pi in Q15
	d->hp_a = (int32_t)(((uint64_t)rate << 15) / (rate + (uint32_t)(((uint64_t)AFSK_HPF_HZ * 205887u) >> 15)));
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
	for (int p = 0; p < AFSK_PATHS; p++) {
		hdlc_dec_init(&d->path[p].hdlc);
	}
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

// A frame from path p: delivered unless the other path just delivered the same one (the same length and FCS within
// a quarter second)
static void deliver(afsk_demod_t *d, const hdlc_dec_t *h, size_t n)
{
	if (n == d->last_len && h->buf[n] == d->last_fcs[0] && h->buf[n + 1] == d->last_fcs[1] &&
	    d->samples - d->last_at < d->rate / 4u) {
		d->dups++;
		return;
	}
	d->last_len = n;
	d->last_fcs[0] = h->buf[n];
	d->last_fcs[1] = h->buf[n + 1];
	d->last_at = d->samples;
	d->frames++;
	if (d->fn != NULL) {
		d->fn(h->buf, n, d->ctx);
	}
}

static void path_sample(afsk_demod_t *d, afsk_path_t *p, int32_t x, int32_t cm, int32_t sm, int32_t cs, int32_t ss)
{
	int32_t v[4];
	v[0] = (x * cm) >> MIX_SHIFT;
	v[1] = (x * sm) >> MIX_SHIFT;
	v[2] = (x * cs) >> MIX_SHIFT;
	v[3] = (x * ss) >> MIX_SHIFT;
	for (int k = 0; k < 4; k++) {	// exact integer sums: no drift to correct
		p->sum[k] += v[k] - p->ring[k][d->pos];
		p->ring[k][d->pos] = v[k];
	}
	uint32_t mm = mag(p->sum[0], p->sum[1]), ms = mag(p->sum[2], p->sum[3]);
	p->peak_mark = peak(p->peak_mark, mm, d->decay_shift);
	p->peak_space = peak(p->peak_space, ms, d->decay_shift);
	// mm / peak_mark - ms / peak_space against the hysteresis, cross multiplied (both peaks > 0 once audio arrives):
	// diff x pm x ps = mm x ps - ms x pm
	int64_t pm = p->peak_mark, ps = p->peak_space;
	int64_t diff = (int64_t)mm * ps - (int64_t)ms * pm;
	int64_t hyst = (int64_t)(((uint64_t)pm * (uint64_t)ps) >> 8) * AFSK_HYST_Q8;
	// a little hysteresis: noise near the crossing doesn't flip the tone back and forth (each flip moves the clock)
	bool mark = p->last_mark ? diff > -hyst : diff > hyst;

	// Clock recovery: the PLL counter wraps from +2^31 to -2^31 once a bit, which is where the bit is sampled (mid
	// bit); a tone change should come at 0, so each one pulls the counter a quarter of the way there
	int32_t prev = p->pll;
	p->pll = (int32_t)((uint32_t)p->pll + (uint32_t)d->pll_step);
	if (prev > 0 && p->pll < 0) {
		if (p == &d->path[0]) {
			d->bits++;
		}
		size_t n = hdlc_dec_bit(&p->hdlc, mark);
		if (n != 0) {
			deliver(d, &p->hdlc, n);
		}
	}
	if (mark != p->last_mark) {
		// fast pull in on the preamble, gentle inside a frame where one noisy edge must not move the clock far
		p->pll -= p->hdlc.nbits > 16u ? p->pll / AFSK_PLL_LOCKED : p->pll / AFSK_PLL_SEARCH;
		p->last_mark = mark;
	}
}

static void sample(afsk_demod_t *d, int16_t s)
{
	// the oscillators are shared: cos (sine a quarter turn on) and sin for each tone
	int32_t cm = sine[((d->ph_mark >> 24) + 64u) & 255u], sm = sine[d->ph_mark >> 24];
	int32_t cs = sine[((d->ph_space >> 24) + 64u) & 255u], ss = sine[d->ph_space >> 24];
	d->ph_mark += d->inc_mark;
	d->ph_space += d->inc_space;
	d->samples++;
	path_sample(d, &d->path[0], s, cm, sm, cs, ss);
	afsk_path_t *h = &d->path[1];
	int32_t x = s;
	for (int k = 0; k < 2; k++) {
		int32_t y = (int32_t)(((int64_t)d->hp_a * (h->hp_y[k] + x - h->hp_x[k])) >> 15);
		h->hp_x[k] = x;
		h->hp_y[k] = y;
		x = y;
	}
	path_sample(d, h, x, cm, sm, cs, ss);
	if (++d->pos >= d->win) {
		d->pos = 0;
	}
}

void afsk_demod(afsk_demod_t *d, const int16_t *s, size_t n)
{
	while (n--) {
		sample(d, *s++);
	}
}

uint32_t afsk_demod_fcs_errors(const afsk_demod_t *d)
{
	uint32_t n = 0;
	for (int p = 0; p < AFSK_PATHS; p++) {
		n += d->path[p].hdlc.fcs_errors;
	}
	return n;
}

uint32_t afsk_demod_aborts(const afsk_demod_t *d)
{
	uint32_t n = 0;
	for (int p = 0; p < AFSK_PATHS; p++) {
		n += d->path[p].hdlc.aborts;
	}
	return n;
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GMSK for D-STAR DV (dstar_gmsk.h)
#include <string.h>
#include "dstar/dstar_gmsk.h"
#include "dstar/dstar_air.h"

static int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// --- low-frequency restorer ---

// 2 pi x 65536
#define TWO_PI_Q16		411775u

void dstar_lfr_init(dstar_lfr_t *f, uint32_t rate, uint16_t fc_hz, uint16_t ff_hz)
{
	memset(f, 0, sizeof(*f));
	if (fc_hz == 0 || rate == 0 || ff_hz >= fc_hz) {
		return;
	}
	f->kc = (int32_t)(((uint64_t)TWO_PI_Q16 * (uint32_t)(fc_hz - ff_hz)) / rate);
	f->kf = (int32_t)(((uint64_t)TWO_PI_Q16 * ff_hz) / rate);
}

// y = x + v, v' = -wf v + (wc - wf) x: the shelf as a wire plus a leaky integrator, which needs no tan() for its
// coefficients and is exact enough while fc is far below the sample rate (a few hundred Hz at 8 kHz)
int16_t dstar_lfr(dstar_lfr_t *f, int32_t x)
{
	if (f->kc == 0) {
		return sat16(x);
	}
	f->v += (int64_t)x * f->kc - ((f->v * f->kf) >> 16);
	return sat16(x + (int32_t)(f->v >> 16));
}

// --- modulator ---

static uint32_t bit_step(uint32_t rate)
{
	return (uint32_t)(((uint64_t)DSTAR_BAUD << 32) / rate);
}

// The modulator is the soft modem's transmitter on D-STAR's profile (air_softmodem_tx.c): the pulse and the arithmetic
// that were here, so its output is the same sample for sample. What stays here is D-STAR's: bits to levels, and the
// low-frequency restorer after the pulse
static int bit_sym(void *ctx)
{
	dstar_gmsk_mod_t *m = ctx;
	int b = m->pull ? m->pull(m->ctx) : -1;
	if (b < 0) {
		return AIR_SOFTMODEM_NO_SYM;
	}
	m->bits++;
	return b ? 1 : -1;
}

bool dstar_gmsk_mod_init(dstar_gmsk_mod_t *m, uint32_t rate, int16_t amp, dstar_bit_pull_t pull, void *ctx)
{
	memset(m, 0, sizeof(*m));
	if (rate < DSTAR_RATE_MIN || rate > DSTAR_RATE_MAX) {
		return false;
	}
	m->pull = pull;
	m->ctx = ctx;
	return air_softmodem_tx_init(&m->tx, &dstar_air_profile, rate, amp, bit_sym, m);
}

void dstar_gmsk_mod_trim(dstar_gmsk_mod_t *m, int32_t ppm)
{
	air_softmodem_tx_trim(&m->tx, ppm);
}

size_t dstar_gmsk_mod(dstar_gmsk_mod_t *m, int16_t *out, size_t max)
{
	size_t n = 0;
	int32_t v;
	while (n < max && air_softmodem_tx_sample(&m->tx, &v)) {
		out[n++] = dstar_lfr(&m->lfr, v);
		m->samples++;
	}
	return n;
}

// --- demodulator ---

#define PHASE_HALF		0x80000000u
// The loops. Phase: a crossing's distance from the expected bit boundary, times 2^-PH_SHIFT, comes off the phase:
// fast while searching (a preamble is 64 bits or more and the clock must settle within it), slow when locked (noise
// on the crossings jitters the clock less). Frequency, locked only: times 2^-FR_SHIFT onto the step. GMSK's crossings
// move with the bits around them, so this is slow: a 400 ppm clock is read in about 2000 bits, the phase loop
// carrying the difference meanwhile
#define PH_SHIFT_SEARCH		2
#define PH_SHIFT_LOCKED		4
#define FR_SHIFT		14
// The frequency loop's range: +-1000 ppm (a transmitter's 4800 bps is far closer than that)
#define ADJ_LIMIT(step)		((int32_t)((step) / 1000u))
// DC. While searching, the plain average of the input over about DC_SEARCH_BITS bits: a preamble and scrambled data
// are balanced, and a decision-directed loop can't start from a large offset (with every decision a 1 its error
// averages zero and it stays there). Once locked, decision-directed and slower, 2^-DC_SHIFT_LOCKED of each bit's
// error: the voice frames aren't scrambled, and a run of one value would pull a plain average off
#define DC_SEARCH_BITS		16u
#define DC_SHIFT_LOCKED		6
// The level: the average magnitude at the decisions
#define LEV_SHIFT_SEARCH	3
#define LEV_SHIFT_LOCKED	6
// A level to start from before any bit has been seen: a quarter of full scale
#define LEV_START		(8192 << 8)

bool dstar_gmsk_demod_init(dstar_gmsk_demod_t *d, uint32_t rate, dstar_soft_push_t push, void *ctx)
{
	memset(d, 0, sizeof(*d));
	if (rate < DSTAR_RATE_MIN || rate > DSTAR_RATE_MAX) {
		return false;
	}
	d->step = bit_step(rate);
	d->lev = LEV_START;
	d->kdc = (int32_t)((65536u * DSTAR_BAUD) / (rate * DC_SEARCH_BITS));
	d->push = push;
	d->ctx = ctx;
	// A boxcar of half a bit ahead of everything at the higher rates takes the discriminator's noise above the
	// signal's band (its first null is at twice the bit rate); at 8 kHz the path's own filters have done that
	uint32_t box = (rate + DSTAR_BAUD) / (2u * DSTAR_BAUD);
	d->box_n = (uint8_t)(box < 1u ? 1u : box > DSTAR_BOX_MAX ? DSTAR_BOX_MAX : box);
	return true;
}

int32_t dstar_gmsk_demod_ppm(const dstar_gmsk_demod_t *d)
{
	return (int32_t)(((int64_t)d->adj * 1000000) / (int64_t)d->step);
}

#define BIT			0x100000000ll

static void bit_out(dstar_gmsk_demod_t *d, int32_t v);

// The decision value at position at (between from, the previous sample, and from + inc, this one), interpolated
static void sample_at(dstar_gmsk_demod_t *d, int64_t from, uint32_t inc, int32_t cur, int64_t at)
{
	int32_t v = d->prev + (int32_t)(((int64_t)(cur - d->prev) * (at - from)) / (int64_t)inc);
	d->decided = true;
	bit_out(d, v);
}

static void bit_out(dstar_gmsk_demod_t *d, int32_t v)
{
	bool one = v > 0;
	// The DC follows the decisions' error, the level their magnitude (both Q8)
	int32_t lev = d->lev >> 8;
	if (d->locked) {
		int32_t err = v - (one ? lev : -lev);
		d->dc += err * 256 >> DC_SHIFT_LOCKED;
	}
	int32_t mag = v < 0 ? -v : v;
	d->lev += (mag * 256 - d->lev) >> (d->locked ? LEV_SHIFT_LOCKED : LEV_SHIFT_SEARCH);
	if (d->lev < 256) {
		d->lev = 256;
	}
	d->bits++;
	if (d->push) {
		int16_t s = sat16(d->invert ? -v : v);
		d->push(d->ctx, s, (int16_t)(lev < 1 ? 1 : lev > 32767 ? 32767 : lev));
	}
}

void dstar_gmsk_demod(dstar_gmsk_demod_t *d, const int16_t *s, size_t n)
{
	// Never started (init refused the rate): its boxcar is 0 long, a division by zero below. Nothing then; the work a
	// sample is fixed, so there is no loop to bound
	if (d->box_n == 0 || d->step == 0) {
		return;
	}
	for (size_t k = 0; k < n; k++) {
		int32_t x = dstar_lfr(&d->lfr, s[k]);
		d->box_sum += x - d->box[d->box_pos];
		d->box[d->box_pos] = x;
		d->box_pos = (uint8_t)((d->box_pos + 1u) % d->box_n);
		int32_t in = d->box_sum / d->box_n;
		if (!d->locked) {
			d->dc += (int32_t)(((int64_t)(in * 256 - d->dc) * d->kdc) >> 16);
		}
		int32_t cur = in - (d->dc >> 8);

		// This sample is inc of a bit after the previous one. Positions are counted from the previous sample's
		// phase without wrapping, so a bit's middle or edge between the two is a plain comparison. At 8 kHz a
		// sample is 0.6 of a bit, so one interval can hold this bit's middle, the edge and the next bit's middle
		// in any combination; each bit is decided once (decided), whatever the clock's corrections do to the phase
		uint32_t inc = d->step + (uint32_t)d->adj;
		int64_t from = d->phase, to = from + inc;
		if (to >= 2 * BIT) {
			to -= BIT;	// can't happen with the loops' limits; keeps a corrupted state from running away
			from -= BIT;
		}
		if (!d->decided && to >= (int64_t)PHASE_HALF) {
			sample_at(d, from, inc, cur, from > (int64_t)PHASE_HALF ? from : (int64_t)PHASE_HALF);
		}
		if (to >= BIT) {
			d->decided = false;
			if (to >= BIT + (int64_t)PHASE_HALF) {
				sample_at(d, from, inc, cur, BIT + (int64_t)PHASE_HALF);
			}
			from -= BIT;
			to -= BIT;
		}
		if ((cur > 0) != (d->prev > 0) && cur != d->prev) {
			// A zero crossing: where between the samples, and how far that is from the nearest bit edge. A
			// transition swings twice the level in about a bit; a crossing slower than a quarter of that is more
			// likely noise wandering near zero, and isn't used. Per sample: |slope| >= 2 lev x inc / 2^32 / 4
			int32_t slope = cur - d->prev;
			int64_t aslope = slope < 0 ? -(int64_t)slope : slope;
			if ((aslope << 33) >= (int64_t)(d->lev >> 8) * inc) {
				int64_t at = from + ((int64_t)(-d->prev) * inc) / slope;
				int32_t e = (int32_t)(uint32_t)at;	// signed distance to the nearest bit edge
				int ph = d->locked ? PH_SHIFT_LOCKED : PH_SHIFT_SEARCH;
				to -= e >> ph;
				// The frequency loop runs only while locked: between transmissions the crossings are noise,
				// and an integrator fed noise walks to its limit. The phase loop alone follows any clock
				// within the limit through a preamble
				if (d->locked) {
					d->adj -= e >> FR_SHIFT;
					int32_t lim = ADJ_LIMIT(d->step);
					d->adj = d->adj > lim ? lim : d->adj < -lim ? -lim : d->adj;
				}
				d->crossings++;
			}
		}
		// A correction may leave the phase a little before this bit's edge (the bit before is decided) or past
		// the next one (the next sample wraps it). Clamping it to the bit instead drops the corrections on one
		// side of each edge, and the frequency loop then settles 500 ppm off to make up for them
		d->phase = to;
		d->prev = cur;
	}
}

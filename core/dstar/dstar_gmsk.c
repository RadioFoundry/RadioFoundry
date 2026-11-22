// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GMSK for D-STAR DV (dstar_gmsk.h)
#include <string.h>
#include "dstar/dstar_gmsk.h"

// The GMSK frequency pulse (a one-bit rectangle through the BT = 0.5 Gaussian filter) from 1.5 bits before a bit's
// middle to 1.5 bits after, 64 points a bit and both ends, interpolated between points; a long run of one value sums
// to 32767 within 3. tools/dstar_ref.py pulse makes it; dstarModemTest checks it against libm's erf. Past 1.5 bits the
// pulse is under 1e-4 of its peak, so three bits make every sample
static const int16_t pulse[193] = {
	3, 3, 4, 5, 7, 8, 10, 13, 16, 19, 24, 29, 36, 43, 52, 63,
	76, 92, 110, 131, 155, 184, 217, 256, 301, 352, 411, 477, 554, 640, 737, 847,
	970, 1107, 1260, 1430, 1618, 1825, 2053, 2302, 2573, 2868, 3188, 3533, 3904, 4302, 4728, 5180,
	5660, 6168, 6703, 7264, 7852, 8464, 9100, 9759, 10438, 11137, 11853, 12584, 13328, 14082, 14844, 15611,
	16381, 17150, 17917, 18678, 19431, 20174, 20903, 21617, 22312, 22988, 23643, 24274, 24880, 25459, 26012, 26536,
	27030, 27495, 27930, 28334, 28707, 29050, 29361, 29643, 29893, 30113, 30304, 30464, 30595, 30697, 30769, 30813,
	30827, 30813, 30769, 30697, 30595, 30464, 30304, 30113, 29893, 29643, 29361, 29050, 28707, 28334, 27930, 27495,
	27030, 26536, 26012, 25459, 24880, 24274, 23643, 22988, 22312, 21617, 20903, 20174, 19431, 18678, 17917, 17150,
	16381, 15611, 14844, 14082, 13328, 12584, 11853, 11137, 10438, 9759, 9100, 8464, 7852, 7264, 6703, 6168,
	5660, 5180, 4728, 4302, 3904, 3533, 3188, 2868, 2573, 2302, 2053, 1825, 1618, 1430, 1260, 1107,
	970, 847, 737, 640, 554, 477, 411, 352, 301, 256, 217, 184, 155, 131, 110, 92,
	76, 63, 52, 43, 36, 29, 24, 19, 16, 13, 10, 8, 7, 5, 4, 3,
	3,
};

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

static int8_t next_sym(dstar_gmsk_mod_t *m)
{
	int b = m->pull ? m->pull(m->ctx) : -1;
	if (b < 0) {
		m->tail++;
		return 0;
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
	m->step = bit_step(rate);
	m->amp = amp;
	m->pull = pull;
	m->ctx = ctx;
	// Nothing before the first bit; the first bit current and the second next
	m->sym[1] = next_sym(m);
	m->sym[2] = next_sym(m);
	return true;
}

void dstar_gmsk_mod_trim(dstar_gmsk_mod_t *m, int32_t ppm)
{
	m->step = (uint32_t)((int64_t)m->step + ((int64_t)m->step * ppm) / 1000000);
}

size_t dstar_gmsk_mod(dstar_gmsk_mod_t *m, int16_t *out, size_t max)
{
	size_t n = 0;
	// Done once the source has run dry and the last real bit's pulse is past: two bits after it went out of the
	// current slot
	while (n < max && m->tail < 3u) {
		// The phase's top six bits pick the pulse point, the next 16 the way to the following one: the current bit
		// is 64-128 into the table, the previous bit 128-192, the next 0-64. Without the interpolation the nearest
		// point is up to 2.4 % of the deviation off
		uint32_t i = m->phase >> 26, fr = (m->phase >> 10) & 0xFFFFu;
		int32_t y = 0;
		for (int b = 0; b < 3; b++) {
			uint32_t at = i + 128u - 64u * (uint32_t)b;
			int32_t p = pulse[at] + (int32_t)((((int32_t)pulse[at + 1u] - pulse[at]) * (int32_t)fr) >> 16);
			y += m->sym[b] * p;
		}
		out[n++] = dstar_lfr(&m->lfr, (int32_t)(((int64_t)y * m->amp) >> 15));
		m->samples++;
		uint32_t was = m->phase;
		m->phase += m->step;
		if (m->phase < was) {
			m->sym[0] = m->sym[1];
			m->sym[1] = m->sym[2];
			m->sym[2] = m->tail ? (m->tail++, (int8_t)0) : next_sym(m);
		}
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

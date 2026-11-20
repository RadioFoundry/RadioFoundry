// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM modem for Yaesu System Fusion (ysf_c4fm.h)
#include <string.h>
#include "ysf/ysf_c4fm.h"

#define S	YSF_RRC_SPAN
#define PTS	32	// table points a symbol

// The root raised cosine pulse, roll-off 0.2, from 6 symbols before its center to 6 after, 32 points a symbol, Q14
// (its peak is 1.0546 for a pulse of unit energy, so a matched filter's value at a symbol's center is its level).
// tools/ysf_ref.py rrc makes it; ysfModemTest checks it against the formula. The span is set by the intersymbol
// interference the cut leaves when both ends use it: at 4 symbols 0.083 of a +1 level rms (the pulse pair's value 4
// symbols out is -0.024), which cost about 1 dB; at 6 symbols 0.015
static const int16_t rrc[2 * S * PTS + 1] = {
	130, 141, 150, 157, 161, 163, 163, 160, 154, 146, 135, 121,
	105, 87, 67, 45, 21, -4, -29, -56, -82, -109, -134, -159,
	-182, -204, -224, -241, -255, -266, -274, -278, -278, -274, -267, -254,
	-238, -218, -194, -166, -134, -100, -62, -22, 20, 64, 109, 155,
	200, 245, 288, 329, 368, 404, 436, 463, 486, 504, 515, 521,
	520, 512, 498, 477, 448, 413, 371, 323, 268, 208, 143, 73,
	0, -76, -155, -235, -316, -395, -474, -549, -622, -689, -751, -806,
	-854, -893, -924, -944, -954, -953, -940, -916, -880, -832, -772, -701,
	-618, -525, -422, -310, -190, -63, 70, 208, 348, 490, 633, 773,
	910, 1042, 1167, 1284, 1391, 1486, 1567, 1634, 1684, 1717, 1732, 1728,
	1703, 1658, 1591, 1504, 1396, 1267, 1118, 950, 763, 560, 341, 109,
	-135, -388, -649, -914, -1181, -1447, -1709, -1964, -2209, -2441, -2657, -2853,
	-3027, -3175, -3295, -3383, -3438, -3457, -3438, -3378, -3277, -3132, -2943, -2709,
	-2429, -2104, -1734, -1319, -861, -360, 181, 760, 1375, 2023, 2701, 3406,
	4133, 4880, 5642, 6414, 7192, 7972, 8749, 9518, 10275, 11014, 11731, 12421,
	13081, 13705, 14290, 14832, 15327, 15772, 16164, 16500, 16778, 16996, 17153, 17248,
	17279, 17248, 17153, 16996, 16778, 16500, 16164, 15772, 15327, 14832, 14290, 13705,
	13081, 12421, 11731, 11014, 10275, 9518, 8749, 7972, 7192, 6414, 5642, 4880,
	4133, 3406, 2701, 2023, 1375, 760, 181, -360, -861, -1319, -1734, -2104,
	-2429, -2709, -2943, -3132, -3277, -3378, -3438, -3457, -3438, -3383, -3295, -3175,
	-3027, -2853, -2657, -2441, -2209, -1964, -1709, -1447, -1181, -914, -649, -388,
	-135, 109, 341, 560, 763, 950, 1118, 1267, 1396, 1504, 1591, 1658,
	1703, 1728, 1732, 1717, 1684, 1634, 1567, 1486, 1391, 1284, 1167, 1042,
	910, 773, 633, 490, 348, 208, 70, -63, -190, -310, -422, -525,
	-618, -701, -772, -832, -880, -916, -940, -953, -954, -944, -924, -893,
	-854, -806, -751, -689, -622, -549, -474, -395, -316, -235, -155, -76,
	0, 73, 143, 208, 268, 323, 371, 413, 448, 477, 498, 512,
	520, 521, 515, 504, 486, 463, 436, 404, 368, 329, 288, 245,
	200, 155, 109, 64, 20, -22, -62, -100, -134, -166, -194, -218,
	-238, -254, -267, -274, -278, -278, -274, -266, -255, -241, -224, -204,
	-182, -159, -134, -109, -82, -56, -29, -4, 21, 45, 67, 87,
	105, 121, 135, 146, 154, 160, 163, 163, 161, 157, 150, 141,
	130,
};

static inline int32_t rrc_at(uint32_t pos)	// pos: Q16 table points from the pulse's start
{
	uint32_t i = pos >> 16;
	if (i >= 2u * S * PTS) {
		return i == 2u * S * PTS ? rrc[i] : 0;
	}
	int32_t fr = (int32_t)(pos & 0xFFFFu);
	return rrc[i] + (int32_t)((((int32_t)rrc[i + 1u] - rrc[i]) * fr) >> 16);
}

// --- low-frequency restorer ---

void ysf_lfr_init(ysf_lfr_t *f, uint32_t rate, uint16_t fc_hz, uint16_t ff_hz)
{
	memset(f, 0, sizeof(*f));
	if (fc_hz == 0 || rate == 0) {
		return;
	}
	if (ff_hz == 0) {
		ff_hz = YSF_LFR_FLOOR_HZ;
	}
	if (ff_hz >= fc_hz) {
		return;	// nothing to restore down to
	}
	f->kc = (int32_t)(((int64_t)(fc_hz - ff_hz) * 411775) / (int64_t)rate);	// 2 pi x 65536 = 411775
	f->kf = (int32_t)(((int64_t)ff_hz * 411775) / (int64_t)rate);
}

int16_t ysf_lfr(ysf_lfr_t *f, int32_t x)
{
	if (f->kc == 0) {
		return (int16_t)x;
	}
	// v integrates x and leaks at the floor; the output is x plus (wc - wf) times it. x is scaled to Q16 by a multiply,
	// not a shift: a left shift of a negative value is undefined in C, and half the samples are negative (UBSan stops
	// ysfModemTest there under make SANITIZE=1). csky-elf-gcc -O2 emits the same code for both
	f->v += (int64_t)x * 65536 - ((f->v * f->kf) >> 16);
	int64_t y = x + (((f->v >> 16) * f->kc) >> 16);
	return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
}

// --- modulator ---

bool ysf_c4fm_mod_init(ysf_c4fm_mod_t *m, uint32_t rate, int16_t amp, ysf_sym_pull_t pull, void *ctx)
{
	memset(m, 0, sizeof(*m));
	if (rate < YSF_RATE_MIN || rate > YSF_RATE_MAX || pull == NULL) {
		return false;
	}
	// Rounded up: rounded down, a symbol whose start falls on a sample (every tenth at 48 kHz) entered one sample late
	m->step = (uint32_t)((((uint64_t)YSF_BAUD << 32) + rate - 1u) / rate);
	m->amp = amp;
	m->pull = pull;
	m->ctx = ctx;
	// The first symbol enters at the far end, so its pulse rises from nothing: the output starts S symbols before
	// the first symbol's center
	int s = pull(ctx);
	if (s < 0) {
		m->tail = 2u * S;
	} else {
		m->lv[2 * S - 1] = (int8_t)ysf_c4fm_level((uint8_t)s);
		m->symbols++;
	}
	return true;
}

void ysf_c4fm_mod_ppm(ysf_c4fm_mod_t *m, int32_t ppm)
{
	m->step = (uint32_t)((int64_t)m->step + (int64_t)m->step * ppm / 1000000);
}

static void mod_next(ysf_c4fm_mod_t *m)
{
	memmove(&m->lv[0], &m->lv[1], sizeof(m->lv) - 1u);
	int s = m->tail ? -1 : m->pull(m->ctx);
	if (s < 0) {
		m->lv[2 * S - 1] = 0;
		if (m->tail < 255u) {
			m->tail++;
		}
	} else {
		m->lv[2 * S - 1] = (int8_t)ysf_c4fm_level((uint8_t)s);
		m->symbols++;
	}
}

bool ysf_c4fm_mod(ysf_c4fm_mod_t *m, int16_t *out, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		// Done once the source has run dry and the last real symbol's pulse is past
		if (m->tail >= 2u * S) {
			memset(&out[k], 0, (n - k) * sizeof(out[0]));
			return false;
		}
		// lv[j] is the symbol whose center lies S - 1 - j symbols (plus the phase) before this sample: its pulse
		// point is (phase + S - 1 - j + S) symbols from the pulse's start
		uint32_t ph = m->phase >> 11;	// Q21 of a symbol -> Q16 table points (32 a symbol)
		int32_t acc = 0;
		for (int j = 0; j < 2 * S; j++) {
			if (m->lv[j] != 0) {
				uint32_t pos = ((uint32_t)(2 * S - 1 - j) * PTS << 16) + ph;
				acc += m->lv[j] * rrc_at(pos);
			}
		}
		int32_t v = (int32_t)(((int64_t)acc * m->amp) >> 14);
		out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		m->samples++;
		uint32_t p = m->phase + m->step;
		if (p < m->phase) {
			mod_next(m);
		}
		m->phase = p;
	}
	return true;
}

// --- demodulator ---

bool ysf_c4fm_demod_init(ysf_c4fm_demod_t *d, uint32_t rate, ysf_sym_push_t push, void *ctx)
{
	memset(d, 0, sizeof(*d));
	if (rate < YSF_RATE_MIN || rate > YSF_RATE_MAX || push == NULL) {
		return false;
	}
	d->rate = rate;
	d->sps = (int32_t)(((uint64_t)rate << 16) / YSF_BAUD);
	d->tstep = (int32_t)(((uint64_t)PTS * YSF_BAUD << 16) / rate);
	d->gain = (int32_t)(((uint64_t)YSF_BAUD << 16) / rate);
	d->push = push;
	d->ctx = ctx;
	d->unit = 16;
	// The first instant far enough in that every tap of the pulse has arrived
	d->t = (int64_t)S * d->sps + (1 << 16);
	return true;
}

void ysf_c4fm_demod_lock(ysf_c4fm_demod_t *d, bool locked)
{
	d->locked = locked;
	if (!locked) {
		d->freq = 0;	// an integrator fed noise walks to its limit: start the next sync from nominal
		// The envelope picks up where the decisions left the DC and the level
		d->hi = d->dc + 3 * d->unit;
		d->lo = d->dc - 3 * d->unit;
	}
}

int32_t ysf_c4fm_demod_ppm(const ysf_c4fm_demod_t *d)
{
	if (d->sps == 0) {
		return 0;	// never started (a symbol path)
	}
	// A fast transmitter makes the symbols come sooner: a negative correction to the step
	return (int32_t)(-(int64_t)d->freq * 1000000 / 256 / d->sps);
}

int32_t ysf_c4fm_demod_offset_hz(const ysf_c4fm_demod_t *d)
{
	return d->unit > 0 ? (int32_t)((int64_t)d->dc * (int32_t)YSF_DEV_HZ / d->unit) : 0;
}

// The matched filter's value at t (Q16 samples): the received samples weighted by the pulse at their distance from
// t, summed and scaled to an integral. The input must hold every sample within S symbols of t
static int32_t matched(const ysf_c4fm_demod_t *d, int64_t t)
{
	int64_t reach = (int64_t)S * d->sps;
	int64_t first = (t - reach + 0xFFFF) >> 16, last = (t + reach) >> 16;
	int64_t acc = 0;
	for (int64_t k = first; k <= last; k++) {
		int64_t dist = (k << 16) - t;	// Q16 samples, -reach .. reach
		int64_t pos = ((dist * d->tstep) >> 16) + ((int64_t)S * PTS << 16);
		if (pos < 0) {
			continue;
		}
		acc += (int64_t)d->ring[(uint32_t)k & (YSF_DEMOD_RING - 1u)] * rrc_at((uint32_t)pos);
	}
	return (int32_t)((acc * d->gain) >> 30);	// Q14 pulse, Q16 gain
}

static int32_t clamp32(int32_t v, int32_t lim)
{
	return v > lim ? lim : v < -lim ? -lim : v;
}

static void symbol(ysf_c4fm_demod_t *d)
{
	int32_t dc = d->dc >> 4;
	int32_t raw = matched(d, d->t);
	int32_t y = raw - dc;
	int32_t u = d->unit >> 4;
	if (u < 1) {
		u = 1;
	}
	int32_t ay = y < 0 ? -y : y;
	int32_t a = ay > 2 * u ? 3 : 1;	// the nearest level
	if (y < 0) {
		a = -a;
	}

	// The symbol clock. Both detectors give a positive error when the instants are late, scaled by the level so it is
	// a fraction of a symbol whatever the signal's strength (Q4):
	//   - searching, Gardner: the value halfway between two symbols has already moved toward the later one when
	//     late. It needs no decisions, but with a roll-off of 0.2 its own pattern noise jittered the clock by about
	//     0.06 of a symbol, which this steep an eye can't afford for long
	//   - locked, Mueller and Mueller: the previous value against this decision less this value against the previous
	//     decision. Little pattern noise at any roll-off, and the decisions are right by then
	int32_t e;
	if (d->locked) {
		e = (int32_t)(((int64_t)d->prev * a - (int64_t)y * d->prev_a) * 16 / u);
	} else {
		int32_t ym = matched(d, d->t - d->sps / 2) - dc;
		e = (int32_t)(((int64_t)(y - d->prev) * ym * 16) / ((int64_t)u * u));
	}
	e = clamp32(e, 16 * 16);	// at most 16 levels squared: a noise spike can't throw the clock
	d->prev = y;
	d->prev_a = a;
	// Divisions, not shifts: a shift rounds down, and in the frequency integrator, with its small gain, that bias of
	// half a step a symbol held the clock 0.13 of a symbol late
	int32_t kp = d->locked ? 6 : 4;	// searching: faster, the frame sync must arrive in time
	int32_t corr = (int32_t)(((int64_t)e * d->sps) / (1 << (4 + 4 + kp)));
	if (d->locked) {
		// Kept to 1/256 of the step's unit: a smaller error than a whole unit's worth must still move it, or the
		// integrator has a dead zone and never takes up a small clock error (it held 300 ppm as 36)
		d->freq = clamp32(d->freq - e * (d->sps >> 10) / 16, d->sps / 1000 * 256);	// late: a shorter step; +-1000 ppm
	}
	d->t += d->sps + d->freq / 256 - corr;

	// The DC and the level. Searching, from the envelope: the highest and lowest symbol values, each following a
	// value beyond it quickly and the rest slowly, are the outer levels, so their middle is the DC and a sixth of their
	// distance a +1 level. A plain average of the symbols was tried first: over 64 symbols the data's own spread moved
	// it by 0.4 of a level, and a longer one takes most of a frame to follow a carrier offset. The slow side must still
	// let go of the receiver's noise within a header (a decay of 1/1024 missed both headers of a carrier 1 kHz off). Locked, decision
	// directed: the error from the nearest level moves the level slowly and the DC at 1/16 a symbol, fast enough to
	// follow what a path's high-pass leaves below 30 Hz or so. 4FSK puts as much of its power near DC as anywhere in
	// its band, so a high-pass at 100 Hz moves the levels by about half a level and a loop slow enough to cost nothing
	// in noise (1/256) left an error floor of several percent; 1/8 cost 0.5 dB on a flat path
	int32_t r16 = raw * 16;
	d->hi += r16 > d->hi ? (r16 - d->hi) / 4 : (r16 - d->hi) / 128;
	d->lo += r16 < d->lo ? (r16 - d->lo) / 4 : (r16 - d->lo) / 128;
	if (d->locked) {
		d->dc += (y - a * u) * 16 / 16;
		d->unit += ((ay * 16) / (a < 0 ? -a : a) - d->unit) / 256;
	} else {
		d->dc = (d->hi + d->lo) / 2;
		d->unit = (d->hi - d->lo) / 6;
	}
	if (d->unit < 16) {
		d->unit = 16;
	}
	d->symbols++;
	d->push(d->ctx, (int16_t)clamp32(y, 32767), (int16_t)clamp32(d->unit >> 4, 32767));
}

void ysf_c4fm_demod(ysf_c4fm_demod_t *d, const int16_t *in, size_t n)
{
	int64_t reach = (int64_t)S * d->sps + (1 << 16);
	for (size_t i = 0; i < n; i++) {
		d->ring[d->n & (YSF_DEMOD_RING - 1u)] = ysf_lfr(&d->lfr, in[i]);
		d->n++;
		while (d->t + reach <= ((int64_t)d->n << 16)) {
			symbol(d);
		}
	}
}

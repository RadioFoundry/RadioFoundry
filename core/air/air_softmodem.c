// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's receiver (air_softmodem.h, docs/spec/air-interface.md 6).
//
// Input samples -> minus the tap's offset -> polyphase interpolator to five samples per symbol -> integrate and
// dump -> the line (a ring of the last 28 symbols of filtered samples). On every filtered sample the newest symbol
// spaced samples in the line are fitted to each sync pattern (sample = offset + level x pattern); where the fit's
// error passes through a minimum, that sample is a symbol center, and the fit's level and offset are the signal's.
// Symbols leave from the far end of the line, so a sync is sampled on the timing it gave.
#include "air_softmodem.h"

#include <string.h>

#include "air_syms.h"

#define SPS		AIR_SOFTMODEM_SPS
#define LINE		AIR_SOFTMODEM_LINE
#define TAPS		AIR_SOFTMODEM_TAPS
#define PHASES		12			// interpolator phases per input sample
#define LINE_LVL	16			// line units per symbol level at nominal: +3 is 48, headroom to 127
#define EMIT_AT		(LINE - 2 * SPS)	// a symbol leaves when its center is this far back
#define Q_NONE		0xFFFFu
// A fit's error: 256 x the mean square distance of the samples from the fitted line, in symbol levels. 92 is 0.6
// of a level RMS, where slicing already fails often: a sync worse than that gives no timing worth taking
#define Q_MAX		92
#define LVL_NOM		(LINE_LVL * 256)
#define LVL_MAX		(LVL_NOM * 3 / 2)
#define LVL_MIN_KNOWN	(LVL_NOM * 7 / 12)	// as the framer's limits
#define LVL_MIN_TRACKED	(LVL_NOM / 4)		// the tracked level sits low after noise: the fit corrects it
#define OFF_MAX		(LINE_LVL * 3 / 2 * 256)	// half the outer level
#define TRACK_SHIFT_MIN	4
#define TRACK_SHIFT_MAX	10
#define HOLE_US		2000			// input time off by more: samples were lost

// The interpolator: a windowed sinc low-pass at 0.45 of the input rate, 12 phases x 8 taps, each phase summing to
// 16384. Row p, tap t weighs the input sample t back from the newest; the output sits (47.5 - p) / 12 samples back
static const int16_t interp[PHASES][TAPS] = {
	{      0,    105,   -630,   1791,  14707,    735,   -394,     70 },
	{     -1,    143,   -884,   3021,  14382,   -130,   -189,     42 },
	{     -2,    182,  -1141,   4393,  13753,   -797,    -24,     20 },
	{     -4,    216,  -1376,   5867,  12843,  -1268,    101,      5 },
	{     -7,    240,  -1565,   7393,  11699,  -1557,    185,     -4 },
	{     -8,    248,  -1678,   8914,  10369,  -1685,    232,     -8 },
	{     -8,    232,  -1685,  10369,   8914,  -1678,    248,     -8 },
	{     -4,    185,  -1557,  11699,   7393,  -1565,    240,     -7 },
	{      5,    101,  -1268,  12843,   5867,  -1376,    216,     -4 },
	{     20,    -24,   -797,  13753,   4393,  -1141,    182,     -2 },
	{     42,   -189,   -130,  14382,   3021,   -884,    143,     -1 },
	{     70,   -394,    735,  14707,   1791,   -630,    105,      0 },
};

static uint32_t gcd32(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static bool rate_ok(const air_profile_t *p, uint32_t rate_hz, uint16_t *step, uint16_t *den)
{
	uint32_t out = (uint32_t)p->sym_rate * SPS, g;

	// Below 8 kHz a 4800 symbol/s signal does not fit; above twice the filtered rate the interpolator (which does
	// not filter for the lower rate) would fold noise in
	if (!p->sym_rate || rate_hz < 8000 || rate_hz > 2 * out || rate_hz < (uint32_t)p->sym_rate * 5 / 3)
		return false;
	g = gcd32(rate_hz, out);
	if (rate_hz / g + out / g > 0xFFFF)
		return false;
	*step = (uint16_t)(rate_hz / g);
	*den = (uint16_t)(out / g);
	return true;
}

uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	uint16_t step, den;

	if (!bb || !bb->rx_start || !bb->rx_read || bb->rx_channels != 1)
		return 0;		// a split band (two channels) is not written
	if (p->levels != 2 && p->levels != 4)
		return 0;
	if (!rate_ok(p, bb->rx_rate_hz, &step, &den))
		return 0;
	// The band: up to a little past half the symbol rate, and down to where a run of one level still holds (four
	// levels have no room for a sagging baseline)
	if (bb->rx_hi_hz < (uint32_t)p->sym_rate * 11 / 20)
		return 0;
	if (bb->rx_lo_hz > p->sym_rate / (p->levels == 4 ? 100 : 20))
		return 0;
	return AIR_FIT_RX | AIR_FIT_SOFT;
}

void air_softmodem_rx_reset(air_softmodem_t *m)
{
	memset(m->in, 0, sizeof m->in);
	memset(m->sum, 0, sizeof m->sum);
	m->acc = 0;
	m->in_at = m->sum_at = 0;
	m->pos = 0;
	m->line_at = m->line_n = 0;
	m->track_shift = TRACK_SHIFT_MIN;
	m->track_n = 1u << TRACK_SHIFT_MIN;
	m->locked = false;
	m->running = false;
	m->cand_q = m->cand_prev_q = m->last_q = Q_NONE;
	m->hold = 0;
	m->due = EMIT_AT - 1;
	m->sub = 0;
	m->scale = (uint16_t)((AIR_SOFT_OUTER / 3) * 256 / LINE_LVL);
	m->off = 0;
	m->since_sync = 0xFFFF;
	m->sync_wait = 0;
	m->gap = true;
	m->n = 0;
}

bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx)
{
	uint16_t step, den;

	if ((p->levels != 2 && p->levels != 4) || !rate_ok(p, rate_hz, &step, &den))
		return false;
	memset(m, 0, sizeof *m);
	m->p = p;
	m->out = out;
	m->ctx = ctx;
	m->rate_hz = rate_hz;
	m->step = step;
	m->den = den;
	// The integrate and dump over a symbol is the receive filter the C4FM pulse is made for, and close to a matched
	// one for a 2-level square pulse. A root raised cosine or Gaussian pulse gets a shorter one: it only smooths
	m->box = (p->shape == AIR_SHAPE_C4FM || p->shape == AIR_SHAPE_NRZ) ? SPS : 3;
	m->gain_known = counts_per_khz != 0;
	if (m->gain_known) {
		// The outer symbol (dev_outer_hz) to 3 x LINE_LVL, after the sum over box samples
		uint64_t outer = (uint64_t)p->dev_outer_hz * counts_per_khz;	// counts x 1000

		if (!outer)
			return false;
		m->gain = (uint32_t)(((uint64_t)(3 * LINE_LVL) * 65536u * 1000u) / (outer * m->box));
	} else {
		m->gain = 65536u / m->box;
		m->env = 3 * LINE_LVL * 1024;
	}
	for (uint8_t i = 0; i < p->nsyncs && i < AIR_SYNCS_MAX; i++) {
		const air_sync_t *s = &p->sync[i];
		int16_t s1 = 0, s2 = 0;

		if (s->nsyms < 8 || s->nsyms > AIR_SYNC_SYMS_MAX)
			continue;
		for (uint8_t k = 0; k < s->nsyms; k++) {
			int8_t e = air_sym_level(air_sym_get(s->syms, k));

			s1 = (int16_t)(s1 + e);
			s2 = (int16_t)(s2 + e * e);
		}
		if (s->nsyms * s2 - s1 * s1 <= 0)
			continue;	// one level only: nothing to fit
		m->s1[i] = (int8_t)s1;
		m->s2[i] = (uint8_t)s2;
	}
	air_softmodem_rx_reset(m);
	return true;
}

static int8_t line_back(const air_softmodem_t *m, uint8_t back)
{
	return m->line[(uint16_t)(m->line_at + LINE - 1 - back) % LINE];
}

static void flush(air_softmodem_t *m)
{
	air_chunk_t c;

	if (!m->n)
		return;
	c.t_us = m->t_chunk_us;
	c.nsyms = m->n;
	c.sync_at = m->sync_at;
	c.sync_id = m->sync_id;
	c.flags = m->flags;
	c.rssi_dbm = 0;
	c.hard = m->hard;
	c.soft = m->soft;
	m->n = 0;
	m->out(&c, m->ctx);
}

// The symbol whose center is due samples back leaves the line. t_us: the time of the input sample in hand, less the
// filters' delay
static void emit(air_softmodem_t *m, uint32_t t_us)
{
	int32_t x = line_back(m, m->due) * 16, v;
	uint8_t d;
	bool sync;

	// Between two samples: straight line to the neighbor on the side the fit's minimum leaned to
	if (m->sub > 0)
		x += m->sub * (line_back(m, (uint8_t)(m->due - 1)) - line_back(m, m->due));
	else if (m->sub < 0)
		x -= m->sub * (line_back(m, (uint8_t)(m->due + 1)) - line_back(m, m->due));
	v = ((x - m->off / 16) * (int32_t)m->scale) / 4096;
	if (v > 127)
		v = 127;
	if (v < -127)
		v = -127;
	if (m->p->levels == 2)
		d = v >= 0 ? 1 : 3;
	else
		d = v >= 2 * AIR_SOFT_OUTER / 3 ? 1 : v >= 0 ? 0 : v >= -2 * AIR_SOFT_OUTER / 3 ? 2 : 3;
	sync = m->sync_wait && --m->sync_wait == 0;
	if (sync && m->n && m->sync_at >= 0)
		flush(m);		// a chunk names one sync
	if (!m->n) {
		uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;

		// The line's newest sample sits pos / den of an input sample after t_us
		m->t_chunk_us = t_us + (uint32_t)(((uint64_t)m->pos * 1000000u) / ((uint32_t)m->den * m->rate_hz))
				- (uint32_t)(((uint32_t)m->due * 16 - m->sub) * 62500u / out_rate);
		m->flags = m->gap ? AIR_CHUNK_GAP : 0;
		// Past the profile's sync spacing with none seen (or none ever): the timing is the last one's, or none.
		// (since_sync starts when a sync is found, two sync lengths before its symbols are all out)
		if (m->since_sync > (uint32_t)m->p->sync_every_syms + 2 * AIR_SYNC_SYMS_MAX)
			m->flags |= AIR_CHUNK_FREE_RUN;
		m->gap = false;
		m->sync_at = -1;
	}
	if (sync) {
		m->sync_at = m->n;
		m->sync_id = m->sync_wait_id;
	}
	air_sym_put(m->hard, m->n, d);
	m->soft[m->n] = (int8_t)v;
	m->n++;
	if (m->since_sync != 0xFFFF)
		m->since_sync++;
	if (m->n == AIR_SOFTMODEM_CHUNK)
		flush(m);
}

// A sync's last symbol center is `back` samples back: take its timing, level and offset
static void commit(air_softmodem_t *m, uint8_t back, uint16_t q_next)
{
	const air_sync_t *s = &m->p->sync[m->cand_sync];
	int32_t lvl = m->cand_lvl, off = m->cand_off;
	uint8_t first = (uint8_t)(back + (s->nsyms - 1) * SPS), d;
	int8_t sub = 0;

	// The level far from nominal (a tap of unknown or wrong scale): bring the line and the gain to it, so the
	// samples keep their resolution and their headroom
	if (lvl < LVL_NOM * 4 / 5 || lvl > LVL_NOM * 5 / 4) {
		int32_t f = (LVL_NOM * 256) / lvl;	// x 256

		for (uint8_t i = 0; i < LINE; i++) {
			int32_t x = (m->line[i] * f) / 256;

			m->line[i] = (int8_t)(x > 127 ? 127 : x < -127 ? -127 : x);
		}
		m->gain = (uint32_t)(((uint64_t)m->gain * (uint32_t)f) >> 8);
		m->env = (int32_t)(((int64_t)m->env * 256) / f);
		off = (off * f) / 256;
		lvl = LVL_NOM;
	}
	m->scale = (uint16_t)(((AIR_SOFT_OUTER / 3) * 65536) / lvl);
	m->off = (int16_t)off;
	// Where between the samples the error was least: a parabola through the candidate and its two neighbors
	if (m->cand_prev_q != Q_NONE && q_next != Q_NONE) {
		int32_t a = m->cand_prev_q, b = m->cand_q, c = q_next, curv = a - 2 * b + c;

		if (curv > 0) {
			int32_t x = (8 * (a - c)) / curv;

			sub = (int8_t)(x > 8 ? 8 : x < -8 ? -8 : x);
		}
	}
	// The next symbol to leave moves to the nearest center of the new timing: no symbol is lost or sent twice,
	// whatever the old timing was. It is still ahead of the sync's first symbol (EMIT_AT is past a sync's length)
	d = (uint8_t)((m->due + 5 * SPS - first % SPS) % SPS);
	if (d || sub != m->sub)
		flush(m);		// the symbols before are on the old timing: the next chunk's time is the new one
	m->sub = sub;
	m->due = (uint8_t)(d <= SPS / 2 ? m->due - d : m->due + SPS - d);
	m->sync_wait = (uint8_t)((m->due - first) / SPS + 1);
	m->sync_wait_id = s->id;
	m->since_sync = 0;
	m->locked = true;
	m->cand_q = Q_NONE;
	m->hold = SPS;
}

// Fit the line's newest symbol-spaced samples to every pattern; returns the least error (past Q_MAX too: the sides
// of a peak place its bottom), Q_NONE when no pattern's slope is there
static uint16_t search(const air_softmodem_t *m, uint8_t *which, int16_t *lvl, int16_t *off)
{
	int8_t x[AIR_SYNC_SYMS_MAX];
	int32_t lvl_min = m->gain_known ? LVL_MIN_KNOWN : LVL_MIN_TRACKED;
	int32_t sx = 0, sxx = 0;
	uint16_t best = Q_NONE;
	uint8_t summed = 0;
	uint16_t at = (uint16_t)(m->line_at + LINE - 1) % LINE;

	// x[k]: the sample k symbols back
	for (uint8_t k = 0; k < AIR_SYNC_SYMS_MAX; k++) {
		x[k] = m->line[at];
		at = (uint16_t)(at + LINE - SPS) % LINE;
	}
	for (uint8_t i = 0; i < m->p->nsyncs && i < AIR_SYNCS_MAX; i++) {
		const air_sync_t *s = &m->p->sync[i];
		int32_t n = s->nsyms, sxe = 0, det, num, a, b, r, a2;
		uint32_t q;

		if (!m->s2[i])
			continue;
		if (summed != s->nsyms) {
			sx = sxx = 0;
			for (uint8_t k = 0; k < s->nsyms; k++) {
				sx += x[k];
				sxx += x[k] * x[k];
			}
			summed = s->nsyms;
		}
		for (uint8_t k = 0; k < s->nsyms; k++)
			sxe += x[s->nsyms - 1 - k] * air_sym_level(air_sym_get(s->syms, k));
		det = n * m->s2[i] - m->s1[i] * m->s1[i];
		num = n * sxe - m->s1[i] * sx;
		// Most samples stop here: no slope of the pattern's sign and size
		if (num * 256 < lvl_min * det)
			continue;
		a = (num * 256) / det;				// level, line units x 256
		b = (sx * 256 - a * m->s1[i]) / n;		// offset, x 256
		if (a > LVL_MAX || b > OFF_MAX || b < -OFF_MAX)
			continue;
		r = sxx - (a * sxe + b * sx) / 256;		// what the line leaves: the sum of squared errors
		if (r < 0)
			r = 0;
		a2 = ((a * a) >> 16) * n;
		q = ((uint32_t)r << 8) / (uint32_t)a2;
		if (q >= Q_NONE)
			q = Q_NONE - 1;
		if (q >= best)
			continue;
		best = (uint16_t)q;
		*which = i;
		*lvl = (int16_t)a;
		*off = (int16_t)b;
	}
	return best;
}

// One filtered sample v (summed input counts, before the gain) into the line; t_us as emit's
static void filtered(air_softmodem_t *m, int32_t v, uint32_t t_us)
{
	int32_t y = (int32_t)(((int64_t)v * m->gain) >> 16);
	uint16_t q;
	uint8_t which = 0;
	int16_t lvl = 0, off = 0;

	m->line[m->line_at] = (int8_t)(y > 127 ? 127 : y < -127 ? -127 : y);
	m->line_at = (uint8_t)((m->line_at + 1) % LINE);
	if (m->line_n < LINE) {
		m->line_n++;
		return;
	}
	m->due++;
	q = search(m, &which, &lvl, &off);
	if (m->hold) {
		m->hold--;
	} else if (m->cand_q != Q_NONE) {
		// Climbing a peak: on while the error still falls, taken as soon as it rises
		if (q < m->cand_q) {	// (a candidate is Q_MAX or better)
			m->cand_prev_q = m->cand_q;
			m->cand_q = q;
			m->cand_sync = which;
			m->cand_lvl = lvl;
			m->cand_off = off;
		} else {
			commit(m, 1, q);
		}
	} else if (q <= Q_MAX) {
		m->cand_prev_q = m->last_q;
		m->cand_q = q;
		m->cand_sync = which;
		m->cand_lvl = lvl;
		m->cand_off = off;
	}
	m->last_q = q;
	while (m->due >= EMIT_AT) {
		emit(m, t_us);
		m->due = (uint8_t)(m->due - SPS);
	}
	// A lock ends when the syncs stop coming: the averages follow the input again
	if (m->locked && m->since_sync > (m->p->sync_every_syms ? 2u * m->p->sync_every_syms : 4800u) + 2 * AIR_SYNC_SYMS_MAX)
		m->locked = false;
}

void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us)
{
	uint32_t us_q = 1000000u / m->rate_hz, us_r = 1000000u % m->rate_hz, us_acc = 0;
	uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;
	// From the newest input sample back to the newest filtered one: the interpolator's 47.5 / 12 input samples
	// (less the phase, below) and half the integrate and dump
	uint32_t delay_us = (uint32_t)((47500000ull / 12) / m->rate_hz) + (uint32_t)(m->box - 1) * 500000u / out_rate;

	if (m->running) {
		int32_t d = (int32_t)(t_us - m->t_next_us);

		if (d > HOLE_US || d < -HOLE_US)
			air_softmodem_rx_reset(m);
	}
	m->running = true;
	for (uint16_t i = 0; i < n; i++) {
		int32_t x = samples[i], v = x - (m->dc >> 10);

		if (!m->locked) {
			m->dc += (x * 1024 - m->dc) >> m->track_shift;
			if (!m->gain_known) {
				// A random 4-level signal averages 2 levels in magnitude, a 2-level one its only level, 3
				int32_t mag = v < 0 ? -v : v, target = (m->p->levels == 4 ? 2 : 3) * LINE_LVL;

				m->env += (mag * 1024 - m->env) >> m->track_shift;
				if (m->env < 1024)
					m->env = 1024;
				m->gain = (((uint32_t)target << 26) / (uint32_t)m->env) / m->box;
			}
			if (m->track_shift < TRACK_SHIFT_MAX && --m->track_n == 0) {
				m->track_shift++;
				m->track_n = (uint16_t)(1u << m->track_shift);
			}
		}
		if (v > 32767)
			v = 32767;
		if (v < -32768)
			v = -32768;
		m->in_at = (uint8_t)((m->in_at + 1) % TAPS);
		m->in[m->in_at] = (int16_t)v;
		while (m->pos < m->den) {
			const int16_t *h = interp[(uint32_t)m->pos * PHASES / m->den];
			int32_t y = 0;

			for (uint8_t k = 0; k < TAPS; k++)
				y += h[k] * m->in[(uint8_t)(m->in_at + TAPS - k) % TAPS];
			y >>= 14;
			if (y > 32767)		// (the interpolator overshoots a full scale input)
				y = 32767;
			if (y < -32768)
				y = -32768;
			m->acc += y - m->sum[m->sum_at];
			m->sum[m->sum_at] = (int16_t)y;
			m->sum_at = (uint8_t)((m->sum_at + 1) % m->box);
			filtered(m, m->acc, t_us - delay_us);
			m->pos = (uint16_t)(m->pos + m->step);
		}
		m->pos = (uint16_t)(m->pos - m->den);
		t_us += us_q;
		us_acc += us_r;
		if (us_acc >= m->rate_hz) {
			us_acc -= m->rate_hz;
			t_us++;
		}
	}
	m->t_next_us = t_us;
	flush(m);
}

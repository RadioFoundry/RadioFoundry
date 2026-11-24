// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's receiver (air_softmodem.h, docs/spec/air-interface.md 6).
//
// Input samples -> minus the tap's offset -> polyphase interpolator to five samples per symbol -> integrate and
// dump -> the line (a ring of the last 28 symbols of filtered samples). On every filtered sample the newest symbol
// spaced samples in the line are fitted to each sync pattern (sample = offset + level x pattern); where the fit's
// error passes through a minimum, that sample is a symbol center, and the fit's level and offset are the signal's.
// Symbols leave from the far end of the line, so a sync is sampled on the timing it gave.
//
// The cost is per filtered sample (24,000 a second at 4800 symbols), so that path is kept short (../dsp/fxp.h has
// the rules and the kernels): the fit runs only where a gate of one bit per symbol says a pattern can be there, the
// input history is laid out twice so the interpolator reads one run, and nothing in it divides or needs 64 bits.
#include "air_softmodem.h"

#include <string.h>

#include "../dsp/fxp.h"
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
#define GAIN_MAX	(1u << 20)		// 16 line units per summed count
// The gate (filtered, below): the steps of four levels or more between a pattern's neighboring symbols, as bits.
// A fit with two of them missing (a step the wrong way, or none) is over Q_MAX whatever the rest does: the two cost
// 10.7 levels squared at least, 256 x 10.7 / 24 symbols = 113. So a pattern is fitted only with one missing at most
// (of the 16 newest steps: what the state has room for; random samples pass a pattern with 8 large steps one time
// in 28, one with 11 one time in 170)

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

// The longest run between two syncs the profile sends, any length (0: none recurs)
static uint16_t sync_spacing(const air_profile_t *p)
{
	return p->sync_any_syms ? p->sync_any_syms : p->sync_every_syms;
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

#if CONFIG_AIR_RX_ADAPT
static void adapt_open(air_softmodem_t *m);
static void adapt_reset(air_softmodem_adapt_t *a, bool tone);
#endif

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
	m->ph = 0;
	m->ph_r = 0;
	m->line_at = m->line_n = 0;
	memset(m->steps, 0, sizeof m->steps);
	m->phase = 0;
	m->fitted = false;
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
#if CONFIG_AIR_RX_ADAPT
	if (m->ad)
		adapt_reset(m->ad, false);	// the tone is the channel's, not the signal's: it stays
#endif
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
		outer = ((uint64_t)(3 * LINE_LVL) * 65536u * 1000u) / (outer * m->box);
		m->gain = outer > GAIN_MAX ? GAIN_MAX : (uint32_t)outer;
	} else {
		m->gain = 65536u / m->box;
		m->env = m->env_fast = 3 * LINE_LVL * 1024;
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
		m->valid |= (uint8_t)(1u << i);
		// The gate's bits: bit j is the step from the symbol j + 1 back to the symbol j back
		for (unsigned j = 0; j + 1 < s->nsyms && j < 16; j++) {
			int d = air_sym_level(air_sym_get(s->syms, (uint16_t)(s->nsyms - 1 - j)))
				- air_sym_level(air_sym_get(s->syms, (uint16_t)(s->nsyms - 2 - j)));

			if (d >= 4)
				m->gate_up[i] |= (uint16_t)(1u << j);
			if (d >= 4 || d <= -4)
				m->gate_mask[i] |= (uint16_t)(1u << j);
		}
	}
#if CONFIG_AIR_RX_ADAPT
	adapt_open(m);
#endif
	air_softmodem_rx_reset(m);
	return true;
}

#if CONFIG_AIR_RX_ADAPT
// ── The adaptive receive options (air-interface.md 6.1): only for a profile that asks, in memory its engine lends ──
#define EQ_ONE		16384				// a tap of 1
#define EQ_MID		(AIR_ADAPT_EQ_TAPS / 2)
#define EQ_MU_SHIFT	6				// the step: 2^-6 of the normalized error
#define FRESH_SLACK	(2 * AIR_SYNC_SYMS_MAX)
#define TONE_NONE	0xFF
#define TONE_BLOCK	6				// frames per tone block: 1.7 Hz bins on a 100 ms frame (neighbors 2.9 Hz apart)
#define TONE_RATIO	10				// a tone's power over the mean of the others' (its neighbors left out)
#define NOTCH_Q		20
#define Q30		(1LL << 30)

static const uint16_t tone_dhz[AIR_ADAPT_TONES] = {
	670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072, 1109, 1148,
	1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773,
	1799, 1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

static air_softmodem_adapt_t *lent;
volatile uint8_t air_softmodem_adapt_off;
static air_softmodem_adapt_view_t adapt_view;

// cos of a phase in 1/65536 turn, x 2^30: folded to the first quarter, then the series to x^8 (2.5e-5 off at most),
// so no floating point and no library cosine are built in
static int64_t cos_turn(uint32_t ph)
{
	int64_t t, t2, c;
	int sign = 1;

	ph &= 0xFFFFu;
	if (ph > 32768u)
		ph = 65536u - ph;
	if (ph > 16384u) {
		ph = 32768u - ph;
		sign = -1;
	}
	t = (int64_t)ph * 102944;		// radians x 2^30: 2 pi x 2^30 / 65536
	t2 = (t * t) >> 30;			// x^2 <= (pi/2)^2 x 2^30
	// Horner: 1 - x^2/2 (1 - x^2/12 (1 - x^2/30 (1 - x^2/56))). Each factor stays below 2^30, so no product
	// passes 2^62 (x^4 times x^2 on its own would overflow near a quarter turn)
	c = Q30 - t2 / 56;
	c = Q30 - ((t2 * c) >> 30) / 30;
	c = Q30 - ((t2 * c) >> 30) / 12;
	c = Q30 - ((t2 * c) >> 30) / 2;
	return sign * c;
}

// A frequency in Hz x 10 at a rate as a phase step in 1/65536 turn
static uint32_t turn_of(uint32_t dhz, uint32_t rate, uint32_t times)
{
	return (uint32_t)(((uint64_t)dhz * times * 65536u) / (10u * rate));
}

void air_softmodem_lend(air_softmodem_adapt_t *a)
{
	lent = a;
}

void air_softmodem_adapt_get(air_softmodem_adapt_view_t *out)
{
	*out = adapt_view;
}

// The equalizer a pass-through: a new lock owes nothing to the last signal's channel. The tone search and the notch
// (tone) only when the session opens: the tone a repeater sends belongs to the channel and outlasts every lock, and
// a strong one costs locks itself, so a search that started over at each never reached its second block, and the
// notch that would have kept the lock went with it
static void adapt_reset(air_softmodem_adapt_t *a, bool tone)
{
	memset(a->eq_w, 0, sizeof a->eq_w);
	a->eq_w[EQ_MID] = EQ_ONE;
	memset(a->eq_past, 0, sizeof a->eq_past);
	adapt_view.eq_center = EQ_ONE;
	// The frame before is lost either way (the place in the frame may have moved): nothing is compared against it
	// until a frame has passed again. What the tone filters summed and the notch stay
	memset(a->hist, 0xFF, sizeof a->hist);
	a->at = 0;
	a->e_ok = false;
	if (!tone)
		return;
	a->ctx = 0;
	a->e_prev = 0;
	a->e_mean = 0;
	memset(a->s1, 0, sizeof a->s1);
	memset(a->s2, 0, sizeof a->s2);
	a->n = 0;
	a->ci1 = a->ci2 = a->cc1 = a->cc2 = 0;
	a->dn = 0;
	a->cand = a->sel = TONE_NONE;
	a->x1 = a->x2 = a->y1 = a->y2 = 0;
	adapt_view.tone_dhz = 0;
}

// At open: the tones' filter coefficients for this symbol rate and frame, from the profile
static void adapt_open(air_softmodem_t *m)
{
	air_softmodem_adapt_t *a = lent;
	uint16_t period = sync_spacing(m->p);

	m->ad = NULL;
	memset(&adapt_view, 0, sizeof adapt_view);
	if (!a || !m->p->rx_opts)
		return;
	memset(a, 0, sizeof *a);
	a->opts = m->p->rx_opts;
	a->period = period <= AIR_ADAPT_PERIOD_MAX ? period : 0;
	if (!a->period)
		a->opts &= (uint8_t)~AIR_RX_TONE;	// no frame to difference against
	for (uint8_t k = 0; k < AIR_ADAPT_TONES; k++) {
		int64_t c = cos_turn(turn_of(tone_dhz[k], m->p->sym_rate, AIR_ADAPT_DECIM));
		int64_t gs = AIR_ADAPT_DECIM * Q30;				// the sum's, |sum of e^-jwn|^2 x 2^30

		for (uint8_t j = 1; j < AIR_ADAPT_DECIM; j++)
			gs += 2 * (AIR_ADAPT_DECIM - j) * cos_turn(turn_of(tone_dhz[k], m->p->sym_rate, j));
		gs /= AIR_ADAPT_DECIM * AIR_ADAPT_DECIM;			// x 2^30, 1 at DC
		gs = (gs * gs) >> 30;						// the CIC is two such sums
		a->coef[k] = (int16_t)((2 * c + (1 << 17)) >> 18);		// x 4096
		// The CIC's droop toward the high tones is weighed in, so the tones compete on what the signal holds
		a->gain[k] = (uint16_t)(((gs >> 18) + 1) ?: 1);
	}
	adapt_reset(a, true);
	m->ad = a;
	adapt_view.opts = a->opts;
}

// The timing is a sync's, not coasting on an old one
static bool fresh(const air_softmodem_t *m)
{
	return m->since_sync != 0xFFFF && m->since_sync <= (uint32_t)sync_spacing(m->p) + FRESH_SLACK;
}

static int decide(const air_softmodem_t *m, int32_t v)
{
	if (m->p->levels == 2)
		return v >= 0 ? AIR_SOFT_OUTER : -AIR_SOFT_OUTER;
	return v >= 2 * AIR_SOFT_OUTER / 3 ? AIR_SOFT_OUTER : v >= 0 ? AIR_SOFT_OUTER / 3 :
	       v >= -2 * AIR_SOFT_OUTER / 3 ? -AIR_SOFT_OUTER / 3 : -AIR_SOFT_OUTER;
}

// The tone search's block: the strongest tone, weighted by what the difference does at it, against the rest. A
// tone that wins two blocks running is notched; a neighbor of the one notched takes over only at twice its power
static void tone_block(air_softmodem_t *m)
{
	air_softmodem_adapt_t *a = m->ad;
	int64_t p[AIR_ADAPT_TONES], sum = 0;
	uint8_t best = TONE_NONE, nsum = 0;

	for (uint8_t k = 0; k < AIR_ADAPT_TONES; k++) {
		int64_t s1 = a->s1[k], s2 = a->s2[k];
		int64_t pw = s1 * s1 + s2 * s2 - ((a->coef[k] * s1) >> 12) * s2;

		p[k] = a->gain[k] && pw > 0 ? (pw << 12) / a->gain[k] : -1;
		if (p[k] >= 0 && (best == TONE_NONE || p[k] > p[best]))
			best = k;
		a->s1[k] = a->s2[k] = 0;
	}
	adapt_view.tone_blocks++;
	if (best == TONE_NONE)
		return;
	for (uint8_t k = 0; k < AIR_ADAPT_TONES; k++) {
		if (p[k] < 0 || k + 1 == best || k == best || k == best + 1)
			continue;
		sum += p[k];
		nsum++;
	}
	if (!nsum || p[best] * nsum <= TONE_RATIO * sum) {
		a->cand = TONE_NONE;
		return;
	}
	if (a->cand != best) {
		a->cand = best;
		return;
	}
	if (best == a->sel)
		return;
	if (a->sel != TONE_NONE && (best + 1 == a->sel || best == a->sel + 1) && p[best] <= 2 * p[a->sel])
		return;
	// The notch: zeros on the unit circle at the tone, poles just inside (Q 20), unity gain at DC
	{
		uint32_t ph = turn_of(tone_dhz[best], m->rate_hz, 1);
		int64_t c = cos_turn(ph), w = (int64_t)ph * 102944;			// x 2^30
		int64_t r = Q30 - w / (2 * NOTCH_Q);
		int64_t a1 = -((2 * r * c) >> 30), a2 = (r * r) >> 30;
		int64_t g = ((Q30 + a1 + a2) << 30) / (2 * Q30 - 2 * c);

		a->nb0 = (int32_t)(g >> 16);						// x 2^14
		a->nb1 = (int32_t)(-((2 * c * g) >> 30) >> 16);
		a->na1 = (int32_t)(a1 >> 16);
		a->na2 = (int32_t)(a2 >> 16);
		a->x1 = a->x2 = a->y1 = a->y2 = 0;
	}
	a->sel = best;
	adapt_view.tone_dhz = tone_dhz[best];
	adapt_view.tone_picks++;
}

// One symbol into the search. Its slicing error counts only where the neighborhood (this symbol and the two before)
// differs from the same place a frame before: what repeats every frame (the sync, a FICH, a transmission of silence)
// makes a comb at the frame rate, and leaving it out removes the comb at its source, where differencing against the
// frame before would also cancel every tone near a multiple of the frame rate (100.0 Hz is one). The error used is the
// middle symbol's, the one its neighborhood centers on; the slow mean comes off first, so the places left out don't
// make a comb of it. Then a second-order CIC (two sums of AIR_ADAPT_DECIM symbols, one output each AIR_ADAPT_DECIM):
// CTCSS tones stay under 254 Hz, so the filters run at a quarter of the symbol rate (1200 Hz at 4800 symbols/s), 50
// updates every 4 symbols, and what folds onto the tones comes from above 940 Hz, held off by more than 20 dB
_Static_assert(AIR_ADAPT_DECIM == 4, "the CIC's gain out with >> 4: 2 log2(AIR_ADAPT_DECIM)");
static void tone_symbol(air_softmodem_t *m, int32_t v, bool fresh_timing)
{
	air_softmodem_adapt_t *a = m->ad;
	int dec = decide(m, v);
	int32_t err = v - dec, e = err > 127 ? 127 : err < -127 ? -127 : err, x = 0;
	uint8_t sym = (uint8_t)(dec > 0 ? (dec > AIR_SOFT_OUTER / 2 ? 3 : 2) : (dec < -AIR_SOFT_OUTER / 2 ? 0 : 1));
	uint8_t was = a->hist[a->at];

	a->ctx = (uint8_t)(((a->ctx << 2) | sym) & 0x3F);
	a->hist[a->at] = fresh_timing ? (uint8_t)(a->ctx | 0x40) : 0xFF;
	a->at = (uint16_t)(a->at + 1 == a->period ? 0 : a->at + 1);
	if (fresh_timing && a->e_ok) {
		a->e_mean += a->e_prev - (a->e_mean >> 8);		// x 256, a time constant of 256 symbols
		if (was != 0xFF && was != (uint8_t)(a->ctx | 0x40))
			x = a->e_prev - (a->e_mean >> 8);
	}
	a->e_prev = (int8_t)e;
	a->e_ok = fresh_timing;
	a->ci1 += (uint32_t)x;
	a->ci2 += a->ci1;
	if (++a->dn >= AIR_ADAPT_DECIM) {
		// The integrators wrap; the combs' differences are exact all the same. Out of the CIC's gain (4 x 4) again,
		// |d| <= 254: the filters' sums stay in 32 bits over a block (coef x s1 under 2^31)
		uint32_t c1 = a->ci2 - a->cc1;
		int32_t d = (int32_t)(c1 - a->cc2) >> 4;

		a->cc1 = a->ci2;
		a->cc2 = c1;
		a->dn = 0;
		for (uint8_t k = 0; k < AIR_ADAPT_TONES; k++) {
			int32_t s0 = d + ((a->coef[k] * a->s1[k]) >> 12) - a->s2[k];

			a->s2[k] = a->s1[k];
			a->s1[k] = s0;
		}
	}
	if (++a->n >= TONE_BLOCK * a->period) {
		a->n = 0;
		tone_block(m);
	}
}

// The notch on one input sample, once a tone is picked. 32 bits, as the rest of the sample path (6): the samples
// at half scale, the coefficients x 2^14 (|b1|, |a1| under 2), so no sum of the five products passes 2^31
#define NOTCH_HALF	16383
static int32_t notch(air_softmodem_adapt_t *a, int32_t x)
{
	int32_t xs = x / 2, y;

	y = (a->nb0 * (xs + a->x2) + a->nb1 * a->x1 - a->na1 * a->y1 - a->na2 * a->y2) >> 14;
	y = y > NOTCH_HALF ? NOTCH_HALF : y < -NOTCH_HALF ? -NOTCH_HALF : y;
	a->x2 = a->x1;
	a->x1 = xs;
	a->y2 = a->y1;
	a->y1 = y;
	return 2 * y;
}
#endif

static int line_back(const air_softmodem_t *m, unsigned back)
{
	int at = (int)m->line_at - 1 - (int)back;

	return m->line[at < 0 ? at + LINE : at];
}

#if CONFIG_AIR_RX_ADAPT
// Symbol-centered sample `back` samples back, on the symbol's own fraction (as emit takes it), in soft units
static int32_t soft_at(const air_softmodem_t *m, unsigned back)
{
	int32_t x = line_back(m, back) * 16;

	if (m->sub > 0)
		x += m->sub * (line_back(m, back - 1u) - line_back(m, back));
	else if (m->sub < 0)
		x -= m->sub * (line_back(m, back + 1u) - line_back(m, back));
	return ((x - m->off / 16) * (int32_t)m->scale) / 4096;
}

// The equalizer on the symbol whose soft value is v (due samples back): the symbols before it from its own memory,
// those after it from the line. Decision directed, normalized LMS towards the level decided, only on fresh timing.
// The center tap stays within 0.7-1.2: free, it has a second resting point near 0.4, where the outer levels fall on
// the inner ones and every decision holds it there. A side tap past 0.5 is a runaway: back to a pass-through
static int32_t equalize(air_softmodem_t *m, int32_t v)
{
	air_softmodem_adapt_t *a = m->ad;
	int32_t x[AIR_ADAPT_EQ_TAPS], y = 0, norm = 64;	// (64: a step that stays small on a weak sample)

	for (unsigned k = 0; k < EQ_MID; k++) {
		x[EQ_MID - 1 - k] = a->eq_past[k];
		x[EQ_MID + 1 + k] = soft_at(m, m->due - (k + 1) * SPS);
	}
	x[EQ_MID] = v;
	for (unsigned k = 0; k < AIR_ADAPT_EQ_TAPS; k++) {
		y += a->eq_w[k] * x[k];
		norm += x[k] * x[k];
	}
	y /= EQ_ONE;
	if (fresh(m)) {
		// One divide a symbol: the step x 2^16, then each tap's share (mu x e x x / norm, x 16384)
		int32_t g = (decide(m, y) - y) * 65536 / norm;
		bool runaway = false;

		for (unsigned k = 0; k < AIR_ADAPT_EQ_TAPS; k++) {
			int32_t w = a->eq_w[k] + ((g * x[k]) >> (16 - 14 + EQ_MU_SHIFT));

			if (k != EQ_MID && (w > EQ_ONE / 2 || w < -EQ_ONE / 2))
				runaway = true;
			a->eq_w[k] = (int16_t)(w > 32767 ? 32767 : w < -32767 ? -32767 : w);
		}
		if (a->eq_w[EQ_MID] < EQ_ONE * 7 / 10)
			a->eq_w[EQ_MID] = EQ_ONE * 7 / 10;
		if (a->eq_w[EQ_MID] > EQ_ONE * 12 / 10)
			a->eq_w[EQ_MID] = EQ_ONE * 12 / 10;
		if (runaway) {
			memset(a->eq_w, 0, sizeof a->eq_w);
			a->eq_w[EQ_MID] = EQ_ONE;
			adapt_view.eq_resets++;
		}
		adapt_view.eq_center = a->eq_w[EQ_MID];
	}
	memmove(&a->eq_past[1], &a->eq_past[0], (EQ_MID - 1) * sizeof a->eq_past[0]);
	a->eq_past[0] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
	return y;
}
#endif

static void flush(air_softmodem_t *m)
{
	uint8_t hard[AIR_SOFTMODEM_CHUNK / 4];
	air_chunk_t c;

	if (!m->n)
		return;
	memset(hard, 0, sizeof hard);
	for (unsigned i = 0; i < m->n; i++) {
		int v = m->soft[i];

		air_sym_put(hard, (uint16_t)i, m->p->levels == 2 ? (v >= 0 ? 1 : 3)
			    : v >= 2 * AIR_SOFT_OUTER / 3 ? 1 : v >= 0 ? 0 : v >= -2 * AIR_SOFT_OUTER / 3 ? 2 : 3);
	}
	c.t_us = m->t_chunk_us;
	c.nsyms = m->n;
	c.sync_at = m->sync_at;
	c.sync_id = m->sync_id;
	c.flags = m->flags;
	c.rssi_dbm = 0;
	c.hard = hard;
	c.soft = m->soft;
	m->n = 0;
	m->out(&c, m->ctx);
}

// The symbol whose center is due samples back leaves the line. t_us: the time of the input sample in hand, less the
// filters' delay
static void emit(air_softmodem_t *m, uint32_t t_us)
{
	int32_t x = line_back(m, m->due) * 16, v;
	bool sync;

	// Between two samples: straight line to the neighbor on the side the fit's minimum leaned to
	if (m->sub > 0)
		x += m->sub * (line_back(m, m->due - 1u) - line_back(m, m->due));
	else if (m->sub < 0)
		x -= m->sub * (line_back(m, m->due + 1u) - line_back(m, m->due));
	v = ((x - m->off / 16) * (int32_t)m->scale) / 4096;
#if CONFIG_AIR_RX_ADAPT
	if (m->ad) {
		uint8_t on = (uint8_t)(m->ad->opts & ~air_softmodem_adapt_off);

		adapt_view.opts = on;
		if (on & AIR_RX_EQ)
			v = equalize(m, v);
		// Syncs and coasting give no error worth having: zeros, as the frame they leave out
		if (on & AIR_RX_TONE)
			tone_symbol(m, v, fresh(m));
	}
#endif
	if (v > 127)
		v = 127;
	if (v < -127)
		v = -127;
	sync = m->sync_wait && --m->sync_wait == 0;
	if (sync && m->n && m->sync_at >= 0)
		flush(m);		// a chunk names one sync
	if (!m->n) {
		uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;

		// The line's newest sample sits pos / den of an input sample after t_us (an input sample in 1/16 us)
		m->t_chunk_us = t_us + (uint32_t)m->pos * (16000000u / m->rate_hz) / ((uint32_t)m->den * 16u)
				- (uint32_t)(((uint32_t)m->due * 16 - m->sub) * 62500u / out_rate);
		m->flags = m->gap ? AIR_CHUNK_GAP : 0;
		// Past the profile's sync spacing with none seen (or none ever): the timing is the last one's, or none.
		// (since_sync starts when a sync is found, two sync lengths before its symbols are all out)
		if (m->since_sync > (uint32_t)sync_spacing(m->p) + 2 * AIR_SYNC_SYMS_MAX)
			m->flags |= AIR_CHUNK_FREE_RUN;
		m->gap = false;
		m->sync_at = -1;
	}
	if (sync) {
		m->sync_at = m->n;
		m->sync_id = m->sync_wait_id;
	}
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

#if CONFIG_AIR_RX_ADAPT
	if (m->ad && !fresh(m))
		adapt_reset(m->ad, false);	// a new lock: the equalizer from a pass-through, the tone kept
#endif
	// The level far from nominal (a tap of unknown or wrong scale): bring the line and the gain to it, so the
	// samples keep their resolution and their headroom
	if (lvl < LVL_NOM * 4 / 5 || lvl > LVL_NOM * 5 / 4) {
		int32_t f = (LVL_NOM * 256) / lvl;	// x 256

		for (unsigned i = 0; i < LINE; i++)
			m->line[i] = (int8_t)fxp_clamp((m->line[i] * f) / 256, -127, 127);
		m->gain = (m->gain * (uint32_t)f) >> 8;		// (f is 1024 at most: 32 bits hold it)
		if (m->gain > GAIN_MAX)
			m->gain = GAIN_MAX;
		m->env = (m->env / f) * 256 + ((m->env % f) * 256) / f;
		m->env_fast = (m->env_fast / f) * 256 + ((m->env_fast % f) * 256) / f;
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

// Fit the symbol-spaced samples that end `back` samples before the line's newest to every pattern; returns the
// least error (past Q_MAX too: the sides of a peak place its bottom), Q_NONE when no pattern's slope is there
static uint16_t search(const air_softmodem_t *m, unsigned back, uint8_t *which, int16_t *lvl, int16_t *off)
{
	int8_t x[AIR_SYNC_SYMS_MAX];
	const air_profile_t *p = m->p;
	int32_t lvl_min = m->gain_known ? LVL_MIN_KNOWN : LVL_MIN_TRACKED;
	int32_t sx = 0, sxx = 0;
	uint16_t best = Q_NONE;
	unsigned summed = 0, squared = 0, valid = m->valid;
	int at = (int)m->line_at - 1 - (int)back;

	// x[k]: the sample k symbols back
	if (at < 0)
		at += LINE;
	for (unsigned k = 0; k < AIR_SYNC_SYMS_MAX; k++) {
		x[k] = m->line[at];
		at -= SPS;
		if (at < 0)
			at += LINE;
	}
	for (unsigned i = 0; valid >> i; i++) {
		const air_sync_t *s = &p->sync[i];
		int32_t n = s->nsyms, sxe = 0, s1 = 0, s2 = 0, det, num, a, b, r, a2;
		uint32_t q;

		if (!(valid >> i & 1))
			continue;
		if (summed != s->nsyms) {
			sx = 0;
			for (int k = 0; k < n; k++)
				sx += x[k];
			summed = s->nsyms;
		}
		for (int k = 0; k < n; k++) {
			int e = air_sym_level(air_sym_get(s->syms, (uint16_t)k));

			sxe += x[n - 1 - k] * e;
			s1 += e;
			s2 += e * e;
		}
		det = n * s2 - s1 * s1;
		num = n * sxe - s1 * sx;
		// Most samples stop here: no slope of the pattern's sign and size
		if (num * 256 < lvl_min * det)
			continue;
		a = (num * 256) / det;				// level, line units x 256
		b = (sx * 256 - a * s1) / n;			// offset, x 256
		if (a > LVL_MAX || b > OFF_MAX || b < -OFF_MAX)
			continue;
		if (squared != s->nsyms) {
			sxx = 0;
			for (int k = 0; k < n; k++)
				sxx += x[k] * x[k];
			squared = s->nsyms;
		}
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
		*which = (uint8_t)i;
		*lvl = (int16_t)a;
		*off = (int16_t)b;
	}
	return best;
}

// Can a pattern end at the newest sample: all its large steps there but one (steps: the sample's phase's bits)
static bool gate(const air_softmodem_t *m, unsigned steps)
{
	for (unsigned i = 0, valid = m->valid; valid; i++, valid >>= 1) {
		unsigned v = (steps ^ m->gate_up[i]) & m->gate_mask[i];

		if ((valid & 1) && !(v & (v - 1)))
			return true;
	}
	return false;
}

// One filtered sample v (summed input counts, before the gain) into the line; t_us as emit's
static void filtered(air_softmodem_t *m, int32_t v, uint32_t t_us)
{
	int32_t y = fxp_clamp(fxp_scale16(v, m->gain), -127, 127);
	unsigned at = m->line_at, ph = m->phase;
	unsigned steps = ((unsigned)m->steps[ph] << 1 | (unsigned)(y > m->line[at >= SPS ? at - SPS : at + LINE - SPS])) & 0xFFFFu;
	uint16_t q = Q_NONE;
	uint8_t which = 0;
	int16_t lvl = 0, off = 0;

	m->line[at] = (int8_t)y;
	m->line_at = (uint8_t)(at + 1 == LINE ? 0 : at + 1);
	m->steps[ph] = (uint16_t)steps;
	m->phase = (uint8_t)(ph + 1 == SPS ? 0 : ph + 1);
	if (m->line_n < LINE) {
		m->line_n++;
		return;
	}
	m->due++;
	// The fit, where it can matter: on a peak being climbed, and where the gate lets a pattern through. A peak's
	// near side is the sample before, fitted now if it was not then
	if (!m->hold && (m->cand_q != Q_NONE || gate(m, steps))) {
		q = search(m, 0, &which, &lvl, &off);
		if (!m->fitted && q <= Q_MAX) {
			uint8_t w;
			int16_t l, o;

			m->last_q = search(m, 1, &w, &l, &o);
		}
		m->fitted = true;
	} else {
		m->fitted = false;
	}
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
	if (m->locked && m->since_sync > (sync_spacing(m->p) ? 2u * sync_spacing(m->p) : 4800u) + 2 * AIR_SYNC_SYMS_MAX)
		m->locked = false;
}

void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us)
{
	uint32_t us_q = 1000000u / m->rate_hz, us_r = 1000000u % m->rate_hz, us_acc = 0;
	uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;
	// From the newest input sample back to the newest filtered one: the interpolator's 47.5 / 12 input samples
	// (less the phase, below) and half the integrate and dump
	uint32_t delay_us = (uint32_t)((47500000ull / 12) / m->rate_hz) + (uint32_t)(m->box - 1) * 500000u / out_rate;
	int32_t target = (m->p->levels == 4 ? 2 : 3) * LINE_LVL;
	unsigned box = m->box, den = m->den;
	// What a filtered sample's step adds to the interpolator phase: whole phases, and the rest in 1/den
	unsigned ph_step = (unsigned)m->step * PHASES / den, ph_rstep = (unsigned)m->step * PHASES % den;

	if (m->running) {
		int32_t d = (int32_t)(t_us - m->t_next_us);

		if (d > HOLE_US || d < -HOLE_US)
			air_softmodem_rx_reset(m);
	}
	m->running = true;
	for (unsigned i = 0; i < n; i++) {
		int32_t x = samples[i], v;
#if CONFIG_AIR_RX_ADAPT
		if (m->ad && m->ad->sel != TONE_NONE && !(air_softmodem_adapt_off & AIR_RX_TONE))
			x = fxp_sat16(notch(m->ad, x));
#endif
		v = x - (m->dc >> 10);
		unsigned at = (m->in_at + 1u) & (TAPS - 1u);

		if (!m->locked) {
			m->dc += (x * 1024 - m->dc) >> m->track_shift;
			if (!m->gain_known) {
				// A random 4-level signal averages 2 levels in magnitude, a 2-level one its only level, 3
				int32_t mag = v < 0 ? -v : v;

				m->env += (mag * 1024 - m->env) >> m->track_shift;
				m->env_fast += (mag * 1024 - m->env_fast) >> TRACK_SHIFT_MIN;
				if (m->env < 1024)
					m->env = 1024;
				// The level stepped by half or double (a signal after a quiet carrier, or the other way): the
				// slow averages would take hundreds of ms to follow, the gain clipping or starving every symbol
				// meanwhile. They start over at their fastest
				if (m->env_fast > 2 * m->env || 2 * m->env_fast < m->env) {
					m->env = m->env_fast < 1024 ? 1024 : m->env_fast;
					m->track_shift = TRACK_SHIFT_MIN;
					m->track_n = 1u << TRACK_SHIFT_MIN;
				}
				m->gain = (((uint32_t)target << 26) / (uint32_t)m->env) / box;
			}
			if (m->track_shift < TRACK_SHIFT_MAX && --m->track_n == 0) {
				m->track_shift++;
				m->track_n = (uint16_t)(1u << m->track_shift);
			}
		}
		// Twice, TAPS apart: the last TAPS samples are in[at + 1 ..], oldest first, whatever at is
		m->in[at] = m->in[at + TAPS] = fxp_sat16(v);
		m->in_at = (uint8_t)at;
		while (m->pos < den) {
			// (a row read backward is the mirror phase's row: the table is symmetric)
			int32_t y = fxp_sat16(fxp_dot16_8(interp[PHASES - 1 - m->ph], &m->in[at + 1]) >> 14);
			unsigned sa = m->sum_at, r = m->ph_r + ph_rstep;

			m->acc += y - m->sum[sa];
			m->sum[sa] = (int16_t)y;
			m->sum_at = (uint8_t)(sa + 1 == box ? 0 : sa + 1);
			filtered(m, m->acc, t_us - delay_us);
			m->pos = (uint16_t)(m->pos + m->step);
			m->ph = (uint8_t)(m->ph + ph_step);
			if (r >= den) {
				r -= den;
				m->ph++;
			}
			m->ph_r = (uint16_t)r;
		}
		m->pos = (uint16_t)(m->pos - den);
		m->ph = (uint8_t)(m->ph - PHASES);
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

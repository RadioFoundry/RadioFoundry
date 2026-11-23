// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's transmitter (air_softmodem.h, docs/spec/air-interface.md 6.2).
//
// The response of one symbol at the tap's input is the profile's pulse convolved with the tap's equalizer: built at
// open into a polyphase table, phase-major, AIR_SOFTMODEM_TX_COEFS samples of it where its energy is. Per output
// sample the history of levels weighs a row of it: 32 multiply-adds at 5 samples a symbol, 16 at 10. The levels come
// through the tap's low-end filter first (tx_lf, at the symbol rate: up to 128 multiply-adds a symbol), x16. The
// build runs once per transmission and may use 64 bits; the per-sample path doesn't.
#include "air_softmodem.h"

#include <string.h>

#include "air_syms.h"

// The pulses, ten samples a symbol over eight symbols each side, Q14, the taps summing to ten (a run of one symbol
// sits at its level). tools/air_pulses.py prints them: the formulas are the bench tools' (p25tool, nxdntool)
#define AIR_PULSE_OSR	10
#define AIR_PULSE_SPAN	8
#define AIR_PULSE_TAPS	(2 * AIR_PULSE_SPAN * AIR_PULSE_OSR + 1)

// C4FM (TIA-102.BAAA-A 9.3): raised cosine 0.2 times the shaper (pi f T) / sin(pi f T)
static const int16_t pulse_c4fm[AIR_PULSE_TAPS] = {
	16, 28, 34, 32, 23, 9, -8, -25, -38, -44, -39, -23,
	4, 38, 77, 115, 144, 159, 154, 126, 74, 0, -89, -184,
	-275, -349, -393, -397, -354, -262, -124, 49, 244, 439, 613, 742,
	805, 786, 677, 479, 201, -136, -500, -855, -1161, -1379, -1473, -1420,
	-1207, -838, -335, 265, 908, 1532, 2069, 2450, 2619, 2532, 2167, 1528,
	647, -418, -1581, -2738, -3774, -4570, -5013, -5011, -4497, -3439, -1844, 238,
	2719, 5474, 8351, 11183, 13798, 16030, 17737, 18809, 19174, 18809, 17737, 16030,
	13798, 11183, 8351, 5474, 2719, 238, -1844, -3439, -4497, -5011, -5013, -4570,
	-3774, -2738, -1581, -418, 647, 1528, 2167, 2532, 2619, 2450, 2069, 1532,
	908, 265, -335, -838, -1207, -1420, -1473, -1379, -1161, -855, -500, -136,
	201, 479, 677, 786, 805, 742, 613, 439, 244, 49, -124, -262,
	-354, -397, -393, -349, -275, -184, -89, 0, 74, 126, 154, 159,
	144, 115, 77, 38, 4, -23, -39, -44, -38, -25, -8, 9,
	23, 32, 34, 28, 16,
};
// NXDN (TS 1-A 3.4): root raised cosine 0.2 times the shaper sin(pi f T) / (pi f T)
static const int16_t pulse_nxdn[AIR_PULSE_TAPS] = {
	-22, -1, 21, 40, 55, 62, 60, 50, 34, 12, -11, -33,
	-49, -58, -58, -47, -28, -4, 23, 47, 65, 73, 68, 50,
	20, -19, -60, -98, -127, -140, -133, -104, -54, 12, 88, 164,
	230, 274, 287, 263, 200, 101, -27, -170, -312, -434, -519, -549,
	-514, -409, -238, -13, 245, 509, 746, 924, 1013, 987, 835, 554,
	161, -315, -828, -1323, -1733, -1993, -2037, -1813, -1282, -426, 750, 2215,
	3913, 5766, 7679, 9549, 11265, 12726, 13840, 14538, 14776, 14538, 13840, 12726,
	11265, 9549, 7679, 5766, 3913, 2215, 750, -426, -1282, -1813, -2037, -1993,
	-1733, -1323, -828, -315, 161, 554, 835, 987, 1013, 924, 746, 509,
	245, -13, -238, -409, -514, -549, -519, -434, -312, -170, -27, 101,
	200, 263, 287, 274, 230, 164, 88, 12, -54, -104, -133, -140,
	-127, -98, -60, -19, 20, 50, 68, 73, 65, 47, 23, -4,
	-28, -47, -58, -58, -49, -33, -11, 12, 34, 50, 60, 62,
	55, 40, 21, -1, -22,
};

#define EQ_ONE		16384		// Q14
#define LEVEL_ONE	16		// hist's scale (shr_round by 4)
#define LEVEL_MAX	(64 * LEVEL_ONE)	// a level out of the low-end filter at most: 32 taps of hist times a
					// coefficient stay in 32 bits
#define BINS_MAX	(AIR_SOFTMODEM_TX_COEFS / 2)

typedef struct {
	const int16_t *pulse;		// every step-th tap of a pulse table: sps samples a symbol
	uint8_t step;
	int16_t center;			// the pulse's center, in output samples
	int16_t npulse;
	const int16_t *eq;
	int16_t eq_n, eq_at;
} resp_t;

static const int16_t eq_flat = EQ_ONE;

static const int16_t *pulse_of(const air_profile_t *p)
{
	if (p->shape == AIR_SHAPE_C4FM && p->shape_param == 20)
		return pulse_c4fm;
	if (p->shape == AIR_SHAPE_RRC && p->shape_param == 20)
		return pulse_nxdn;
	return NULL;
}

uint32_t air_softmodem_tx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	uint32_t sps;

	if (!bb || !bb->tx_start || !bb->tx_write || !bb->tx_stop || !bb->tx_counts_per_khz || !p->sym_rate)
		return 0;
	if ((p->levels != 2 && p->levels != 4) || p->timing != AIR_TIMING_CONTINUOUS || !pulse_of(p))
		return 0;
	if (bb->tx_rate_hz % p->sym_rate)
		return 0;
	sps = bb->tx_rate_hz / p->sym_rate;
	if (sps != 5 && sps != 10)
		return 0;
	if (bb->tx_eq && (!bb->tx_eq_n || bb->tx_eq_at >= bb->tx_eq_n || bb->tx_eq_n > 255))
		return 0;
	// A low-end filter at the profile's rate, or at twice it (its taps in pairs), within the ring
	if (bb->tx_lf) {
		uint32_t step = bb->tx_lf_rate == p->sym_rate ? 1 : bb->tx_lf_rate == 2u * p->sym_rate ? 2 : 0;

		if (!step || !bb->tx_lf_n || bb->tx_lf_at >= bb->tx_lf_n || (bb->tx_lf_n + step - 1) / step > AIR_SOFTMODEM_TX_LF)
			return 0;
	}
	// The band, as receive asks of a tap: a little past half the symbol rate, and down to where a run of one level
	// holds
	if (bb->tx_hi_hz < (uint32_t)p->sym_rate * 11 / 20 || bb->tx_lo_hz > p->sym_rate / (p->levels == 4 ? 100 : 20))
		return 0;
	return AIR_FIT_TX;
}

// The response at output sample tau (0: the symbol's center), Q28
static int64_t resp(const resp_t *r, int32_t tau)
{
	int64_t acc = 0;

	for (int32_t j = 0; j < r->npulse; j++) {
		int32_t q = tau + r->center + r->eq_at - j;

		if (q >= 0 && q < r->eq_n)
			acc += (int32_t)r->pulse[j * r->step] * r->eq[q];
	}
	return acc;
}

// a / b rounded to nearest, b > 0
static int64_t div_round(int64_t a, int64_t b)
{
	return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

// a / 2^s rounded (the per-sample path's: no divide)
static inline int32_t shr_round(int32_t a, uint8_t s)
{
	return (a + (1 << (s - 1))) >> s;
}

bool air_softmodem_tx_open(air_softmodem_tx_t *m, const air_profile_t *p, const hal_baseband_t *bb)
{
	resp_t r;
	int32_t tau_min, tau_max, b0, nbins, best_lead = 0;
	uint32_t best = 0;
	int64_t scale;

	if (!air_softmodem_tx_fit(p, bb))
		return false;
	memset(m, 0, sizeof *m);
	m->sps = (uint8_t)(bb->tx_rate_hz / p->sym_rate);
	m->span = (uint8_t)(AIR_SOFTMODEM_TX_COEFS / m->sps);
	r.pulse = pulse_of(p);
	r.step = (uint8_t)(AIR_PULSE_OSR / m->sps);
	r.center = (int16_t)(AIR_PULSE_SPAN * m->sps);
	r.npulse = (int16_t)(2 * r.center + 1);
	r.eq = bb->tx_eq ? bb->tx_eq : &eq_flat;
	r.eq_n = (int16_t)(bb->tx_eq ? bb->tx_eq_n : 1);
	r.eq_at = (int16_t)(bb->tx_eq ? bb->tx_eq_at : 0);
	// Where the table goes: the window of span symbols with the most energy, from energies per symbol-long bin (in
	// the coefficients' memory, which is filled after). Bin b holds tau from (b - b0) x sps on
	tau_min = -(r.center + r.eq_at);
	tau_max = r.center + r.eq_n - 1 - r.eq_at;
	b0 = (-tau_min + m->sps - 1) / m->sps;
	nbins = (tau_max + b0 * m->sps) / m->sps + 1;
	if (nbins > BINS_MAX)
		nbins = BINS_MAX;
	for (int32_t b = 0; b < nbins; b++) {
		uint32_t e = 0;

		for (int32_t k = 0; k < m->sps; k++) {
			int32_t v = (int32_t)(resp(&r, (b - b0) * m->sps + k) >> 18);	// Q10

			e += (uint32_t)(v * v);
		}
		m->scratch[b] = e;
	}
	for (int32_t lead = 0; lead <= m->span; lead++) {
		uint32_t e = 0;

		for (int32_t i = 0; i < m->span; i++) {
			int32_t b = b0 - lead + i;

			if (b >= 0 && b < nbins)
				e += m->scratch[b];
		}
		if (e > best) {
			best = e;
			best_lead = lead;
		}
	}
	m->lead = (uint8_t)best_lead;
	// Q28 response to counts: the outer level is dev_outer_hz, a level unit a third of it
	scale = (int64_t)3000 << 28;
	for (int32_t ph = 0; ph < m->sps; ph++)
		for (int32_t i = 0; i < m->span; i++) {
			int64_t v = div_round(resp(&r, (i - m->lead) * m->sps + ph) * p->dev_outer_hz * bb->tx_counts_per_khz,
					      scale);

			m->coef[ph * m->span + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	// The low-end filter's look-ahead delays the output too
	if (bb->tx_lf) {
		m->lf = bb->tx_lf;
		m->lf_n = bb->tx_lf_n;
		m->lf_step = (uint8_t)(bb->tx_lf_rate / p->sym_rate);
		m->lead = (uint8_t)(m->lead + bb->tx_lf_at / m->lf_step);
	}
	return true;
}

// The level into the table for the symbol just in: through the low-end filter (tap i weighs the symbol i back from the
// newest, a pair of taps a symbol at half the filter's rate), x16
static int16_t lf_level(air_softmodem_tx_t *m, int8_t level)
{
	uint8_t at = (uint8_t)((m->lv_at + 1) & (AIR_SOFTMODEM_TX_LF - 1));
	int32_t acc = 0;

	m->lv_at = at;
	m->lv[at] = level;
	if (!m->lf)
		return (int16_t)(level * LEVEL_ONE);
	for (uint8_t i = 0, j = 0; i < m->lf_n; i = (uint8_t)(i + m->lf_step), j++) {
		int32_t k = m->lf[i];

		if (m->lf_step == 2 && i + 1 < m->lf_n)
			k += m->lf[i + 1];
		acc += k * m->lv[(uint8_t)(at - j) & (AIR_SOFTMODEM_TX_LF - 1)];
	}
	acc = shr_round(acc, 10);		// Q14 to x16
	return (int16_t)(acc > LEVEL_MAX ? LEVEL_MAX : acc < -LEVEL_MAX ? -LEVEL_MAX : acc);
}

uint8_t air_softmodem_tx_sym(air_softmodem_tx_t *m, uint8_t dibit, int16_t *out)
{
	uint8_t mask = (uint8_t)(m->span - 1), at = (uint8_t)((m->hist_at + 1) & mask);
	const int16_t *c = m->coef;

	m->hist_at = at;
	m->hist[at] = lf_level(m, (int8_t)(dibit == AIR_SOFTMODEM_TX_NONE ? 0 : air_sym_level(dibit)));
	for (uint8_t ph = 0; ph < m->sps; ph++, c += m->span) {
		int32_t acc = 0;

		for (uint8_t i = 0; i < m->span; i++)
			acc += (int32_t)m->hist[(at - i) & mask] * c[i];
		acc = shr_round(acc, 4);	// x16 to counts
		out[ph] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
	}
	return m->sps;
}

uint8_t air_softmodem_tx_tail(const air_softmodem_tx_t *m)
{
	uint8_t lf = m->lf ? (uint8_t)((m->lf_n + m->lf_step - 1) / m->lf_step - 1) : 0;

	return (uint8_t)(m->span - 1 + lf);
}

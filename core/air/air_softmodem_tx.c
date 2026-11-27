// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's transmitter (air_softmodem.h, docs/spec/air-interface.md 6.1): symbols to samples for a board's
// transmit tap, each symbol's level times the profile's frequency pulse, summed. The pulse and the arithmetic are
// D-STAR's GMSK modulator's (dstar-modem.md 2.2), which now runs on this one: one copy of the pulse for every mode
// with a Gaussian profile.
//
// Taken by name in a build with D-STAR and no air interface (the Makefile), so it uses nothing else of core/air.
#include "air_softmodem.h"

#include <string.h>

// The frequency pulse of a Gaussian profile at BT = 0.5 (a one-symbol rectangle through the Gaussian filter) from 1.5
// symbols before a symbol's middle to 1.5 after, 64 points a symbol and both ends, interpolated between points; a long
// run of one level sums to 32767 within 3. tools/dstar_ref.py pulse makes it; dstarModemTest checks it against libm's
// erf. Past 1.5 symbols the pulse is under 1e-4 of its peak, so three symbols make every sample
static const int16_t gauss50[193] = {
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

// The pulses written: Gaussian at BT 0.5. Others (root raised cosine, C4FM) are not: a provider sends those through
// its chip
static const int16_t *pulse_of(const air_profile_t *p)
{
	return p->shape == AIR_SHAPE_GAUSS && p->shape_param == 50 ? gauss50 : NULL;
}

static int8_t next_sym(air_softmodem_tx_t *m)
{
	int s = m->pull ? m->pull(m->ctx) : AIR_SOFTMODEM_NO_SYM;

	if (s == AIR_SOFTMODEM_NO_SYM) {
		m->tail++;
		return 0;
	}
	m->syms++;
	return (int8_t)s;
}

uint32_t air_softmodem_tx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	if (!bb || !bb->tx_start || !bb->tx_write || !bb->tx_counts_per_khz || !pulse_of(p))
		return 0;
	if (p->levels != 2 && p->levels != 4)
		return 0;
	if (bb->tx_rate_hz < AIR_SOFTMODEM_TX_RATE_MIN || bb->tx_rate_hz > AIR_SOFTMODEM_TX_RATE_MAX)
		return 0;
	// The band, as for receive: a little past half the symbol rate, and low enough for a run of one level
	if (bb->tx_hi_hz < (uint32_t)p->sym_rate * 11 / 20)
		return 0;
	if (bb->tx_lo_hz > p->sym_rate / (p->levels == 4 ? 100 : 20))
		return 0;
	return AIR_FIT_TX;
}

bool air_softmodem_tx_init(air_softmodem_tx_t *m, const air_profile_t *p, uint32_t rate_hz, int16_t amp,
			   air_softmodem_sym_t pull, void *ctx)
{
	memset(m, 0, sizeof *m);
	m->pulse = pulse_of(p);
	if (!m->pulse || rate_hz < AIR_SOFTMODEM_TX_RATE_MIN || rate_hz > AIR_SOFTMODEM_TX_RATE_MAX)
		return false;
	m->step = (uint32_t)(((uint64_t)p->sym_rate << 32) / rate_hz);
	m->amp = p->levels == 4 ? (int16_t)(amp / 3) : amp;
	m->pull = pull;
	m->ctx = ctx;
	// Nothing before the first symbol; the first current and the second next
	m->sym[1] = next_sym(m);
	m->sym[2] = next_sym(m);
	return true;
}

bool air_softmodem_tx_open(air_softmodem_tx_t *m, const air_profile_t *p, const hal_baseband_t *bb,
			   air_softmodem_sym_t pull, void *ctx)
{
	int32_t amp;

	if (!air_softmodem_tx_fit(p, bb))
		return false;
	amp = (int32_t)bb->tx_counts_per_khz * p->dev_outer_hz / 1000;
	if (bb->tx_invert)
		amp = -amp;
	if (amp > 32767 || amp < -32767)
		return false;
	return air_softmodem_tx_init(m, p, bb->tx_rate_hz, (int16_t)amp, pull, ctx);
}

void air_softmodem_tx_trim(air_softmodem_tx_t *m, int32_t ppm)
{
	m->step = (uint32_t)((int64_t)m->step + ((int64_t)m->step * ppm) / 1000000);
}

bool air_softmodem_tx_sample(air_softmodem_tx_t *m, int32_t *out)
{
	uint32_t i, fr, was;
	int32_t y = 0;

	// Done once the source has run dry and the last real symbol's pulse is past: two symbols after it went out of
	// the current slot
	if (m->tail >= 3)
		return false;
	// The phase's top six bits pick the pulse point, the next 16 the way to the following one: the current symbol
	// is 64-128 into the table, the previous one 128-192, the next 0-64. Without the interpolation the nearest point
	// is up to 2.4 % of the deviation off
	i = m->phase >> 26;
	fr = (m->phase >> 10) & 0xFFFFu;
	for (int b = 0; b < 3; b++) {
		uint32_t at = i + 128u - 64u * (uint32_t)b;
		int32_t q = m->pulse[at] + (int32_t)((((int32_t)m->pulse[at + 1u] - m->pulse[at]) * (int32_t)fr) >> 16);
		y += m->sym[b] * q;
	}
	*out = (int32_t)(((int64_t)y * m->amp) >> 15);
	m->samples++;
	was = m->phase;
	m->phase += m->step;
	if (m->phase < was) {
		m->sym[0] = m->sym[1];
		m->sym[1] = m->sym[2];
		m->sym[2] = m->tail ? (m->tail++, (int8_t)0) : next_sym(m);
	}
	return true;
}

size_t air_softmodem_tx(air_softmodem_tx_t *m, int16_t *out, size_t max)
{
	size_t n = 0;
	int32_t v;

	while (n < max && air_softmodem_tx_sample(m, &v))
		out[n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	return n;
}

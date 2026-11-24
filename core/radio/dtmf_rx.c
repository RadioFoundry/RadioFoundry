// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DTMF receive (dtmf_rx.h): Hann-windowed Goertzel filters on the Q.23 frequencies, in integers
#include "config.h"
#if CONFIG_DTMF_RX
#include <string.h>
#include "radio/dtmf_rx.h"
#include "hal_dtmf.h"

static const uint16_t tone_hz[DTMF_RX_TONES] = {
	DTMF_LOW_1, DTMF_LOW_2, DTMF_LOW_3, DTMF_LOW_4, DTMF_HIGH_1, DTMF_HIGH_2, DTMF_HIGH_3, DTMF_HIGH_4,
};
static const char keys[] = "123A456B789C*0#D";	// row (low tone) by column (high tone), as hal_dtmf.h

#define Q30		(1ll << 30)
#define PI_Q30		3373259426ll	// pi, Q30
#define TWO_PI_Q30	6746518852ll

// cos(x) for 0 <= x <= pi (x in Q30): the Taylor series to x^14 on 0 .. pi / 2, far below a Q14 step there, and
// cos(x) = -cos(pi - x) above
static int64_t cos_q30(int64_t x)
{
	bool flip = x > PI_Q30 / 2;
	if (flip) {
		x = PI_Q30 - x;
	}
	int64_t x2 = (x * x) >> 30;
	int64_t term = Q30, sum = Q30;
	for (int64_t k = 1; k <= 7; k++) {
		term = -((term * x2) >> 30) / ((2 * k - 1) * (2 * k));
		sum += term;
	}
	return flip ? -sum : sum;
}

// cos(x) for 0 <= x <= 2 pi
static int64_t cos_turn(int64_t x)
{
	return cos_q30(x <= PI_Q30 ? x : TWO_PI_Q30 - x);
}

// A fresh window, starting at sample n of it (its window's cosine there)
static void bank_clear(dtmf_rx_t *d, uint8_t b, uint16_t n)
{
	memset(d->win[b].s1, 0, sizeof(d->win[b].s1));
	memset(d->win[b].s2, 0, sizeof(d->win[b].s2));
	d->win[b].energy = 0;
	d->win[b].n = n;
	d->win[b].c = cos_turn(TWO_PI_Q30 * n / d->len);
	d->win[b].c_prev = cos_turn(TWO_PI_Q30 * (n != 0 ? n - 1u : d->len - 1u) / d->len);
}

// The window's next weight, Q15 (Hann: (1 - cos) / 2), and its cosine moved on a sample
static int32_t hann_next(dtmf_rx_t *d, uint8_t b)
{
	int64_t c = d->win[b].c;
	d->win[b].c = ((d->hann_k * c) >> 30) - d->win[b].c_prev;
	d->win[b].c_prev = c;
	return (int32_t)((Q30 - c) >> 16);
}

void dtmf_rx_reset(dtmf_rx_t *d)
{
	bank_clear(d, 0, 0);
	bank_clear(d, 1, (uint16_t)(d->len / 2u));	// half a window ahead: a result from each bank in turn
	d->seen = 0;
	d->same = 0;
	d->on = 0;
}

void dtmf_rx_init(dtmf_rx_t *d, uint32_t fs_hz)
{
	memset(d, 0, sizeof(*d));
	d->len = (uint16_t)(((uint64_t)fs_hz * DTMF_RX_WINDOW_US + 500000u) / 1000000u);
	for (uint8_t i = 0; i < DTMF_RX_TONES; i++) {
		d->coef[i] = (int32_t)((2 * cos_q30(TWO_PI_Q30 * tone_hz[i] / (int64_t)fs_hz)) >> 16);
	}
	d->hann_k = 2 * cos_q30(TWO_PI_Q30 / d->len);
	// The window's sums: a tone of amplitude A gives the power (A sum(w) / 2)^2 and the energy A^2 sum(w^2) / 2
	bank_clear(d, 0, 0);
	int64_t s1 = 0, s2 = 0;
	for (uint16_t k = 0; k < d->len; k++) {
		int64_t w = hann_next(d, 0);
		s1 += w;
		s2 += (w * w) >> 15;
	}
	int64_t a = (int64_t)DTMF_RX_MIN_AMP * s1 / 65536;
	d->min_pow = a * a;
	d->share_k = ((2 * s2) << 39) / (s1 * s1);
	dtmf_rx_reset(d);
}

// The digit a full window holds, 0 for none
static char classify(const dtmf_rx_t *d, uint8_t b)
{
	int64_t p[DTMF_RX_TONES];
	for (uint8_t i = 0; i < DTMF_RX_TONES; i++) {
		int64_t s1 = d->win[b].s1[i], s2 = d->win[b].s2[i];
		p[i] = s1 * s1 + s2 * s2 - (((int64_t)d->coef[i] * s1) >> 14) * s2;
	}
	uint8_t lo = 0, hi = 4;
	for (uint8_t i = 1; i < 4; i++) {
		if (p[i] > p[lo]) {
			lo = i;
		}
		if (p[4 + i] > p[hi]) {
			hi = (uint8_t)(4 + i);
		}
	}
	if (p[lo] < d->min_pow || p[hi] < d->min_pow) {
		return 0;
	}
	// Twist within 8 dB (a power ratio of 6.31) either way
	if (p[hi] * 100 > p[lo] * 631 || p[lo] * 100 > p[hi] * 631) {
		return 0;
	}
	// Each 6 dB (4x) over the rest of its group
	for (uint8_t i = 0; i < 4; i++) {
		if ((i != lo && p[i] * 4 > p[lo]) || (4 + i != hi && p[4 + i] * 4 > p[hi])) {
			return 0;
		}
	}
	// The two tones' share of the window's energy (share_k turns a tone's power into its energy)
	if (((p[lo] + p[hi]) >> 8) * d->share_k * 100 < ((d->win[b].energy >> 8) * DTMF_RX_SHARE_PCT) << 24) {
		return 0;
	}
	return keys[lo * 4 + (hi - 4)];
}

// One window's verdict; true (and the digit in *out) when it accepts a digit
static bool result(dtmf_rx_t *d, char c, char *out)
{
	if (c == d->seen) {
		if (d->same < 255) {
			d->same++;
		}
	} else {
		d->seen = c;
		d->same = 1;
	}
	if (d->same < (c != 0 ? DTMF_RX_ON_RESULTS : DTMF_RX_OFF_RESULTS)) {
		return false;
	}
	if (c == 0) {
		d->on = 0;
		return false;
	}
	if (d->on == c) {
		return false;
	}
	d->on = c;
	d->digits++;
	*out = c;
	return true;
}

size_t dtmf_rx_feed(dtmf_rx_t *d, const int16_t *s, size_t n, char *out, size_t max)
{
	size_t got = 0;
	for (size_t k = 0; k < n; k++) {
		d->dc += ((int32_t)s[k] * 256 - d->dc) / 128;
		int32_t x0 = (int32_t)s[k] - (d->dc >> 8);
		for (uint8_t b = 0; b < 2; b++) {
			int32_t x = (int32_t)(((int64_t)x0 * hann_next(d, b)) >> 15);
			for (uint8_t i = 0; i < DTMF_RX_TONES; i++) {
				int32_t v = x + (int32_t)(((int64_t)d->coef[i] * d->win[b].s1[i]) >> 14) - d->win[b].s2[i];
				d->win[b].s2[i] = d->win[b].s1[i];
				d->win[b].s1[i] = v;
			}
			d->win[b].energy += (int64_t)x * x;
			if (++d->win[b].n >= d->len) {
				char c;
				if (result(d, classify(d, b), &c) && got < max) {
					out[got++] = c;
				}
				bank_clear(d, b, 0);
			}
		}
	}
	return got;
}
#endif

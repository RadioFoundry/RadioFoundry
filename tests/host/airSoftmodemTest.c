// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's receiver (core/air/air_softmodem.c) on a modeled channel: symbols through the mode's transmit
// pulse to a deviation waveform, a band-limited tap sampled at its own rate and phase, with a scale error, an
// offset and noise. It checks the symbols and soft values that come out, the times on them, that a whole P25 call
// decodes through the framer behind it, that noise makes no sync, and what a hole in the samples does.
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "radio/tones.h"
#include "p25/p25.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xC0FFEE;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static double gauss(void)
{
	double u = (rng() % 1000000 + 1) / 1000001.0, v = (rng() % 1000000) / 1000000.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void fill(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = (uint8_t)rng();
}

// ── The channel model, at 240 kHz ──
#define FS		240000
#define SPAN		8			// symbols of pulse each side
#define SYMS_MAX	12000
#define WAVE_MAX	(SYMS_MAX * 100 + 4000)
#define LEAD		2000			// model samples before symbol 0
#define LPF_HALF	120

static double wave[WAVE_MAX];			// deviation, Hz
static uint32_t wave_n;
static uint8_t air[SYMS_MAX];			// one dibit per byte
static uint32_t air_n;
static double lpf[2 * LPF_HALF + 1];
static int16_t tap[WAVE_MAX / 5];
static uint32_t tap_n;

// The transmit pulse for one symbol level, osr model samples per symbol, DC gain 1 (a run of one symbol sits at
// its nominal deviation)
static void make_pulse(double *h, int osr, uint8_t shape, double alpha)
{
	int n = 2 * SPAN * osr + 1;
	double rate = (double)FS / osr, sum = 0;

	for (int i = 0; i < n; i++) {
		double t = (double)(i - SPAN * osr) / FS, v = 0;

		if (shape == AIR_SHAPE_C4FM) {
			// TIA-102.BAAA-A 9.3: the raised cosine H(f) times the shaper P(f) = (pi f T) / sin(pi f T)
			double f1 = rate * (1 - alpha) / 2, f2 = rate * (1 + alpha) / 2;

			for (double f = 2.5; f < f2; f += 5) {
				double hf = f < f1 ? 1 : 0.5 + 0.5 * cos(M_PI * (f - f1) / (f2 - f1));
				double x = M_PI * f / rate;

				v += hf * x / sin(x) * cos(2 * M_PI * f * t);
			}
		} else if (shape == AIR_SHAPE_RRC) {
			double x = t * rate;

			if (fabs(x) < 1e-9)
				v = 1 - alpha + 4 * alpha / M_PI;
			else if (fabs(fabs(4 * alpha * x) - 1) < 1e-6)
				v = alpha / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
			else
				v = (sin(M_PI * x * (1 - alpha)) + 4 * alpha * x * cos(M_PI * x * (1 + alpha))) /
				    (M_PI * x * (1 - (4 * alpha * x) * (4 * alpha * x)));
		} else {
			v = abs(i - SPAN * osr) * 2 < osr ? 1 : 0;	// a square pulse
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
}

// air[] to wave[]: the symbol clock ppm fast
static void modulate(const air_profile_t *p, double ppm)
{
	static double h[2 * SPAN * 100 + 1];
	int osr = FS / p->sym_rate;

	make_pulse(h, osr, p->shape, p->shape_param / 100.0);
	wave_n = LEAD + air_n * (uint32_t)osr + LEAD;
	memset(wave, 0, wave_n * sizeof(double));
	for (uint32_t k = 0; k < air_n; k++) {
		long at = LEAD + lround(k * osr / (1 + ppm * 1e-6)) - SPAN * osr;
		double lv = air_sym_level(air[k]) * (p->dev_outer_hz / 3.0);

		for (int i = 0; i <= 2 * SPAN * osr; i++)
			if (at + i >= 0 && at + i < (long)wave_n)
				wave[at + i] += lv * h[i];
	}
}

// The model sample of symbol k's center
static double sym_at(const air_profile_t *p, uint32_t k)
{
	return LEAD + (double)k * (FS / p->sym_rate);
}

// wave[] through the tap: its low-pass (hi_hz), sampled at rate from model sample phase on, counts_per_khz, an
// offset in counts and noise (sigma, counts)
static void sample_tap(uint32_t rate, uint32_t phase, double hi_hz, double counts_per_khz, double off, double sigma)
{
	uint32_t dec = FS / rate;
	double sum = 0;

	for (int i = -LPF_HALF; i <= LPF_HALF; i++) {
		double x = 2 * M_PI * hi_hz / FS * i, w = 0.54 + 0.46 * cos(M_PI * i / LPF_HALF);

		lpf[i + LPF_HALF] = (i ? sin(x) / x : 1) * w;
		sum += lpf[i + LPF_HALF];
	}
	tap_n = 0;
	for (uint32_t j = phase + LPF_HALF; j + LPF_HALF < wave_n; j += dec) {
		double v = 0;

		for (int i = -LPF_HALF; i <= LPF_HALF; i++)
			v += wave[j + (uint32_t)i] * lpf[i + LPF_HALF];
		v = v / sum / 1000 * counts_per_khz + off + sigma * gauss();
		tap[tap_n++] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

// The time of model sample j on the tap's clock (tap sample 0 is at T0)
#define T0	0xFFFF0000u		// the clock wraps inside every run
static double model_us(uint32_t phase, double j)
{
	return (j - phase - LPF_HALF) * 1e6 / FS;
}

// ── What comes out ──
#define OUT_MAX		(SYMS_MAX + 2000)
static uint8_t out_hard[OUT_MAX];
static int8_t out_soft[OUT_MAX];
static uint32_t out_n, out_total, out_chunks;
static struct { uint32_t at; uint8_t id; double t_us; } out_sync[64];
static uint8_t out_nsync;
static uint8_t out_flags_or, out_flags_and;
static uint32_t out_gap_chunks;
static air_framer_t *out_framer;

static void on_chunk(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	CHECK(c->nsyms >= 1 && c->nsyms <= AIR_SOFTMODEM_CHUNK && c->hard && c->soft && c->sync_at < (int)c->nsyms, "chunk shape");
	out_chunks++;
	out_total += c->nsyms;
	out_flags_or |= c->flags;
	out_flags_and &= c->flags;
	if (c->flags & AIR_CHUNK_GAP)
		out_gap_chunks++;
	if (c->sync_at >= 0 && out_nsync < 64) {
		out_sync[out_nsync].at = out_n + (uint32_t)c->sync_at;
		out_sync[out_nsync].id = c->sync_id;
		out_sync[out_nsync].t_us = (double)(uint32_t)(c->t_us - T0);
		out_sync[out_nsync].t_us += c->sync_at * 1e6 / 4800;	// the callers that read it run 4800 symbols/s
		out_nsync++;
	}
	for (uint16_t i = 0; i < c->nsyms && out_n < OUT_MAX; i++) {
		out_hard[out_n] = air_sym_get(c->hard, i);
		out_soft[out_n++] = c->soft[i];
	}
	if (out_framer)
		air_framer_rx(out_framer, c);
}

static void out_reset(void)
{
	out_n = out_total = out_chunks = out_gap_chunks = 0;
	out_nsync = 0;
	out_flags_or = 0;
	out_flags_and = 0xFF;
}

// The tap's samples into the modem, in calls of the size a board's buffer gives
static void feed(air_softmodem_t *m, uint32_t rate, uint32_t from, uint32_t to, uint16_t per_call)
{
	while (from < to) {
		uint16_t n = (uint16_t)(to - from < per_call ? to - from : per_call);

		air_softmodem_rx(m, tap + from, n, T0 + (uint32_t)(((uint64_t)from * 1000000) / rate));
		from += n;
	}
}

// ── Profiles ──
static air_profile_t dmr_like(void)
{
	air_profile_t p;

	memset(&p, 0, sizeof(p));
	p.sym_rate = 4800;
	p.levels = 4;
	p.shape = AIR_SHAPE_RRC;
	p.shape_param = 20;
	p.dev_outer_hz = 1944;
	p.sync_every_syms = 288;
	p.nsyncs = 2;
	p.sync[0].id = 7;
	p.sync[0].nsyms = 24;
	memcpy(p.sync[0].syms, (uint8_t[]){ 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 }, 6);
	p.sync[1].id = 9;
	p.sync[1].nsyms = 24;
	memcpy(p.sync[1].syms, (uint8_t[]){ 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D }, 6);
	return p;
}

// Random symbols with sync pattern s of profile p every `every` symbols, the first at `first`
static void air_stream(const air_profile_t *p, uint8_t s, uint32_t n, uint32_t first, uint32_t every)
{
	air_n = 0;
	for (uint32_t k = 0; k < n; k++)
		air[air_n++] = p->levels == 2 ? (uint8_t)(rng() & 2) | 1 : (uint8_t)(rng() & 3);
	for (uint32_t at = first; at + p->sync[s].nsyms <= n; at += every)
		for (uint8_t i = 0; i < p->sync[s].nsyms; i++)
			air[at + i] = air_sym_get(p->sync[s].syms, i);
}

// Symbols out against the air from the first sync found on: errors, and the soft values' RMS distance from their
// level in soft units. False when no sync was found where one was sent
static bool compare(const air_profile_t *p, uint32_t first, uint32_t every, uint32_t *nerr, uint32_t *ncmp, double *rms)
{
	uint32_t o, a;
	double se = 0;

	*nerr = *ncmp = 0;
	*rms = 0;
	if (!out_nsync)
		return false;
	// Which sent sync it is, from the tap's time: the modem's delay is a few symbols, the spacing hundreds
	o = out_sync[0].at;
	a = first;
	if (every) {
		double t = out_sync[0].t_us * p->sym_rate / 1e6;	// in symbols from tap sample 0, about

		a = first + (uint32_t)lround((t - first - (double)LEAD / (FS / p->sym_rate)) / every) * every;
	}
	for (; o < out_n && a < air_n; o++, a++) {
		int want = air_sym_level(air[a]) * (AIR_SOFT_OUTER / 3);

		(*ncmp)++;
		if (out_hard[o] != air[a])
			(*nerr)++;
		se += (double)(out_soft[o] - want) * (out_soft[o] - want);
	}
	*rms = *ncmp ? sqrt(se / *ncmp) : 0;
	return true;
}

static void testFit(void)
{
	hal_baseband_t bb;
	air_profile_t p = p25_air_mode.profile, q;
	air_softmodem_t m;

	memset(&bb, 0, sizeof(bb));
	bb.rx_rate_hz = 8000;
	bb.rx_channels = 1;
	bb.rx_hi_hz = 3400;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "no tap functions: no fit");
	bb.rx_start = (bool (*)(void))abort;
	bb.rx_read = (uint16_t (*)(int16_t *, uint16_t))abort;
	CHECK(air_softmodem_rx_fit(&p, &bb) == (AIR_FIT_RX | AIR_FIT_SOFT), "8 kHz DC coupled tap carries P25");
	bb.rx_hi_hz = 2500;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "a tap cut at 2.5 kHz can't carry 4800 symbols/s");
	bb.rx_hi_hz = 3400;
	bb.rx_lo_hz = 300;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "an audio band tap (300 Hz up) can't carry four levels");
	q = p;
	q.levels = 2;
	q.sym_rate = 1200;
	bb.rx_lo_hz = 50;
	CHECK(air_softmodem_rx_fit(&q, &bb) != 0, "two levels at 1200 symbols/s ride a 50 Hz corner");
	q.sym_rate = 600;
	CHECK(air_softmodem_rx_fit(&q, &bb) == 0, "8 kHz is past twice the filtered rate of 600 symbols/s");
	bb.rx_lo_hz = 0;
	bb.rx_channels = 2;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "a split band is not written");
	bb.rx_channels = 1;
	bb.rx_rate_hz = 7000;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "7 kHz: too slow");
	bb.rx_rate_hz = 48000;
	CHECK(air_softmodem_rx_fit(&p, &bb) != 0, "48 kHz fits");
	bb.rx_rate_hz = 96000;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "96 kHz needs a decimator the board must do");
	CHECK(!air_softmodem_rx_open(&m, &p, 96000, 0, on_chunk, NULL) && air_softmodem_rx_open(&m, &p, 8000, 0, on_chunk, NULL), "open follows the rate");
	CHECK(sizeof(air_softmodem_t) <= 400, "state is %zu bytes", sizeof(air_softmodem_t));	// (three 8-byte pointers here)
}

// A stream of P25-shaped symbols at every tap phase and a few tap rates: no symbol wrong, soft values on their
// levels, every sync reported at its symbol, times right
static void testClean(void)
{
	static const uint32_t rates[] = { 8000, 16000, 24000, 48000 };
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;
	double worst_rms = 0, worst_t = 0;

	air_stream(&p, 0, 2600, 100, 864);
	modulate(&p, 0);
	for (uint8_t r = 0; r < 4; r++) {
		for (uint32_t phase = 0; phase < FS / rates[r]; phase += (r ? 3 : 1)) {
			uint32_t nerr, ncmp;
			double rms;

			sample_tap(rates[r], phase, 3600, 2000, 0, 0);
			CHECK(air_softmodem_rx_open(&m, &p, rates[r], 2000, on_chunk, NULL), "open");
			out_reset();
			feed(&m, rates[r], 0, tap_n, 256);
			CHECK(out_nsync == 3, "rate %u phase %u: %u syncs", rates[r], phase, out_nsync);
			if (!compare(&p, 100, 864, &nerr, &ncmp, &rms))
				continue;
			CHECK(nerr == 0 && ncmp > 2400, "rate %u phase %u: %u errors in %u", rates[r], phase, nerr, ncmp);
			CHECK(rms < 7, "rate %u phase %u: soft RMS error %.1f", rates[r], phase, rms);
			if (rms > worst_rms)
				worst_rms = rms;
			for (uint8_t i = 0; i < out_nsync; i++) {
				double want = model_us(phase, sym_at(&p, 100 + 864u * i)), d = out_sync[i].t_us - want;

				if (fabs(d) > worst_t)
					worst_t = fabs(d);
				CHECK(fabs(d) < 12 && out_sync[i].id == 0, "rate %u phase %u sync %u: time off %.0f us", rates[r], phase, i, d);
				CHECK(i == 0 || out_sync[i].at - out_sync[i - 1].at == 864, "sync spacing %u", out_sync[i].at - out_sync[i - 1].at);
			}
			// The first chunk says a gap and no timing; the stream after the first sync says neither
			CHECK(out_gap_chunks == 1 && (out_flags_or & AIR_CHUNK_FREE_RUN) && !(out_flags_and & AIR_CHUNK_FREE_RUN), "flags or %x and %x gaps %u", out_flags_or, out_flags_and, out_gap_chunks);
		}
	}
	printf("  clean: worst soft RMS error %.1f (of %d a level), worst sync time error %.0f us\n", worst_rms, AIR_SOFT_OUTER / 3, worst_t);
}

// Scale and offset errors, a tap of unknown scale, a slow and a fast symbol clock, small calls
static void testLevels(void)
{
	static const struct { double cpk, off, ppm; uint16_t told, per_call; const char *what; } t[] = {
		{ 2000, 0, 0, 2000, 256, "nominal" },
		{ 2600, 0, 0, 2000, 256, "30 % hot" },
		{ 1300, 0, 0, 2000, 256, "35 % low" },
		{ 2000, 1500, 0, 2000, 256, "+750 Hz offset" },
		{ 2000, -1500, 0, 2000, 256, "-750 Hz offset" },
		{ 2300, 9000, 0, 2000, 256, "a converter offset of 4.5 kHz worth, hot" },
		{ 2000, 0, 0, 0, 256, "unknown scale" },
		{ 300, -200, 0, 0, 256, "unknown scale, small" },
		{ 9000, 3000, 0, 0, 256, "unknown scale, large" },
		{ 2000, 0, 40, 2000, 256, "symbol clock 40 ppm fast" },
		{ 2000, 0, -40, 2000, 256, "symbol clock 40 ppm slow" },
		{ 2000, 300, 0, 2000, 7, "calls of 7 samples" },
	};
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;

	air_stream(&p, 0, 4400, 900, 864);
	for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		uint32_t nerr, ncmp;
		double rms;

		modulate(&p, t[i].ppm);
		sample_tap(8000, 11, 3600, t[i].cpk, t[i].off, 0);
		CHECK(air_softmodem_rx_open(&m, &p, 8000, t[i].told, on_chunk, NULL), "open");
		out_reset();
		feed(&m, 8000, 0, tap_n, t[i].per_call);
		CHECK(compare(&p, 900, 864, &nerr, &ncmp, &rms), "%s: no sync", t[i].what);
		// A clock 40 ppm off moves 0.035 of a symbol between two syncs: no errors still
		CHECK(out_nsync >= 4 && nerr == 0 && ncmp > 3000 && rms < 9, "%s: %u syncs, %u errors in %u, RMS %.1f", t[i].what, out_nsync, nerr, ncmp, rms);
	}
}

// Noise on the tap: the symbol error rate stays near what the levels allow, and syncs are still found
static void testNoise(void)
{
	// sigma in counts at 2000 counts/kHz: a level is 600 Hz = 1200 counts
	static const struct { double sigma; double ser_max; } t[] = { { 150, 0.0005 }, { 300, 0.006 }, { 450, 0.04 }, { 600, 0.10 } };
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;

	air_stream(&p, 0, 11000, 500, 864);
	modulate(&p, 0);
	for (uint8_t i = 0; i < 4; i++) {
		uint32_t nerr, ncmp;
		double rms;

		sample_tap(8000, 17, 3600, 2000, 400, t[i].sigma);
		air_softmodem_rx_open(&m, &p, 8000, 2000, on_chunk, NULL);
		out_reset();
		feed(&m, 8000, 0, tap_n, 256);
		CHECK(compare(&p, 500, 864, &nerr, &ncmp, &rms), "sigma %.0f: no sync", t[i].sigma);
		printf("  noise %.0f counts: %u syncs of 13, symbol errors %u of %u (%.2f %%), soft RMS %.1f\n", t[i].sigma, out_nsync, nerr, ncmp, ncmp ? 100.0 * nerr / ncmp : 0, rms);
		CHECK(ncmp > 9000 && (double)nerr / ncmp <= t[i].ser_max, "sigma %.0f: %u errors in %u", t[i].sigma, nerr, ncmp);
		CHECK(out_nsync >= (i < 3 ? 13 : 8), "sigma %.0f: %u syncs", t[i].sigma, out_nsync);
	}
	// Nothing but noise, 12 s of it, at every scale the level tracker meets: no sync
	for (uint8_t k = 0; k < 2; k++) {
		air_softmodem_rx_open(&m, &p, 8000, k ? 0 : 2000, on_chunk, NULL);
		out_reset();
		for (uint8_t rep = 0; rep < 12; rep++) {
			for (uint32_t i = 0; i < 8000; i++)
				tap[i] = (int16_t)lround(3000 * gauss());
			air_softmodem_rx(&m, tap, 8000, T0 + rep * 1000000u);
		}
		CHECK(out_nsync == 0 && out_total > 57000 && (out_flags_and & AIR_CHUNK_FREE_RUN), "noise only (%u): %u syncs, %u symbols", k, out_nsync, out_total);
	}
}

// The gate only saves work: with it, and with every sample fitted, the same symbols, soft values and syncs come out
static void testGate(void)
{
	static uint8_t ref_hard[OUT_MAX];
	static int8_t ref_soft[OUT_MAX];
	static const double sigma[] = { 0, 300, 600, 900, 1500, 3000 };
	uint32_t runs = 0;

	for (uint8_t prof = 0; prof < 2; prof++) {
		air_profile_t p = prof ? dmr_like() : p25_air_mode.profile;
		air_softmodem_t m;

		air_stream(&p, prof, 6000, 300, prof ? 288 : 864);
		modulate(&p, 0);
		for (uint8_t i = 0; i < 6; i++) {
			for (uint8_t known = 0; known < 2; known++) {
				uint32_t ref_n, ref_at[64], diff = 0;
				uint8_t ref_nsync;
				double ref_t[64];

				sample_tap(8000, 5 + i, 3600, known ? 2000 : 700, 400, sigma[i]);
				CHECK(air_softmodem_rx_open(&m, &p, 8000, known ? 2000 : 0, on_chunk, NULL), "open");
				CHECK(m.gate_mask[0] != 0, "profile %u has a gate", prof);
				memset(m.gate_mask, 0, sizeof(m.gate_mask));	// no gate: every sample is fitted
				out_reset();
				feed(&m, 8000, 0, tap_n, 256);
				ref_n = out_n;
				ref_nsync = out_nsync;
				memcpy(ref_hard, out_hard, out_n);
				memcpy(ref_soft, out_soft, out_n);
				for (uint8_t k = 0; k < out_nsync; k++) {
					ref_at[k] = out_sync[k].at;
					ref_t[k] = out_sync[k].t_us;
				}
				air_softmodem_rx_open(&m, &p, 8000, known ? 2000 : 0, on_chunk, NULL);
				out_reset();
				feed(&m, 8000, 0, tap_n, 256);
				CHECK(out_n == ref_n && out_nsync == ref_nsync, "profile %u sigma %.0f: %u symbols, %u syncs with the gate, %u, %u without",
				      prof, sigma[i], out_n, out_nsync, ref_n, ref_nsync);
				for (uint32_t k = 0; k < out_n && k < ref_n; k++)
					diff += out_hard[k] != ref_hard[k] || out_soft[k] != ref_soft[k];
				for (uint8_t k = 0; k < out_nsync && k < ref_nsync; k++)
					diff += out_sync[k].at != ref_at[k] || out_sync[k].t_us != ref_t[k];
				CHECK(diff == 0, "profile %u sigma %.0f known %u: %u differences", prof, sigma[i], known, diff);
				CHECK(i > 1 || out_nsync >= 5, "profile %u sigma %.0f: %u syncs", prof, sigma[i], out_nsync);	// (unknown scale: a third of the level)
				runs++;
			}
		}
	}
	printf("  gate: %u runs the same with it and without\n", runs);
}

// The other pulse shapes and rates
static void testShapes(void)
{
	air_profile_t p = dmr_like(), q;
	air_softmodem_t m;
	uint32_t nerr, ncmp;
	double rms;

	// A root raised cosine pulse through the short filter: not matched, so some eye is lost; clean symbols still
	air_stream(&p, 1, 3000, 300, 288);
	modulate(&p, 0);
	sample_tap(8000, 5, 3600, 2000, -300, 0);
	CHECK(air_softmodem_rx_open(&m, &p, 8000, 2000, on_chunk, NULL), "open");
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	CHECK(compare(&p, 300, 288, &nerr, &ncmp, &rms) && out_sync[0].id == 9, "RRC: sync id %u", out_sync[0].id);
	printf("  RRC 4800: %u syncs, %u errors in %u, soft RMS %.1f\n", out_nsync, nerr, ncmp, rms);
	CHECK(nerr == 0 && out_nsync == 10 && rms < 12, "RRC: %u syncs %u errors RMS %.1f", out_nsync, nerr, rms);
	// 2400 symbols/s, four levels, a 20-symbol sync
	q = p;
	q.sym_rate = 2400;
	q.dev_outer_hz = 1050;
	q.nsyncs = 1;
	q.sync[0].nsyms = 20;
	air_stream(&q, 0, 1500, 200, 192);
	modulate(&q, 0);
	sample_tap(8000, 5, 3600, 2000, 100, 0);
	CHECK(air_softmodem_rx_open(&m, &q, 8000, 2000, on_chunk, NULL), "open 2400");
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	CHECK(out_nsync == 7, "2400: %u syncs", out_nsync);
	// Two levels, square pulses (as a 1200 baud FSK data mode at twice the rate)
	q = p;
	q.levels = 2;
	q.sym_rate = 2400;
	q.shape = AIR_SHAPE_NRZ;
	q.dev_outer_hz = 3000;
	q.nsyncs = 1;
	memcpy(q.sync[0].syms, (uint8_t[]){ 0x77, 0xDF, 0x5D, 0x7F, 0xD5, 0xDD }, 6);
	air_stream(&q, 0, 1500, 200, 192);
	modulate(&q, 0);
	sample_tap(8000, 5, 3600, 1000, 100, 100);
	CHECK(air_softmodem_rx_open(&m, &q, 8000, 1000, on_chunk, NULL), "open 2-level");
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	CHECK(out_nsync == 7, "2-level: %u syncs", out_nsync);
}

// ── A P25 call through the tap, the modem, the framer and the receive hooks ──
static p25_rx_unit_t got[40];
static uint8_t got_air[40][P25_UNIT_BYTES_MAX];
static uint8_t ngot, nlost;

static void on_unit(const p25_rx_unit_t *u, void *ctx)
{
	(void)ctx;
	if (ngot < 40) {
		got[ngot] = *u;
		memcpy(got_air[ngot], u->air, (size_t)(u->nsyms + 3) / 4);
		ngot++;
	}
}

static void on_lost(void *ctx) { (void)ctx; nlost++; }

static void air_add(const uint8_t *unit, uint16_t nsyms)
{
	for (uint16_t i = 0; i < nsyms; i++)
		air[air_n++] = air_sym_get(unit, i);
}

static void testCall(void)
{
	static uint8_t fbuf[P25_UNIT_BYTES_MAX];
	static int8_t fsoft[P25_UNIT_SYMS_MAX];
	// quiet: samples of unmodulated carrier between the noise and the call, as many transmitters key up
	static const struct { double cpk, off, sigma; uint16_t told, quiet; uint8_t units_min; const char *what; } t[] = {
		{ 2000, 0, 0, 2000, 0, 6, "clean" },
		{ 1700, 800, 250, 2000, 0, 6, "low, offset, noise" },
		{ 2000, -500, 350, 0, 0, 5, "unknown scale, noise" },	// the header can go while the level settles
		// The discriminator goes quiet before the call: the level the modem follows must not lag it
		{ 2000, -500, 100, 0, 4000, 6, "unknown scale, quiet carrier first" },
	};
	uint8_t unit[P25_UNIT_BYTES_MAX], hdr[P25_HDR_BYTES], lcraw[P25_LC_BYTES], es[P25_ES_BYTES], out[16];
	uint8_t voice[4][P25_VOICE_FRAMES][P25_IMBE_BYTES];
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 1001, .src = 1234567 }, lcgot;
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;
	air_framer_t fr;
	p25_rx_t rx;
	uint32_t at[6], call_from, call_to;

	fill(hdr, sizeof(hdr));
	fill(es, sizeof(es));
	p25_lc_build(&lc, lcraw);
	// Idle symbols, HDU, LDU1, LDU2, LDU1, LDU2, TDULC, idle symbols; outside them the tap is noise (no carrier)
	air_n = 0;
	for (uint16_t i = 0; i < 40; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	call_from = 0;
	at[0] = air_n;
	p25_unit_begin(unit, P25_HDU_SYMS, 0x293, P25_DUID_HDU, P25_SS_UNKNOWN);
	p25_hdu_put(unit, hdr);
	air_add(unit, P25_HDU_SYMS);
	for (uint8_t k = 0; k < 4; k++) {
		at[1 + k] = air_n;
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, k & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1, P25_SS_UNKNOWN);
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			fill(voice[k][n], P25_IMBE_BYTES);
			p25_ldu_voice_put(unit, n, voice[k][n]);
		}
		if (k & 1)
			p25_ldu_es_put(unit, es);
		else
			p25_ldu_lc_put(unit, lcraw);
		air_add(unit, P25_LDU_SYMS);
	}
	at[5] = air_n;
	p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_UNKNOWN);
	p25_tdulc_put(unit, lcraw);
	air_add(unit, P25_TDULC_SYMS);
	for (uint16_t i = 0; i < 20; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	call_to = air_n;
	modulate(&p, 0);

	for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		static int16_t all[WAVE_MAX / 30 + 16000];
		uint32_t n = 0, lead = 6000, phase = 3 + 8 * i, first, last;
		uint8_t bad_voice = 0, k0;

		sample_tap(8000, phase, 3600, t[i].cpk, t[i].off, t[i].sigma);
		// No carrier before and after: discriminator noise, well above the signal's deviation
		first = (uint32_t)((sym_at(&p, call_from) - phase - LPF_HALF) / 30);
		last = (uint32_t)((sym_at(&p, call_to - 1) - phase - LPF_HALF) / 30);
		for (uint32_t j = 0; j < lead; j++)
			all[n++] = (int16_t)lround(t[i].off + 2.5 * t[i].cpk * gauss());
		for (uint32_t j = 0; j < t[i].quiet; j++)
			all[n++] = (int16_t)lround(t[i].off + t[i].sigma * gauss());
		for (uint32_t j = first; j <= last; j++)
			all[n++] = tap[j];
		for (uint32_t j = 0; j < 4000; j++)
			all[n++] = (int16_t)lround(t[i].off + 2.5 * t[i].cpk * gauss());

		ngot = nlost = 0;
		p25_rx_init(&rx, 0x293, on_unit, on_lost, NULL);
		air_framer_init(&fr, &p25_air_mode, &rx, fbuf, fsoft, P25_UNIT_SYMS_MAX);
		CHECK(air_softmodem_rx_open(&m, &p, 8000, t[i].told, on_chunk, NULL), "open");
		out_reset();
		out_framer = &fr;
		for (uint32_t from = 0; from < n; from += 256)
			air_softmodem_rx(&m, all + from, (uint16_t)(n - from < 256 ? n - from : 256), T0 + from * 125u);
		out_framer = NULL;
		CHECK(ngot >= t[i].units_min && ngot <= 6, "%s: %u units", t[i].what, ngot);
		if (ngot < t[i].units_min)
			continue;
		k0 = (uint8_t)(6 - ngot);		// units missed at the start
		for (uint8_t g = 0; g < ngot; g++) {
			const p25_rx_unit_t *u = &got[g];
			uint8_t k = (uint8_t)(g + k0);
			// The unit's first symbol on the tap's clock: the lead of noise and the quiet carrier, then the call
			// from sample `first`
			double want = (lead + t[i].quiet - (double)first) * 125 + model_us(phase, sym_at(&p, at[k]));
			double d = (double)(uint32_t)(u->t_us - T0) - want;

			CHECK(fabs(d) < 30, "%s unit %u: time off %.0f us", t[i].what, k, d);
			if (k == 0) {
				CHECK(u->duid == P25_DUID_HDU && p25_hdu_get(got_air[g], out) >= 0 && !memcmp(out, hdr, P25_HDR_BYTES), "%s: HDU", t[i].what);
			} else if (k == 5) {
				CHECK(u->duid == P25_DUID_TDULC && p25_tdulc_get(got_air[g], out) >= 0 && !memcmp(out, lcraw, P25_LC_BYTES), "%s: TDULC", t[i].what);
			} else {
				CHECK(u->duid == ((k - 1) & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1) && u->nsyms == P25_LDU_SYMS, "%s LDU %u: duid %x", t[i].what, k, u->duid);
				if ((k - 1) & 1)
					CHECK(p25_ldu_es_get(got_air[g], out) >= 0 && !memcmp(out, es, P25_ES_BYTES), "%s: ES %u", t[i].what, k);
				else
					CHECK(p25_ldu_lc_get(got_air[g], out) >= 0 && p25_lc_parse(out, &lcgot) && lcgot.tgid == 1001 && lcgot.src == 1234567, "%s: LC %u", t[i].what, k);
				for (uint8_t v = 0; v < P25_VOICE_FRAMES; v++)
					if (p25_ldu_voice_get(got_air[g], v, out) < 0 || memcmp(out, voice[k - 1][v], P25_IMBE_BYTES))
						bad_voice++;
			}
		}
		CHECK(bad_voice == 0, "%s: %u voice frames wrong", t[i].what, bad_voice);
		CHECK(nlost == 1, "%s: the end of the call reported %u times", t[i].what, nlost);
		printf("  call, %s: %u units, framer syncs %u misses %u rejects %u\n", t[i].what, ngot, fr.stats.syncs, fr.stats.misses, fr.stats.rejects);
	}
}

// Samples lost between two calls: the modem starts over and says so
static void testHole(void)
{
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;

	air_stream(&p, 0, 3000, 100, 864);
	modulate(&p, 0);
	sample_tap(8000, 0, 3600, 2000, 0, 0);
	air_softmodem_rx_open(&m, &p, 8000, 2000, on_chunk, NULL);
	out_reset();
	feed(&m, 8000, 0, 2048, 256);
	CHECK(out_gap_chunks == 1 && out_nsync == 2, "before: gaps %u syncs %u", out_gap_chunks, out_nsync);
	// 40 ms of samples never arrive (the time on the next call says so)
	feed(&m, 8000, 2048 + 320, tap_n, 256);
	CHECK(out_gap_chunks == 2 && out_nsync == 4, "after: gaps %u syncs %u", out_gap_chunks, out_nsync);
	// A reset by the caller: the same
	air_softmodem_rx_reset(&m);
	feed(&m, 8000, 0, 2048, 256);
	CHECK(out_gap_chunks == 3 && out_nsync == 6, "reset: gaps %u syncs %u", out_gap_chunks, out_nsync);
}

#if CONFIG_AIR_RX_ADAPT
// ── The adaptive options (air-interface.md 6.1): a profile's own, in memory its engine lends ──
static air_softmodem_adapt_t adapt_mem;

// A YSF-like profile: 4800 symbols/s, 2700 Hz, a 20-symbol sync opening every 480-symbol frame
static air_profile_t c4fm_like(uint8_t opts)
{
	air_profile_t p;

	memset(&p, 0, sizeof(p));
	p.sym_rate = 4800;
	p.levels = 4;
	p.shape = AIR_SHAPE_RRC;
	p.shape_param = 20;
	p.dev_outer_hz = 2700;
	p.sync_any_syms = 480;
	p.rx_opts = opts;
	p.nsyncs = 1;
	p.sync[0].id = 3;
	p.sync[0].nsyms = 20;
	memcpy(p.sync[0].syms, (uint8_t[]){ 0xD4, 0x71, 0xC9, 0x63, 0x4D }, 5);
	return p;
}

// One run through the modem: symbol errors per ncmp from the first sync
static double run_adapt(const air_profile_t *p, bool lend, uint32_t *nsync)
{
	air_softmodem_t m;
	uint32_t nerr, ncmp;
	double rms;

	air_softmodem_lend(lend ? &adapt_mem : NULL);
	air_softmodem_rx_open(&m, p, 8000, 2000, on_chunk, NULL);
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	air_softmodem_lend(NULL);
	*nsync = out_nsync;
	if (!compare(p, 200, 480, &nerr, &ncmp, &rms) || !ncmp)
		return 1;
	return (double)nerr / ncmp;
}

// A DCS code (octal) at amplitude a (counts) added to the tap: the Golay(23,12) word (the 9-bit code, then 100,
// then 11 parity bits), data first and LSB first, sent over and over at 134.4 bit/s through a 300 Hz low-pass, as a
// transmitter does; inverted when inv
static void add_dcs(unsigned code, bool inv, double a)
{
	uint32_t data = code | 4u << 9, word = data << 11, r = word;
	int bits[23];
	double y = 0, k = 1 - exp(-2 * M_PI * 300.0 / 8000);

	for (int i = 22; i >= 11; i--)
		if (r & (1u << i))
			r ^= 0xC75u << (i - 11);
	word |= r & 0x7FFu;
	for (int i = 0; i < 12; i++)
		bits[i] = (word >> (11 + i)) & 1;
	for (int i = 0; i < 11; i++)
		bits[12 + i] = (word >> i) & 1;
	for (uint32_t i = 0; i < tap_n; i++) {
		int b = bits[(uint32_t)(i * 134.4 / 8000) % 23] ^ (inv ? 1 : 0);
		double v;

		y += k * ((b ? a : -a) - y);
		v = tap[i] + y;
		tap[i] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

// The same with the receiver reset every `every` samples, as lost samples or a new lock do: only the tone found counts
static void run_adapt_resets(const air_profile_t *p, uint32_t every)
{
	air_softmodem_t m;

	air_softmodem_lend(&adapt_mem);
	air_softmodem_rx_open(&m, p, 8000, 2000, on_chunk, NULL);
	out_reset();
	for (uint32_t at = 0; at < tap_n; at += every) {
		feed(&m, 8000, at, at + every < tap_n ? at + every : tap_n, 256);
		air_softmodem_rx_reset(&m);
	}
	air_softmodem_lend(NULL);
}

// A tone of amplitude a (counts) at f Hz added to the tap
static void add_tone(double f, double a)
{
	for (uint32_t i = 0; i < tap_n; i++) {
		double v = tap[i] + a * sin(2 * M_PI * f * i / 8000);

		tap[i] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

static void testAdapt(void)
{
	air_profile_t plain = c4fm_like(0), eq = c4fm_like(AIR_RX_EQ), tone = c4fm_like(AIR_RX_TONE);
	air_profile_t both = c4fm_like(AIR_RX_EQ | AIR_RX_TONE);
	air_softmodem_adapt_view_t v;
	uint32_t ns0, ns1;
	double e0, e1;

	// A profile that asks for nothing gets nothing, lent memory or not: the plain receiver, symbol for symbol
	air_stream(&plain, 0, 11000, 200, 480);
	modulate(&plain, 0);
	sample_tap(8000, 17, 3600, 2000, 300, 300);
	e0 = run_adapt(&plain, false, &ns0);
	{
		static uint8_t first_hard[OUT_MAX];
		uint32_t n0 = out_n;

		memcpy(first_hard, out_hard, out_n);
		e1 = run_adapt(&plain, true, &ns1);
		CHECK(n0 == out_n && !memcmp(first_hard, out_hard, out_n) && e0 == e1, "no options asked: the same symbols");
		air_softmodem_adapt_get(&v);
		CHECK(v.opts == 0, "no options asked: none running (%u)", v.opts);
	}

	// A clean signal through the equalizer stays clean
	sample_tap(8000, 17, 3600, 2000, 300, 0);
	e1 = run_adapt(&eq, true, &ns1);
	CHECK(e1 < 0.0005 && ns1 >= 20, "equalizer, clean: %.4f errors, %u syncs", e1, ns1);
	air_softmodem_adapt_get(&v);
	CHECK(v.opts == AIR_RX_EQ && v.eq_center > 16384 * 7 / 10 && v.eq_center < 16384 * 12 / 10 && v.eq_resets == 0,
	      "equalizer, clean: center %d, resets %u", v.eq_center, v.eq_resets);

	// The upper band cut, as a repeater's chain does, and noise: the equalizer takes out errors the filter leaves
	for (unsigned hi = 2200; hi <= 2400; hi += 200) {
		sample_tap(8000, 17, hi, 2000, 300, 350);
		e0 = run_adapt(&plain, true, &ns0);
		e1 = run_adapt(&eq, true, &ns1);
		printf("  band cut to %u Hz: symbol errors %.2f %% plain, %.2f %% equalized\n", hi, 100 * e0, 100 * e1);
		CHECK(e0 > 0.01 && e1 < e0 / 2 && ns1 >= ns0, "equalizer, band cut to %u Hz: %.4f against %.4f", hi, e1, e0);
	}

	// A 127.3 Hz tone of 0.4 of a level under the signal: found, notched, fewer errors
	sample_tap(8000, 17, 3600, 2000, 300, 450);
	add_tone(127.3, 0.4 * 1800);
	e0 = run_adapt(&plain, true, &ns0);
	e1 = run_adapt(&tone, true, &ns1);
	air_softmodem_adapt_get(&v);
	printf("  127.3 Hz tone: symbol errors %.2f %% plain, %.2f %% notched (found %u.%u Hz)\n", 100 * e0, 100 * e1,
	       v.tone_dhz / 10, v.tone_dhz % 10);
	CHECK(v.tone_dhz == 1273 && v.tone_picks >= 1, "tone found: %u", v.tone_dhz);
	CHECK(e0 > 0.005 && e1 < e0 * 0.6, "tone notched: %.4f against %.4f", e1, e0);
	// A low tone notched as deep as a high one: coefficients x 2^14 put the zero of a 69.3 Hz notch 0.7 Hz off the
	// tone. And found as fast behind the equalizer, which follows a low tone in part and hid it from a search run
	// after it
	sample_tap(8000, 17, 3600, 2000, 300, 450);
	add_tone(69.3, 0.4 * 1800);
	e0 = run_adapt(&plain, true, &ns0);
	e1 = run_adapt(&tone, true, &ns1);
	{
		// The notch's own response at the tone, from the coefficients it runs (x 2^14 and x 2^20, at 8 kHz)
		const air_softmodem_adapt_t *a = &adapt_mem;
		double w = 2 * M_PI * 69.3 / 8000, g = a->ng / 16384.0, dc = a->ndc / 1048576.0;
		double a1 = a->nda / 1048576.0 - 2, a2 = 1 - a->ne / 1048576.0;
		double complex z1 = cexp(-I * w), z2 = z1 * z1;
		double h = cabs(g * (1 - (2 - dc) * z1 + z2) / (1 + a1 * z1 + a2 * z2));

		printf("  69.3 Hz tone: symbol errors %.2f %% plain, %.2f %% notched, the notch %.1f dB at the tone\n",
		       100 * e0, 100 * e1, 20 * log10(h + 1e-12));
		CHECK(e0 > 0.005 && e1 < e0 * 0.7 && h < 0.01, "69.3 Hz tone notched: %.4f against %.4f, %.4f at the tone",
		      e1, e0, h);
	}
	e1 = run_adapt(&both, true, &ns1);
	air_softmodem_adapt_get(&v);
	printf("  69.3 Hz tone, equalizer on: symbol errors %.2f %% (found %u.%u Hz)\n", 100 * e1, v.tone_dhz / 10,
	       v.tone_dhz % 10);
	CHECK(v.tone_dhz == 693 && e1 < e0 * 0.5, "69.3 Hz tone behind the equalizer: %u, %.4f against %.4f",
	      v.tone_dhz, e1, e0);
	// Tones on or next to a multiple of the 10 Hz frame rate (100.0 Hz is one: what repeats every frame is left out,
	// not differenced away), close neighbors and the band's edges, each found as itself
	{
		static const struct { double hz; uint16_t dhz; } t[] = { { 67.0, 670 }, { 69.3, 693 }, { 79.7, 797 },
			{ 100.0, 1000 }, { 146.2, 1462 }, { 159.8, 1598 }, { 192.8, 1928 }, { 199.5, 1995 }, { 250.3, 2503 },
			{ 254.1, 2541 } };

		for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
			sample_tap(8000, 17, 3600, 2000, 300, 450);
			add_tone(t[i].hz, 0.4 * 1800);
			run_adapt(&tone, true, &ns1);
			air_softmodem_adapt_get(&v);
			CHECK(v.tone_dhz == t[i].dhz, "%.1f Hz tone: found %u", t[i].hz, v.tone_dhz);
		}
	}
	// Every standard tone, at 0.2 and at 0.4 of a level, found as itself (air-interface.md 6.1)
	{
		const uint16_t *all = tones_ctcss;
		unsigned found = 0;

		for (unsigned lv = 1; lv <= 2; lv++) {
			for (unsigned i = 0; i < TONES_CTCSS_COUNT; i++) {
				sample_tap(8000, 17, 3600, 2000, 300, 450);
				add_tone(all[i] / 10.0, 0.2 * lv * 1800);
				run_adapt(&tone, true, &ns1);
				air_softmodem_adapt_get(&v);
				found += v.tone_dhz == all[i];
				CHECK(v.tone_dhz == all[i], "%u.%u Hz tone at %.1f of a level: found %u", all[i] / 10,
				      all[i] % 10, 0.2 * lv, v.tone_dhz);
			}
		}
		printf("  standard tones: %u of 100 found as themselves (50 at 0.2, 50 at 0.4 of a level)\n", found);
	}

	// Resets every half second (lost samples, a new lock) don't start the search over: the tone a repeater sends
	// outlasts them, and a search that began again at each never reached the second block it needs
	sample_tap(8000, 17, 3600, 2000, 300, 450);
	add_tone(127.3, 0.4 * 1800);
	run_adapt_resets(&tone, 4000);
	air_softmodem_adapt_get(&v);
	CHECK(v.tone_dhz == 1273, "tone found across resets: %u", v.tone_dhz);
	// DCS 023 at half a level under the signal (about 450 Hz, as repeaters send it): found (its word averaged by its place, any start of it a
	// codeword), named, taken off the samples: fewer errors than without. Inverted it reads as 047, the code whose
	// word is 023's inverted, as radios list them. Without DCS nothing locks
	{
		air_profile_t dcs = c4fm_like(AIR_RX_DCS);

		sample_tap(8000, 17, 3600, 2000, 300, 450);
		add_dcs(023, false, 0.5 * 1800);
		e0 = run_adapt(&plain, true, &ns0);
		e1 = run_adapt(&dcs, true, &ns1);
		air_softmodem_adapt_get(&v);
		printf("  DCS 023: symbol errors %.2f %% plain, %.2f %% with DCS taken off (code %03o)\n", 100 * e0, 100 * e1,
		       v.dcs_code & 0x1FFu);
		CHECK(v.dcs_code == 023 && v.dcs_locks >= 1, "DCS found: %03o, %u locks", v.dcs_code, v.dcs_locks);
		CHECK(e1 < e0 * 0.45, "DCS taken off: %.4f against %.4f", e1, e0);
		sample_tap(8000, 17, 3600, 2000, 300, 450);
		add_dcs(023, true, 0.5 * 1800);
		run_adapt(&dcs, true, &ns1);
		air_softmodem_adapt_get(&v);
		CHECK(v.dcs_code == 047, "DCS 023 inverted reads as 047: %03o", v.dcs_code);
		sample_tap(8000, 17, 3600, 2000, 300, 450);
		add_tone(127.3, 0.4 * 1800);
		run_adapt(&dcs, true, &ns1);
		air_softmodem_adapt_get(&v);
		CHECK(v.dcs_code == 0 && v.dcs_locks == 0, "no DCS, a tone: none locked (%03o, %u)", v.dcs_code, v.dcs_locks);
	}

	// No tone: none picked, however long (and the same data again and again, as silence repeats, picks none)
	sample_tap(8000, 17, 3600, 2000, 300, 300);
	e1 = run_adapt(&both, true, &ns1);
	air_softmodem_adapt_get(&v);
	CHECK(v.tone_dhz == 0 && v.tone_picks == 0 && v.tone_blocks >= 3, "no tone: %u picked in %u blocks", v.tone_dhz,
	      v.tone_blocks);
	for (uint32_t k = 680; k < air_n; k++)
		air[k] = air[200 + (k - 200) % 480];	// every frame the first one
	modulate(&both, 0);
	sample_tap(8000, 17, 3600, 2000, 300, 200);
	run_adapt(&both, true, &ns1);
	air_softmodem_adapt_get(&v);
	CHECK(v.tone_dhz == 0, "repeating frames: no tone (%u)", v.tone_dhz);

	// The bench turns an option off at once
	sample_tap(8000, 17, 3600, 2000, 300, 250);
	add_tone(127.3, 0.4 * 1800);
	air_softmodem_adapt_off = AIR_RX_TONE;
	run_adapt(&both, true, &ns1);
	air_softmodem_adapt_get(&v);
	air_softmodem_adapt_off = 0;
	CHECK(v.opts == AIR_RX_EQ && v.tone_dhz == 0, "bench off: opts %u tone %u", v.opts, v.tone_dhz);
}
#endif

int main(void)
{
	testFit();
	testClean();
	testLevels();
	testNoise();
	testGate();
	testShapes();
	testCall();
	testHole();
#if CONFIG_AIR_RX_ADAPT
	testAdapt();
#endif
	printf("airSoftmodemTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}

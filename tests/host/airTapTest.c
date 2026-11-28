// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The provider on a board's sample taps (core/air/air_tap.c). Receive, over a slope tap: a tap whose samples are the
// derivative of the deviation (hal_baseband_t.rx_slope_hz), as an FM chip's demodulator output for a DMR baseband
// gives it. P25 (C4FM, 4800 symbols/s) and NXDN 4800 bit/s (2400 symbols/s) are modulated, band-limited,
// differentiated, sampled at 16 kHz with an offset and noise of about an eighth of the signal, and read through a
// made-up board tap a poll at a time. It checks the fit (receive only without a transmit tap), open, close and
// restart, the lock hooks, the counters, the symbol error rate, a whole P25 call through the framer, a lost run of
// samples, and what a tap of the wrong polarity gives. Transmit, through a made-up board's transmit tap on a clock of
// the test's: the events and their times, the receive tap stopped and started again, an empty queue, a late poll, an
// abort, and the samples received back as the symbols queued.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_tap.h"
#include "air/air_syms.h"
#include "p25/p25.h"
#include "nxdn/nxdn.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x5EED1234;
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
#define RATE		16000
#define DEC		(FS / RATE)
#define SPAN		8
#define SYMS_MAX	24000
#define WAVE_MAX	(SYMS_MAX * 100 + 4000)
#define LEAD		2000
#define LPF_HALF	120

static double wave[WAVE_MAX];		// deviation, Hz
static double band[WAVE_MAX];		// ... band-limited
static uint32_t wave_n;
static uint8_t air[SYMS_MAX];
static uint32_t air_n;
static int16_t tap[WAVE_MAX / DEC + 32000];
static uint32_t tap_n;

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
		} else {
			// Root raised cosine (NXDN's, without its sinc shaper: the receive filter is not matched to it)
			double x = t * rate;

			if (fabs(x) < 1e-9)
				v = 1 - alpha + 4 * alpha / M_PI;
			else if (fabs(fabs(4 * alpha * x) - 1) < 1e-6)
				v = alpha / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
			else
				v = (sin(M_PI * x * (1 - alpha)) + 4 * alpha * x * cos(M_PI * x * (1 + alpha))) /
				    (M_PI * x * (1 - (4 * alpha * x) * (4 * alpha * x)));
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
}

static void modulate(const air_profile_t *p)
{
	static double h[2 * SPAN * 100 + 1];
	int osr = FS / p->sym_rate;

	make_pulse(h, osr, p->shape, p->shape_param / 100.0);
	wave_n = LEAD + air_n * (uint32_t)osr + LEAD;
	memset(wave, 0, wave_n * sizeof(double));
	for (uint32_t k = 0; k < air_n; k++) {
		long at = LEAD + (long)k * osr - SPAN * osr;
		double lv = air_sym_level(air[k]) * (p->dev_outer_hz / 3.0);

		for (int i = 0; i <= 2 * SPAN * osr; i++)
			if (at + i >= 0 && at + i < (long)wave_n)
				wave[at + i] += lv * h[i];
	}
}

static double sym_at(const air_profile_t *p, uint32_t k)
{
	return LEAD + (double)k * (FS / p->sym_rate);
}

// The tap: wave[] plus white noise of dev_noise x its RMS (the discriminator's, in the band), through a low pass at
// hi_hz, plus a wander of wander x its RMS (noise under WANDER_HZ: most of what a radio's slope tap leaves after the
// integrator, measured), differentiated, sampled at RATE from model sample phase on, plus white noise of tap_noise x
// the derivative's RMS (the tap's own: the ADC, hum) and an offset of off counts. scale: counts per kHz/ms of slope;
// sign -1 a tap of the other polarity
static double nz[WAVE_MAX], nzf[WAVE_MAX];
static double sig_rms;

static double rms(const double *x, uint32_t from, uint32_t to)
{
	double ss = 0;

	for (uint32_t j = from; j < to; j++)
		ss += x[j] * x[j];
	return sqrt(ss / (to > from ? to - from : 1));
}

static void lowpass(double *out, const double *in, const double *h, double hsum)
{
	memset(out, 0, sizeof(double) * wave_n);
	for (uint32_t j = LPF_HALF; j + LPF_HALF < wave_n; j++) {
		double v = 0;

		for (int i = -LPF_HALF; i <= LPF_HALF; i++)
			v += in[j + (uint32_t)i] * h[i + LPF_HALF];
		out[j] = v / hsum;
	}
}

#define WANDER_HZ	150.0

static void sample_slope(uint32_t phase, double hi_hz, double scale, double off, double dev_noise, double wander,
			 double tap_noise, int sign)
{
	static double lpf[2 * LPF_HALF + 1];
	double sum = 0, ss = 0, sigma;
	uint32_t n = 0;

	for (int i = -LPF_HALF; i <= LPF_HALF; i++) {
		double x = 2 * M_PI * hi_hz / FS * i, w = 0.54 + 0.46 * cos(M_PI * i / LPF_HALF);

		lpf[i + LPF_HALF] = (i ? sin(x) / x : 1) * w;
		sum += lpf[i + LPF_HALF];
	}
	lowpass(band, wave, lpf, sum);
	if (dev_noise > 0) {
		double want = dev_noise * rms(band, LEAD, wave_n - LEAD), got;

		for (uint32_t j = 0; j < wave_n; j++)
			nz[j] = gauss();
		lowpass(nzf, nz, lpf, sum);
		got = rms(nzf, LEAD, wave_n - LEAD);
		for (uint32_t j = 0; j < wave_n; j++)
			band[j] += nzf[j] * want / got;
	}
	if (wander > 0) {
		// White noise through two one-pole low passes at WANDER_HZ
		double want = wander * rms(band, LEAD, wave_n - LEAD), got, a = exp(-2 * M_PI * WANDER_HZ / FS), y1 = 0, y2 = 0;

		for (uint32_t j = 0; j < wave_n; j++) {
			y1 = a * y1 + (1 - a) * gauss();
			y2 = a * y2 + (1 - a) * y1;
			nz[j] = y2;
		}
		got = rms(nz, LEAD, wave_n - LEAD);
		for (uint32_t j = 0; j < wave_n; j++)
			band[j] += nz[j] * want / got;
	}
	// The derivative's RMS, then the samples with the tap's noise at the fraction asked
	for (uint32_t j = phase + LPF_HALF + 1; j + LPF_HALF + 1 < wave_n; j += DEC, n++) {
		double d = (band[j + 1] - band[j - 1]) * FS / 2 / 1e6 * scale;	// kHz per ms

		ss += d * d;
	}
	sig_rms = sqrt(ss / (n ? n : 1));
	sigma = tap_noise * sig_rms;
	for (uint32_t j = phase + LPF_HALF + 1; j + LPF_HALF + 1 < wave_n; j += DEC) {
		double d = sign * (band[j + 1] - band[j - 1]) * FS / 2 / 1e6 * scale + off + sigma * gauss();

		tap[tap_n++] = (int16_t)lround(d > 32767 ? 32767 : d < -32768 ? -32768 : d);
	}
}

// Silence on the tap (no carrier): noise at about the signal's level, and the offset
static void sample_noise(uint32_t n, double off, double sigma)
{
	while (n--)
		tap[tap_n++] = (int16_t)lround(off + sigma * gauss());
}

// ── The board's tap ──
#define T0		0xFFF00000u	// the clock wraps inside the runs
static uint32_t pos, avail;		// next sample read; samples the "interrupt" has taken so far
static bool started;
static uint32_t starts, stops, locks, unlocks;
static int depth;
static bool refuse_start;
static uint32_t clock_us;

static double model_us_of(uint32_t phase, double j)
{
	return (j - phase - LPF_HALF - 1) * 1e6 / FS;
}

static bool bb_start(void)
{
	if (refuse_start)
		return false;
	started = true;
	starts++;
	return true;
}

static uint16_t bb_read(int16_t *buf, uint16_t max)
{
	uint16_t n = 0;

	CHECK(started, "read only while started");
	while (pos < avail && n < max)
		buf[n++] = tap[pos++];
	return n;
}

static void bb_stop(void)
{
	started = false;
	stops++;
}

static uint32_t bb_now(void)
{
	return clock_us += 3;
}

static uint32_t bb_time(void)
{
	return T0 + (uint32_t)(((uint64_t)pos * 1000000u) / RATE);
}

static hal_baseband_t bb = {
	.rx_rate_hz = RATE, .rx_channels = 1, .rx_lo_hz = 25, .rx_hi_hz = 3000, .rx_counts_per_khz = 0, .rx_slope_hz = 25,
	.rx_start = bb_start, .rx_read = bb_read, .rx_stop = bb_stop, .now_us = bb_now, .rx_time_us = bb_time,
};

static void lk(void)
{
	locks++;
	depth++;
	CHECK(depth == 1, "lock nests");
}

static void unlk(void)
{
	unlocks++;
	depth--;
}

static air_softmodem_t sm;
static const air_tap_cfg_t cfg = { .bb = &bb, .sm = &sm, .fit_flags = AIR_FIT_UNTESTED, .lock = lk, .unlock = unlk };

// ── What comes out ──
#define OUT_MAX		(SYMS_MAX + 4000)
static uint8_t out_hard[OUT_MAX];
static int8_t out_soft[OUT_MAX];
static uint32_t out_n, out_chunks, out_gaps;
static struct { uint32_t at; double t_us; } out_sync[256];
static uint16_t out_nsync;
static air_framer_t *out_framer;
static uint16_t out_rate;

static void on_event(const air_event_t *ev, void *ctx)
{
	const air_chunk_t *c = ev->chunk;

	CHECK(ctx == &out_n, "context");
	CHECK(ev->kind == AIR_EV_RX && c != NULL && c->soft != NULL && depth == 1, "an RX event, inside the poll's lock");
	out_chunks++;
	if (c->flags & AIR_CHUNK_GAP)
		out_gaps++;
	if (c->sync_at >= 0 && out_nsync < 256) {
		out_sync[out_nsync].at = out_n + (uint32_t)c->sync_at;
		out_sync[out_nsync].t_us = (double)(uint32_t)(c->t_us - T0) + c->sync_at * 1e6 / out_rate;
		out_nsync++;
	}
	for (uint16_t i = 0; i < c->nsyms && out_n < OUT_MAX; i++) {
		out_hard[out_n] = air_sym_get(c->hard, i);
		out_soft[out_n++] = c->soft[i];
	}
	if (out_framer)
		air_framer_rx(out_framer, c);
}

static void out_reset(uint16_t rate)
{
	out_n = out_chunks = out_gaps = 0;
	out_nsync = 0;
	out_rate = rate;
}

// The board's task: 2 ms of samples arrive, then a poll
static void run(uint32_t to)
{
	while (avail < to) {
		avail += RATE / 500;
		if (avail > to)
			avail = to;
		CHECK(air_tap_poll(), "poll with a session");
	}
}

static void air_stream(const air_profile_t *p, uint8_t s, uint32_t n, uint32_t first, uint32_t every)
{
	air_n = 0;
	for (uint32_t k = 0; k < n; k++)
		air[air_n++] = (uint8_t)(rng() & 3);
	for (uint32_t at = first; at + p->sync[s].nsyms <= n; at += every)
		for (uint8_t i = 0; i < p->sync[s].nsyms; i++)
			air[at + i] = air_sym_get(p->sync[s].syms, i);
}

// Symbols out against the air from the first sync found on (the tap's time says which sent sync it is)
static bool compare(const air_profile_t *p, uint32_t phase, uint32_t first, uint32_t every, uint32_t *nerr,
		    uint32_t *ncmp)
{
	uint32_t o, a;

	*nerr = *ncmp = 0;
	if (!out_nsync)
		return false;
	o = out_sync[0].at;
	{
		double t = out_sync[0].t_us * 1e-6 * FS + phase + LPF_HALF + 1;	// model sample
		double k = (t - LEAD) / (FS / p->sym_rate);

		a = first + (uint32_t)lround((k - first) / every) * every;
	}
	for (; o < out_n && a < air_n; o++, a++) {
		(*ncmp)++;
		if (out_hard[o] != air[a])
			(*nerr)++;
	}
	return true;
}

static void open_tap(const air_mode_t *mode)
{
	pos = avail = 0;
	CHECK(air_tap.open(&mode->profile, on_event, &out_n), "open");
	CHECK(started, "the tap started");
}

// ── Tests ──
static void testFit(void)
{
	static const air_mode_t *modes[] = { &p25_air_mode, &nxdn_air_mode, &nxdn_air_mode_9600 };
	air_profile_t q = p25_air_mode.profile;
	uint32_t f;

	air_tap_setup(&cfg);
	for (uint8_t i = 0; i < 3; i++) {
		f = air_tap.fit(&modes[i]->profile);
		printf("  fit %s: %#x\n", modes[i]->name, (unsigned)f);
		CHECK((f & (AIR_FIT_RX | AIR_FIT_SOFT | AIR_FIT_UNTESTED)) == (AIR_FIT_RX | AIR_FIT_SOFT | AIR_FIT_UNTESTED) &&
		      !(f & AIR_FIT_TX), "%s: receive only, untested: %#x", modes[i]->name, (unsigned)f);
	}
	// The integrator's corner sits above the tap's band edge: the profile's lowest tones are what counts
	bb.rx_lo_hz = 400;
	CHECK(air_tap.fit(&q) == 0, "a slope tap whose band starts at 400 Hz can't carry P25's 4-level runs");
	bb.rx_lo_hz = 25;
	bb.rx_hi_hz = 2000;
	CHECK(air_tap.fit(&q) == 0, "a band to 2 kHz can't carry 4800 symbols/s");
	bb.rx_hi_hz = 3000;
	// The leak: the power of two nearest 25 Hz at 16 kHz
	{
		uint16_t corner = 0;
		uint8_t leak = air_softmodem_leak(RATE, 25, &corner);

		CHECK(leak == 7 && corner >= 18 && corner <= 22, "leak %u corner %u Hz", leak, corner);
		CHECK(air_softmodem_leak(RATE, 0, NULL) == 0 && air_softmodem_leak(8000, 2000, NULL) == 0, "no slope, a slope out of range");
	}
	// The transmit side refuses; nothing to count while closed
	{
		air_tx_chunk_t c;
		air_rx_counts_t rc;

		memset(&c, 0, sizeof c);
		CHECK(!air_tap.tx_begin() && !air_tap.tx_queue(&c) && air_tap.tx_space() == 0, "no transmit");
		CHECK(!air_tap.rx_counts(&rc) && !air_tap_poll(), "closed: no counts, nothing polled");
	}
}

static void testOpenClose(void)
{
	const air_profile_t *p = &p25_air_mode.profile;
	air_rx_counts_t rc;

	air_tap_setup(&cfg);
	locks = unlocks = starts = stops = 0;
	refuse_start = true;
	CHECK(!air_tap.open(p, on_event, &out_n), "the board refused the tap (rule 1): open fails");
	refuse_start = false;
	CHECK(!air_tap.open(p, NULL, NULL), "no callback");
	open_tap(&p25_air_mode);
	CHECK(!air_tap.open(p, on_event, &out_n), "one session at a time");
	CHECK(air_tap.rx_counts(&rc) && rc.halves == 0 && rc.chunks == 0, "counts from 0");
	air_tap.close();
	CHECK(!started && stops == 1 && locks == unlocks && locks >= 2, "close stops the tap under the lock (%u, %u)", locks, unlocks);
	air_tap.close();
	CHECK(stops == 1, "a second close does nothing");
	CHECK(!air_tap_poll(), "no poll after close");
}

// A stream of P25 symbols, its frame sync every 864, through the tap a poll at a time: the error rate at an eighth
// of the signal in noise, and the counters
static void testP25Ser(void)
{
	static const double noise[] = { 0, 0.12, 0.2 };
	static const double max_ser[] = { 0.001, 0.01, 0.08 };
	const air_profile_t *p = &p25_air_mode.profile;
	air_rx_counts_t rc;

	air_stream(p, 0, 20000, 300, 864);
	modulate(p);
	for (uint8_t i = 0; i < 3; i++) {
		uint32_t nerr, ncmp, phase = 3 + 4 * i;
		uint32_t work, span;

		tap_n = 0;
		sample_slope(phase, 3000, 1000, 600, noise[i], 0, 0.02, 1);
		open_tap(&p25_air_mode);
		out_reset(p->sym_rate);
		run(tap_n);
		CHECK(compare(p, phase, 300, 864, &nerr, &ncmp), "noise %.2f: no sync", noise[i]);
		printf("  P25, slope tap at 16 kHz, noise %.0f %% of the signal: %u syncs of 23, symbol errors %u of %u (%.3f %%)\n",
		       noise[i] * 100, out_nsync, nerr, ncmp, ncmp ? 100.0 * nerr / ncmp : 0);
		CHECK(out_nsync >= 20 && ncmp > 18000 && (double)nerr / ncmp <= max_ser[i], "noise %.2f: %u syncs, %u errors in %u",
		      noise[i], out_nsync, nerr, ncmp);
		CHECK(air_tap.rx_counts(&rc) && rc.halves >= tap_n / 32 && rc.overruns == 0 && rc.chunks == out_chunks,
		      "counts: reads %u overruns %u chunks %u/%u", rc.halves, rc.overruns, rc.chunks, out_chunks);
		CHECK(air_tap.rx_load(&work, &span) && span > tap_n * 62u && span <= tap_n * 63u && work > 0, "load %u of %u us",
		      work, span);
		// Sync times: on the tap's clock, at the symbol's center
		for (uint16_t k = 0; k < out_nsync; k++) {
			double t = out_sync[k].t_us, want = model_us_of(phase, sym_at(p, 300 + 864u * (uint32_t)lround(
				(t * 1e-6 * FS + phase + LPF_HALF + 1 - LEAD) / (FS / p->sym_rate) / 864.0 - 300 / 864.0)));
			CHECK(fabs(t - want) < 40, "sync %u: time off %.0f us", k, t - want);
		}
		air_tap.close();
	}
}

// P25 with the noise a radio's slope tap leaves after the integrator: a wander under WANDER_HZ, with a little in the
// band. The soft modem slices from both halves of the integral (air_softmodem.c, TWO_K): the wander comes in from
// independent halves and the corner between them takes most of it
static void testP25Wander(void)
{
	static const double wander[] = { 0.2, 0.3 };
	static const double max_ser[] = { 0.008, 0.06 };
	static const uint32_t min_syncs[] = { 15, 10 };	// (the sync fit sees the one-sided sum: the wander costs it syncs)
	const air_profile_t *p = &p25_air_mode.profile;

	air_stream(p, 0, 20000, 300, 864);
	modulate(p);
	for (uint8_t i = 0; i < 2; i++) {
		uint32_t nerr, ncmp, phase = 5;

		tap_n = 0;
		sample_slope(phase, 3000, 1000, 600, 0.05, wander[i], 0.02, 1);
		open_tap(&p25_air_mode);
		out_reset(p->sym_rate);
		run(tap_n);
		CHECK(compare(p, phase, 300, 864, &nerr, &ncmp), "wander %.2f: no sync", wander[i]);
		printf("  P25, slope tap, wander %.0f %% of the signal under %.0f Hz: %u syncs of 23, symbol errors %u of %u (%.3f %%)\n",
		       wander[i] * 100, WANDER_HZ, out_nsync, nerr, ncmp, ncmp ? 100.0 * nerr / ncmp : 0);
		CHECK(out_nsync >= min_syncs[i] && ncmp > 18000 && (double)nerr / ncmp <= max_ser[i], "wander %.2f: %u syncs, %u errors in %u",
		      wander[i], out_nsync, nerr, ncmp);
		air_tap.close();
	}
}

// NXDN at 4800 bit/s: 2400 symbols/s, its frame sync word every frame
static void testNxdnSer(void)
{
	static const double noise[] = { 0, 0.12 };
	const air_profile_t *p = &nxdn_air_mode.profile;

	air_stream(p, 0, 9000, 100, NXDN_FRAME_SYMS);
	modulate(p);
	for (uint8_t i = 0; i < 2; i++) {
		uint32_t nerr, ncmp, phase = 7;

		tap_n = 0;
		sample_slope(phase, 3000, 1000, -400, noise[i], 0, 0.02, 1);
		open_tap(&nxdn_air_mode);
		out_reset(p->sym_rate);
		run(tap_n);
		CHECK(compare(p, phase, 100, NXDN_FRAME_SYMS, &nerr, &ncmp), "NXDN noise %.2f: no sync", noise[i]);
		printf("  NXDN 4800, slope tap at 16 kHz, noise %.0f %%: %u syncs of 46, symbol errors %u of %u (%.3f %%)\n",
		       noise[i] * 100, out_nsync, nerr, ncmp, ncmp ? 100.0 * nerr / ncmp : 0);
		// The soft modem misses some of NXDN's short syncs (10 symbols, 4 ms): the integrator's leak sags across
		// them by more than the fit allows after a run of data off center. The timing holds from the last one, and
		// the framer searches the symbols for syncs itself
		CHECK(out_nsync >= 36 && ncmp > 8000 && (double)nerr / ncmp <= (i ? 0.02 : 0.002), "NXDN noise %.2f: %u syncs, %u errors in %u",
		      noise[i], out_nsync, nerr, ncmp);
		air_tap.close();
	}
}

// ── A P25 call through the tap, the framer and the receive hooks ──
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
	uint8_t unit[P25_UNIT_BYTES_MAX], hdr[P25_HDR_BYTES], lcraw[P25_LC_BYTES], es[P25_ES_BYTES], out[16];
	uint8_t voice[4][P25_VOICE_FRAMES][P25_IMBE_BYTES];
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 1001, .src = 1234567 }, lcgot;
	const air_profile_t *p = &p25_air_mode.profile;
	air_framer_t fr;
	p25_rx_t rx;
	uint8_t bad_voice = 0, k0;

	fill(hdr, sizeof(hdr));
	fill(es, sizeof(es));
	p25_lc_build(&lc, lcraw);
	air_n = 0;
	for (uint16_t i = 0; i < 40; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	p25_unit_begin(unit, P25_HDU_SYMS, 0x293, P25_DUID_HDU, P25_SS_UNKNOWN);
	p25_hdu_put(unit, hdr);
	air_add(unit, P25_HDU_SYMS);
	for (uint8_t k = 0; k < 4; k++) {
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
	p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_UNKNOWN);
	p25_tdulc_put(unit, lcraw);
	air_add(unit, P25_TDULC_SYMS);
	for (uint16_t i = 0; i < 20; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	modulate(p);

	// No carrier first and after: the tap shows noise at about the signal's level
	tap_n = 0;
	sample_slope(5, 3000, 1000, 300, 0.12, 0, 0.02, 1);
	{
		static int16_t call[WAVE_MAX / DEC];
		uint32_t n = tap_n;

		memcpy(call, tap, n * sizeof(int16_t));
		tap_n = 0;
		sample_noise(8000, 300, sig_rms);
		memcpy(tap + tap_n, call, n * sizeof(int16_t));
		tap_n += n;
		sample_noise(4000, 300, sig_rms);
	}
	ngot = nlost = 0;
	p25_rx_init(&rx, 0x293, on_unit, on_lost, NULL);
	air_framer_init(&fr, &p25_air_mode, &rx, fbuf, fsoft, P25_UNIT_SYMS_MAX);
	open_tap(&p25_air_mode);
	out_reset(p->sym_rate);
	out_framer = &fr;
	run(tap_n);
	out_framer = NULL;
	air_tap.close();
	printf("  P25 call, noise 12 %%: %u units of 6, framer syncs %u misses %u rejects %u\n", ngot, fr.stats.syncs,
	       fr.stats.misses, fr.stats.rejects);
	CHECK(ngot >= 5 && ngot <= 6, "%u units", ngot);
	if (ngot < 5)
		return;
	k0 = (uint8_t)(6 - ngot);
	for (uint8_t g = 0; g < ngot; g++) {
		const p25_rx_unit_t *u = &got[g];
		uint8_t k = (uint8_t)(g + k0);

		CHECK(u->nac == 0x293, "unit %u: NAC %#x", k, u->nac);
		if (k == 0) {
			CHECK(u->duid == P25_DUID_HDU && p25_hdu_get(got_air[g], out) >= 0 && !memcmp(out, hdr, P25_HDR_BYTES), "HDU");
		} else if (k == 5) {
			CHECK(u->duid == P25_DUID_TDULC && p25_tdulc_get(got_air[g], out) >= 0 && !memcmp(out, lcraw, P25_LC_BYTES), "TDULC");
		} else {
			CHECK(u->duid == ((k - 1) & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1) && u->nsyms == P25_LDU_SYMS, "LDU %u: duid %x", k, u->duid);
			if ((k - 1) & 1)
				CHECK(p25_ldu_es_get(got_air[g], out) >= 0 && !memcmp(out, es, P25_ES_BYTES), "ES %u", k);
			else
				CHECK(p25_ldu_lc_get(got_air[g], out) >= 0 && p25_lc_parse(out, &lcgot) && lcgot.tgid == 1001 && lcgot.src == 1234567, "LC %u", k);
			for (uint8_t v = 0; v < P25_VOICE_FRAMES; v++)
				if (p25_ldu_voice_get(got_air[g], v, out) < 0 || memcmp(out, voice[k - 1][v], P25_IMBE_BYTES))
					bad_voice++;
		}
	}
	CHECK(bad_voice == 0, "%u voice frames wrong", bad_voice);
	CHECK(nlost == 1, "the end of the call reported %u times", nlost);
}

// Samples lost (the board's ring overran) and a restart: counted, and the modem starts over at each
static void testHoleRestart(void)
{
	const air_profile_t *p = &p25_air_mode.profile;
	air_rx_counts_t rc;
	uint32_t gaps;

	air_stream(p, 0, 6000, 100, 864);
	modulate(p);
	tap_n = 0;
	sample_slope(0, 3000, 1000, 0, 0.05, 0, 0.02, 1);
	open_tap(&p25_air_mode);
	out_reset(p->sym_rate);
	run(tap_n / 3);
	gaps = out_gaps;
	CHECK(gaps == 1 && out_nsync >= 1, "before: gaps %u syncs %u", gaps, out_nsync);
	// 40 ms the board never delivered
	pos += 640;
	avail = pos;
	run(2 * tap_n / 3);
	CHECK(air_tap.rx_counts(&rc) && rc.overruns == 1 && out_gaps == 2, "a hole: overruns %u gaps %u", rc.overruns, out_gaps);
	// A restart: what the tap holds goes, the modem starts over
	avail += 500;
	air_tap.rx_restart();
	CHECK(air_tap_poll() && pos == avail, "restart: the tap emptied (%u of %u)", pos, avail);
	run(tap_n);
	CHECK(air_tap.rx_counts(&rc) && out_gaps == 3, "restart: gaps %u overruns %u", out_gaps, rc.overruns);
	CHECK(out_nsync >= 4, "syncs after: %u", out_nsync);
	air_tap.close();
}

// A tap of the other polarity: no sync is ever found (on a radio, no sync at all with a carrier on the channel says
// the tap's sign is wrong)
static void testPolarity(void)
{
	const air_profile_t *p = &p25_air_mode.profile;

	air_stream(p, 0, 6000, 100, 864);
	modulate(p);
	tap_n = 0;
	sample_slope(0, 3000, 1000, 0, 0.05, 0, 0.02, -1);
	open_tap(&p25_air_mode);
	out_reset(p->sym_rate);
	run(tap_n);
	printf("  P25, the tap's polarity reversed: %u syncs\n", out_nsync);
	CHECK(out_nsync == 0, "reversed: %u syncs", out_nsync);
	air_tap.close();
}

// ── Transmit: a made-up board with both taps, on the test's clock ──
#define TX_RATE		24000
#define TX_RING		2048		// the board's buffer, in samples
#define TX_CPK		4000
#define TX_MAX		(SYMS_MAX * 5 + 4000)
static int16_t txw[TX_MAX + 2240];		// every sample the board took, in order
static uint32_t txw_n;
static uint32_t tclock = T0;
static uint32_t tx_t0;			// when the board's clock started
static bool tx_on, tx_refuse;
static uint32_t tx_starts, tx_stops;
static struct { uint8_t kind; uint32_t t_us; } tev[16];	// the first events, and per kind a count and the latest
static uint8_t ntev;
static uint16_t nkind[8];
static uint32_t last_t[8];
static uint8_t last_kind, started_lead;
static uint32_t tx_next;		// next symbol of air[] to queue
static int tx_depth;

static uint32_t t_now(void) { return tclock; }

// What the board has clocked out by now
static uint32_t t_out(void)
{
	uint64_t n = tx_on ? (uint64_t)(uint32_t)(tclock - tx_t0) * TX_RATE / 1000000u : 0;

	return n < txw_n ? (uint32_t)n : txw_n;
}

static bool t_start(void)
{
	if (tx_refuse)
		return false;
	CHECK(!started, "the receive tap stopped before transmitting");
	tx_on = true;
	tx_t0 = tclock;
	tx_starts++;
	return true;
}

static uint16_t t_write(const int16_t *b, uint16_t n)
{
	uint32_t room = TX_RING - (txw_n - t_out());
	uint16_t k = (uint16_t)(n < room ? n : room);

	CHECK(txw_n + k <= TX_MAX, "the test's buffer");
	memcpy(txw + txw_n, b, k * sizeof b[0]);
	txw_n += k;
	return k;
}

static void t_stop(void)
{
	tx_on = false;
	tx_stops++;
}

static uint16_t t_read_none(int16_t *buf, uint16_t max)
{
	(void)buf;
	(void)max;
	CHECK(started && !tx_on, "receive reads only while the receive tap runs, never while transmitting");
	return 0;
}

static uint32_t t_rx_time(void) { return tclock; }

static hal_baseband_t bbt = {
	.rx_rate_hz = RATE, .rx_channels = 1, .rx_lo_hz = 25, .rx_hi_hz = 3000, .rx_slope_hz = 25,
	.rx_start = bb_start, .rx_read = t_read_none, .rx_stop = bb_stop,
	.tx_rate_hz = TX_RATE, .tx_lo_hz = 20, .tx_hi_hz = 2800, .tx_counts_per_khz = TX_CPK,
	.tx_start = t_start, .tx_write = t_write, .tx_stop = t_stop, .now_us = t_now, .rx_time_us = t_rx_time,
};

// A recursive lock: a mode's hooks queue from inside a poll
static void tlk(void) { tx_depth++; }
static void tunlk(void) { tx_depth--; }

// The receiver's and the transmitter's state in one place, as a board short of memory lays them
static union { air_softmodem_t rx; air_softmodem_tx_t tx; } tmem;
static const air_tap_cfg_t tcfg = { .bb = &bbt, .sm = &tmem.rx, .tx = &tmem.tx, .fit_flags = AIR_FIT_UNTESTED,
				    .lock = tlk, .unlock = tunlk };

static bool queue_next(void)
{
	air_tx_chunk_t c;
	uint16_t n = (uint16_t)(air_n - tx_next < air_tap.tx_chunk_syms ? air_n - tx_next : air_tap.tx_chunk_syms);

	if (n == 0)
		return false;
	memset(&c, 0, sizeof c);
	c.nsyms = n;
	for (uint16_t i = 0; i < n; i++)
		air_sym_put(c.syms, i, air[tx_next + i]);
	c.flags = tx_next + n == air_n ? AIR_TX_LAST : 0;
	if (!air_tap.tx_queue(&c))
		return false;
	tx_next += n;
	return true;
}

static bool tx_feed = true;		// the "mode" queues on each TX_SPACE

static void on_tx_event(const air_event_t *ev, void *ctx)
{
	(void)ctx;
	CHECK(tx_depth == 1, "an event inside the poll's lock: depth %d", tx_depth);
	CHECK(ev->kind != AIR_EV_RX || !tx_on, "no receive while transmitting");
	if (ev->kind == AIR_EV_RX)
		return;
	if (ntev < 16) {
		tev[ntev].kind = ev->kind;
		tev[ntev].t_us = ev->t_us;
		ntev++;
	}
	nkind[ev->kind & 7]++;
	last_t[ev->kind & 7] = ev->t_us;
	last_kind = ev->kind;
	if (ev->kind == AIR_EV_TX_STARTED)
		started_lead = tmem.tx.lead;	// (the receiver takes the memory back after)
	if ((ev->kind == AIR_EV_TX_SPACE || ev->kind == AIR_EV_TX_UNDERRUN) && tx_feed)
		while (air_tap.tx_space() && queue_next())
			;
}

static uint16_t count_ev(uint8_t kind)
{
	return nkind[kind & 7];
}

static void tx_reset_test(void)
{
	txw_n = 0;
	ntev = 0;
	memset(nkind, 0, sizeof nkind);
	tx_next = 0;
	tx_starts = tx_stops = 0;
	tx_feed = true;
	starts = stops = 0;
}

// Polls every step_us until AIR_EV_TX_DONE or max_ms
static bool tx_run(uint32_t step_us, uint32_t max_ms)
{
	for (uint32_t t = 0; t < max_ms * 1000; t += step_us) {
		tclock += step_us;
		CHECK(air_tap_poll(), "poll with a session");
		if (count_ev(AIR_EV_TX_DONE))
			return true;
	}
	return false;
}

static void sm_chunk(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	if (c->sync_at >= 0 && out_nsync < 256)
		out_sync[out_nsync++].at = out_n + (uint32_t)c->sync_at;
	for (uint16_t i = 0; i < c->nsyms && out_n < OUT_MAX; i++)
		out_hard[out_n++] = air_sym_get(c->hard, i);
}

// Received back at the board's rate and scale: the symbols out against air[] from the first frame sync
static uint32_t tx_errors(const air_profile_t *p, uint32_t *ncmp)
{
	static air_softmodem_t m;
	uint32_t err = 0, o, a;

	memset(txw + txw_n, 0, 2000 * sizeof txw[0]);	// the carrier after: the receiver's delay line empties
	out_reset(p->sym_rate);
	CHECK(air_softmodem_rx_open(&m, p, TX_RATE, TX_CPK, 0, sm_chunk, NULL), "receiver");
	for (uint32_t from = 0; from < txw_n + 2000; from += 240)
		air_softmodem_rx(&m, txw + from, 240, T0 + (uint32_t)((uint64_t)from * 1000000u / TX_RATE));
	*ncmp = 0;
	if (!out_nsync)
		return 0xFFFFFFFF;
	// The first sync sent is at symbol 100
	for (o = out_sync[0].at, a = 100; o < out_n && a < air_n; o++, a++, (*ncmp)++)
		err += out_hard[o] != air[a];
	return err;
}

static void testTransmit(void)
{
	const air_profile_t *p = &p25_air_mode.profile;
	uint32_t f, ncmp, err;

	air_tap_setup(&tcfg);
	f = air_tap.fit(p);
	CHECK((f & (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT)) == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT) && !(f & AIR_FIT_FULL_DUPLEX),
	      "both taps: receive and transmit, half duplex: %#x", (unsigned)f);
	CHECK(air_tap.tx_chunk_syms && air_tap.tx_queue_depth && air_tap.tx_lead_ms, "transmit constants");

	// A P25 stream of 20 frames, queued as the session does: the queue full, then begin, then on each TX_SPACE
	air_stream(p, 0, 864 * 20, 100, 864);
	tx_reset_test();
	CHECK(air_tap.open(p, on_tx_event, NULL) && started && starts == 1, "open: the receive tap runs");
	CHECK(!air_tap_transmitting() && air_tap_is_open(), "open, not transmitting");
	while (air_tap.tx_space() && queue_next())
		;
	CHECK(air_tap.tx_space() == 0 && tx_next == 4u * air_tap.tx_chunk_syms, "the queue takes %u chunks", air_tap.tx_queue_depth);
	CHECK(air_tap.tx_begin(), "begin");
	CHECK(tx_on && !started && stops == 1 && air_tap_transmitting(), "the receive tap stopped, the transmit tap runs");
	CHECK(txw_n > 0, "samples before the board's clock started: %u", txw_n);
	CHECK(ntev == 0, "no event inside begin (the radio core's task)");
	CHECK(tx_run(2000, 6000), "done within 6 s");
	CHECK(count_ev(AIR_EV_TX_STARTED) == 1 && tev[0].kind == AIR_EV_TX_STARTED, "TX_STARTED first, once");
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 0, "no underrun: %u", count_ev(AIR_EV_TX_UNDERRUN));
	CHECK(count_ev(AIR_EV_TX_DONE) == 1 && last_kind == AIR_EV_TX_DONE, "TX_DONE last, once");
	CHECK(tx_next == air_n, "every symbol queued");
	CHECK(!tx_on && tx_stops == 1 && started && starts == 2, "the transmit tap stopped, the receive tap runs again");
	CHECK(t_out() == txw_n || !tx_on, "every sample out before done");
	// Done once the last sample is out, not before
	{
		uint32_t end = tx_t0 + (uint32_t)((uint64_t)txw_n * 1000000u / TX_RATE);
		int32_t d = (int32_t)(last_t[AIR_EV_TX_DONE] - end);

		CHECK(d >= 0 && d <= 2100, "TX_DONE %d us after the last sample", (int)d);
	}
	// Symbol 0's center: lead symbols after sample 0
	{
		uint32_t want = tx_t0 + (uint32_t)(started_lead * 1000000u / p->sym_rate);
		int32_t d = (int32_t)(tev[0].t_us - want);

		CHECK(d >= -2 && d <= 2, "TX_STARTED at symbol 0's center: off %d us", (int)d);
	}
	err = tx_errors(p, &ncmp);
	printf("  transmit, P25 20 frames: %u samples, lead %u symbols, %u TX_SPACE, received back %u errors in %u\n", txw_n,
	       started_lead, count_ev(AIR_EV_TX_SPACE), err, ncmp);
	CHECK(err == 0 && ncmp > 864 * 19, "received back: %u errors in %u", err, ncmp);
	air_tap.close();

	// The mode queues one chunk and then nothing for a while: idle symbols, one underrun, then the rest
	air_stream(p, 0, 864 * 2, 100, 864);
	tx_reset_test();
	CHECK(air_tap.open(p, on_tx_event, NULL), "open");
	tx_feed = false;
	queue_next();
	CHECK(air_tap.tx_begin(), "begin");
	for (int i = 0; i < 25; i++) {
		tclock += 2000;
		air_tap_poll();
	}
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 1, "one underrun while dry: %u", count_ev(AIR_EV_TX_UNDERRUN));
	tx_feed = true;
	while (air_tap.tx_space() && queue_next())
		;
	CHECK(tx_run(2000, 3000), "done after the rest");
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 1 && tx_next == air_n, "still one underrun, every symbol queued");
	air_tap.close();

	// A poll 60 ms late: the board ran dry, one underrun, the end still comes
	air_stream(p, 0, 864 * 4, 100, 864);
	tx_reset_test();
	CHECK(air_tap.open(p, on_tx_event, NULL), "open");
	while (air_tap.tx_space() && queue_next())
		;
	CHECK(air_tap.tx_begin(), "begin");
	tclock += 2000;
	air_tap_poll();
	tclock += 60000;
	air_tap_poll();
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 1, "a late poll: one underrun: %u", count_ev(AIR_EV_TX_UNDERRUN));
	CHECK(tx_run(2000, 3000), "done");
	air_tap.close();

	// Abort: the transmit tap stops at once, TX_DONE at the next poll, the receive tap runs again
	air_stream(p, 0, 864 * 4, 100, 864);
	tx_reset_test();
	CHECK(air_tap.open(p, on_tx_event, NULL), "open");
	while (air_tap.tx_space() && queue_next())
		;
	CHECK(air_tap.tx_begin(), "begin");
	tclock += 2000;
	air_tap_poll();
	air_tap.tx_abort();
	CHECK(!tx_on && tx_stops == 1 && started && !air_tap_transmitting(), "abort: transmit tap stopped, receive tap on");
	tclock += 2000;
	air_tap_poll();
	CHECK(count_ev(AIR_EV_TX_DONE) == 1, "abort: TX_DONE");
	// Aborted before begin (queued, never keyed): TX_DONE too, nothing started
	while (air_tap.tx_space() && queue_next())
		;
	air_tap.tx_abort();
	tclock += 2000;
	air_tap_poll();
	CHECK(count_ev(AIR_EV_TX_DONE) == 2 && tx_starts == 1, "abort while queued: TX_DONE, never started");
	// The board refuses to start: begin fails, the receive tap runs again
	tx_refuse = true;
	tx_next = 0;
	while (air_tap.tx_space() && queue_next())
		;
	CHECK(!air_tap.tx_begin() && started && !air_tap_transmitting(), "refused: begin fails, receiving again");
	tx_refuse = false;
	air_tap.close();
	CHECK(!started && !tx_on, "closed: both taps stopped");
}

int main(void)
{
	testFit();
	testOpenClose();
	testP25Ser();
	testNxdnSer();
	testCall();
	testHoleRestart();
	testPolarity();
	testP25Wander();	// last but transmit: the cases above keep their noise draws
	testTransmit();
	printf("airTapTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}

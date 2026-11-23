// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's transmitter (core/air/air_softmodem_tx.c): what it fits, its samples against the pulses computed
// here in floating point, an equalizer's look-ahead and polarity, a low-end filter's look-ahead, and a whole P25 call
// and an NXDN stream sent through it and received by the soft modem's receiver, flat and through a path with two
// high-pass corners undone by the low-end filter.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_framer.h"
#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "nxdn/nxdn.h"
#include "p25/p25.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xBEEF01;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void fill(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = (uint8_t)rng();
}

static bool t_start(void) { return true; }
static uint16_t t_write(const int16_t *b, uint16_t n) { (void)b; return n; }
static void t_stop(void) { }

#define CPK	4000		// counts per kHz
static hal_baseband_t tap_of(uint32_t rate)
{
	hal_baseband_t bb;

	memset(&bb, 0, sizeof bb);
	bb.tx_rate_hz = rate;
	bb.tx_lo_hz = 0;
	bb.tx_hi_hz = 3000;
	bb.tx_counts_per_khz = CPK;
	bb.tx_start = t_start;
	bb.tx_write = t_write;
	bb.tx_stop = t_stop;
	return bb;
}

// The pulse at sps samples a symbol, eight symbols each side, its taps summing to sps (floating point, from the
// standards' frequency responses: raised cosine or root raised cosine 0.2 times a shaper)
static void ref_pulse(double *h, int sps, bool nxdn)
{
	double sum = 0, f1 = 0.4, f2 = 0.6;

	for (int i = 0; i <= 16 * sps; i++) {
		double t = (double)(i - 8 * sps) / sps, v = 0;

		for (double f = 1.0 / 1920; f < f2; f += 1.0 / 960) {
			double hf = f < f1 ? 1 : 0.5 + 0.5 * cos(M_PI * (f - f1) / (f2 - f1)), x = M_PI * f;

			if (nxdn)
				hf = sqrt(hf) * sin(x) / x;
			else
				hf *= x / sin(x);
			v += hf * cos(2 * M_PI * f * t);
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i <= 16 * sps; i++)
		h[i] *= sps / sum;
}

// ── Symbols and what comes out ──
#define SYMS_MAX	9000
#define SPS_MAX		10
static uint8_t air[SYMS_MAX];
static uint32_t air_n;
static int16_t wave[(SYMS_MAX + 64 + 100) * SPS_MAX];
static uint32_t wave_n;

// air[] through the transmitter, then its tail: wave[] holds every sample out; returns the transmitter's lead
static uint8_t send(const air_profile_t *p, const hal_baseband_t *bb)
{
	static air_softmodem_tx_t m;

	wave_n = 0;
	if (!air_softmodem_tx_open(&m, p, bb))
		return 0xFF;
	for (uint32_t k = 0; k < air_n; k++)
		wave_n += air_softmodem_tx_sym(&m, air[k], wave + wave_n);
	for (uint8_t k = 0; k < air_softmodem_tx_tail(&m); k++)
		wave_n += air_softmodem_tx_sym(&m, AIR_SOFTMODEM_TX_NONE, wave + wave_n);
	return m.lead;
}

// A path's low end: high-pass corners at 72.6 and 124.6 Hz (an audio input's coupling, as one radio measures), and
// the low-end filter for them at 4800 symbols/s
// tools/air_tx_lf.py --corners 72.6,124.6 --rate 4800 --taps 97 --at 48 --lam 0.003
#define LF_TAPS		97
#define LF_AT		48
static const int16_t lf_path[LF_TAPS] = {
	-154, -193, -235, -279, -326, -375, -427, -482, -539, -599, -662, -728,
	-797, -868, -942, -1020, -1100, -1183, -1269, -1358, -1450, -1545, -1644, -1745,
	-1850, -1957, -2068, -2182, -2299, -2420, -2544, -2671, -2801, -2934, -3071, -3211,
	-3354, -3501, -3651, -3804, -3961, -4120, -4284, -4450, -4619, -4792, -4968, -5148,
	13173, -1052, -985, -922, -861, -802, -746, -693, -641, -592, -546, -501,
	-459, -419, -381, -345, -311, -279, -248, -220, -193, -168, -145, -124,
	-104, -85, -68, -52, -38, -25, -14, -4, 6, 14, 21, 26,
	31, 35, 38, 40, 42, 42, 42, 41, 40, 38, 35, 32,
	29,
};

// wave[] through the corners at rate (first-order sections, matched z, unity gain at half the rate)
static void highpass(uint32_t rate)
{
	static const double corners[2] = { 72.6, 124.6 };

	for (int c = 0; c < 2; c++) {
		double z = exp(-2 * M_PI * corners[c] / rate), xp = 0, yp = 0;

		for (uint32_t j = 0; j < wave_n; j++) {
			double x = wave[j];

			yp = (x - xp) * (1 + z) / 2 + z * yp;
			xp = x;
			wave[j] = (int16_t)lround(yp > 32767 ? 32767 : yp < -32768 ? -32768 : yp);
		}
	}
}

static void testFit(void)
{
	air_profile_t p = p25_air_mode.profile, q;
	hal_baseband_t bb = tap_of(24000);
	int16_t eq[3] = { 0, 16384, 0 };

	CHECK(air_softmodem_tx_fit(&p, &bb) == AIR_FIT_TX, "P25 at 24 kHz");
	CHECK(air_softmodem_tx_fit(&nxdn_air_mode.profile, &bb) == AIR_FIT_TX, "NXDN 4800 at 24 kHz (10 a symbol)");
	CHECK(air_softmodem_tx_fit(&nxdn_air_mode_9600.profile, &bb) == AIR_FIT_TX, "NXDN 9600 at 24 kHz");
	bb.tx_rate_hz = 48000;
	CHECK(air_softmodem_tx_fit(&p, &bb) == AIR_FIT_TX, "P25 at 48 kHz");
	CHECK(air_softmodem_tx_fit(&nxdn_air_mode.profile, &bb) == 0, "NXDN 4800 at 48 kHz: 20 a symbol");
	bb.tx_rate_hz = 16000;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "16 kHz: not a whole number a symbol");
	bb = tap_of(24000);
	bb.tx_counts_per_khz = 0;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "no scale: no transmit");
	bb = tap_of(24000);
	bb.tx_start = NULL;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "no transmit tap");
	bb = tap_of(24000);
	bb.tx_hi_hz = 2500;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "a path cut at 2.5 kHz can't carry 4800 symbols/s");
	bb = tap_of(24000);
	bb.tx_lo_hz = 300;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "an audio band path can't carry four levels");
	bb = tap_of(24000);
	bb.tx_eq = eq;
	bb.tx_eq_n = 3;
	bb.tx_eq_at = 3;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "the equalizer's time 0 past its end");
	bb.tx_eq_at = 1;
	CHECK(air_softmodem_tx_fit(&p, &bb) == AIR_FIT_TX, "a 3-tap equalizer");
	bb = tap_of(24000);
	q = p;
	q.shape = AIR_SHAPE_NRZ;
	CHECK(air_softmodem_tx_fit(&q, &bb) == 0, "no table for square pulses");
	q = p;
	q.timing = AIR_TIMING_TDMA;
	CHECK(air_softmodem_tx_fit(&q, &bb) == 0, "no slots");
	// The low-end filter: at the profile's rate or twice it, its symbol inside it, within the ring
	bb = tap_of(24000);
	bb.tx_lf = lf_path;
	bb.tx_lf_n = LF_TAPS;
	bb.tx_lf_at = LF_AT;
	bb.tx_lf_rate = 4800;
	CHECK(air_softmodem_tx_fit(&p, &bb) == AIR_FIT_TX, "P25 with a low-end filter at 4800");
	CHECK(air_softmodem_tx_fit(&nxdn_air_mode.profile, &bb) == AIR_FIT_TX, "NXDN 4800 (2400 symbols/s) with it in pairs");
	bb.tx_lf_rate = 3600;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "a low-end filter for another rate");
	bb.tx_lf_rate = 4800;
	bb.tx_lf_at = LF_TAPS;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "the low-end filter's symbol past its end");
	bb.tx_lf_at = LF_AT;
	bb.tx_lf_n = 200;
	CHECK(air_softmodem_tx_fit(&p, &bb) == 0, "200 taps a symbol: past the ring");
	CHECK(air_softmodem_tx_fit(&nxdn_air_mode.profile, &bb) == AIR_FIT_TX, "200 taps in pairs: 100 symbols");
	// The size the header gives (a board lays it over memory its receive tap leaves)
	CHECK(sizeof(air_softmodem_tx_t) <= 540, "transmit state %zu bytes", sizeof(air_softmodem_tx_t));
}

// Random symbols at 5 and 10 samples a symbol against the floating point pulse: the error far under a level
static void testWave(void)
{
	static const struct { const air_mode_t *mode; uint32_t rate; bool nxdn; const char *what; } t[] = {
		{ &p25_air_mode, 24000, false, "P25, 5 a symbol" },
		{ &p25_air_mode, 48000, false, "P25, 10 a symbol" },
		{ &nxdn_air_mode, 24000, true, "NXDN 4800, 10 a symbol" },
		{ &nxdn_air_mode_9600, 24000, true, "NXDN 9600, 5 a symbol" },
	};
	static double h[16 * SPS_MAX + 1];

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		const air_profile_t *p = &t[i].mode->profile;
		hal_baseband_t bb = tap_of(t[i].rate);
		int sps = (int)(t[i].rate / p->sym_rate);
		double unit = p->dev_outer_hz / 3.0 * CPK / 1000, se = 0, ss = 0;
		uint8_t lead;

		air_n = 2000;
		for (uint32_t k = 0; k < air_n; k++)
			air[k] = (uint8_t)(rng() & 3);
		lead = send(p, &bb);
		CHECK(lead != 0xFF && wave_n == (air_n + 16u * 10 / (unsigned)sps - 1) * (unsigned)sps, "%s: open, %u samples", t[i].what, wave_n);
		ref_pulse(h, sps, t[i].nxdn);
		// Output sample j is at time j - lead x sps from symbol 0's center
		for (uint32_t j = 0; j < wave_n; j++) {
			double want = 0;

			for (int k = 0; k < (int)air_n; k++) {
				int d = (int)j - lead * sps - k * sps + 8 * sps;

				if (d >= 0 && d <= 16 * sps)
					want += air_sym_level(air[k]) * unit * h[d];
			}
			se += (wave[j] - want) * (wave[j] - want);
			ss += want * want;
		}
		double snr = 10 * log10(ss / se);
		printf("  %s: lead %u symbols, waveform SNR %.1f dB\n", t[i].what, lead, snr);
		CHECK(snr > 40, "%s: waveform SNR %.1f dB", t[i].what, snr);
	}
	// A run of +3 sits at the outer deviation
	{
		const air_profile_t *p = &p25_air_mode.profile;
		hal_baseband_t bb = tap_of(24000);

		air_n = 200;
		memset(air, 1, air_n);
		send(p, &bb);
		double want = p->dev_outer_hz * CPK / 1000.0;
		CHECK(fabs(wave[500] - want) < want / 200, "a run of +3: %d counts, want %.0f", wave[500], want);
	}
}

// An equalizer that is only a delay: the same samples, the lead longer by the look-ahead. One that inverts: the
// samples inverted
static void testEq(void)
{
	static int16_t flat[(SYMS_MAX + 64) * SPS_MAX];
	static int16_t eq[81];
	const air_profile_t *p = &p25_air_mode.profile;
	hal_baseband_t bb = tap_of(24000);
	uint32_t n, diff = 0;
	uint8_t lead0, lead;

	air_n = 1500;
	for (uint32_t k = 0; k < air_n; k++)
		air[k] = (uint8_t)(rng() & 3);
	lead0 = send(p, &bb);
	n = wave_n;
	memcpy(flat, wave, n * sizeof flat[0]);
	memset(eq, 0, sizeof eq);
	eq[40] = 16384;
	bb.tx_eq = eq;
	bb.tx_eq_n = 81;
	bb.tx_eq_at = 40;
	lead = send(p, &bb);
	CHECK(lead == lead0, "a centered delta: lead %u, flat %u", lead, lead0);
	eq[40] = 0;
	eq[0] = 16384;		// all look-ahead: 40 samples, 8 symbols
	lead = send(p, &bb);
	// The response comes 8 symbols earlier and the output waits for it: the same samples
	CHECK(lead == lead0 + 8, "a delta 8 symbols ahead: lead %u against %u", lead, lead0);
	for (uint32_t j = 0; j < n && j < wave_n; j++)
		if (abs(wave[j] - flat[j]) > 1)
			diff++;
	CHECK(diff == 0, "a delta 8 symbols ahead: %u samples differ from the flat output", diff);
	eq[0] = 0;
	eq[40] = -16384;
	lead = send(p, &bb);
	diff = 0;
	for (uint32_t j = 0; j < n; j++)
		if (abs(wave[j] + flat[j]) > 1)
			diff++;
	CHECK(lead == lead0 && diff == 0, "inverted: %u samples differ", diff);
}

// ── Received back by the soft modem's receiver ──
static p25_rx_unit_t got[16];
static uint8_t got_air[16][P25_UNIT_BYTES_MAX];
static uint8_t ngot;
static air_framer_t *framer;
static uint32_t nsyncs;

static void on_unit(const p25_rx_unit_t *u, void *ctx)
{
	(void)ctx;
	if (ngot < 16) {
		got[ngot] = *u;
		memcpy(got_air[ngot], u->air, (size_t)(u->nsyms + 3) / 4);
		ngot++;
	}
}

static void on_lost(void *ctx) { (void)ctx; }

static void on_chunk(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	if (c->sync_at >= 0)
		nsyncs++;
	if (framer)
		air_framer_rx(framer, c);
}

static void air_add(const uint8_t *unit, uint16_t nsyms)
{
	for (uint16_t i = 0; i < nsyms; i++)
		air[air_n++] = air_sym_get(unit, i);
}

// Received at the transmitter's own rate and scale (a flat tap), with noise of sigma counts
static void receive(const air_profile_t *p, uint32_t rate, double sigma)
{
	static air_softmodem_t m;
	static int16_t buf[256];

	CHECK(air_softmodem_rx_open(&m, p, rate, CPK, 0, on_chunk, NULL), "receiver open");
	// The carrier stays on a while after the last sample: the receiver's delay line empties
	memset(wave + wave_n, 0, 100 * SPS_MAX * sizeof wave[0]);
	wave_n += 100 * SPS_MAX;
	for (uint32_t from = 0; from < wave_n; from += 256) {
		uint16_t n = (uint16_t)(wave_n - from < 256 ? wave_n - from : 256);

		for (uint16_t i = 0; i < n; i++) {
			double u = (rng() % 1000000 + 1) / 1000001.0, v = (rng() % 1000000) / 1000000.0;
			double x = wave[from + i] + sigma * sqrt(-2 * log(u)) * cos(2 * M_PI * v);

			buf[i] = (int16_t)lround(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
		}
		air_softmodem_rx(&m, buf, n, (uint32_t)((uint64_t)from * 1000000 / rate));
	}
}

// A P25 call: a header, two LDU pairs, a terminator, with random header, voice and encryption sync
static uint8_t c_hdr[P25_HDR_BYTES], c_lc[P25_LC_BYTES], c_es[P25_ES_BYTES];
static uint8_t c_voice[4][P25_VOICE_FRAMES][P25_IMBE_BYTES];

static void build_call(void)
{
	uint8_t unit[P25_UNIT_BYTES_MAX];
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 1001, .src = 1234567 };

	fill(c_hdr, sizeof c_hdr);
	fill(c_es, sizeof c_es);
	p25_lc_build(&lc, c_lc);
	air_n = 0;
	for (uint16_t i = 0; i < 40; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	p25_unit_begin(unit, P25_HDU_SYMS, 0x293, P25_DUID_HDU, P25_SS_UNKNOWN);
	p25_hdu_put(unit, c_hdr);
	air_add(unit, P25_HDU_SYMS);
	for (uint8_t k = 0; k < 4; k++) {
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, k & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1, P25_SS_UNKNOWN);
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			fill(c_voice[k][n], P25_IMBE_BYTES);
			p25_ldu_voice_put(unit, n, c_voice[k][n]);
		}
		if (k & 1)
			p25_ldu_es_put(unit, c_es);
		else
			p25_ldu_lc_put(unit, c_lc);
		air_add(unit, P25_LDU_SYMS);
	}
	p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_UNKNOWN);
	p25_tdulc_put(unit, c_lc);
	air_add(unit, P25_TDULC_SYMS);
}

// wave[] received (300 Hz of noise): the units back, and the voice frames wrong. check: every unit's content checked
static uint8_t receive_call(bool check, uint8_t *bad_voice)
{
	static uint8_t fbuf[P25_UNIT_BYTES_MAX];
	static int8_t fsoft[P25_UNIT_SYMS_MAX];
	const air_profile_t *p = &p25_air_mode.profile;
	uint8_t out[16];
	p25_lc_t lcgot;
	air_framer_t fr;
	p25_rx_t rx;

	*bad_voice = 0;
	ngot = 0;
	p25_rx_init(&rx, 0x293, on_unit, on_lost, NULL);
	air_framer_init(&fr, &p25_air_mode, &rx, fbuf, fsoft, P25_UNIT_SYMS_MAX);
	framer = &fr;
	receive(p, 24000, 0.3 * CPK);
	framer = NULL;
	for (uint8_t k = 0; k < ngot && k < 6; k++) {
		if (k == 0) {
			if (check)
				CHECK(got[k].duid == P25_DUID_HDU && p25_hdu_get(got_air[k], out) >= 0 && !memcmp(out, c_hdr, P25_HDR_BYTES), "HDU");
		} else if (k == 5) {
			if (check)
				CHECK(got[k].duid == P25_DUID_TDULC && p25_tdulc_get(got_air[k], out) >= 0 && !memcmp(out, c_lc, P25_LC_BYTES), "TDULC");
		} else {
			if (check && ((k - 1) & 1))
				CHECK(p25_ldu_es_get(got_air[k], out) >= 0 && !memcmp(out, c_es, P25_ES_BYTES), "ES %u", k);
			else if (check)
				CHECK(p25_ldu_lc_get(got_air[k], out) >= 0 && p25_lc_parse(out, &lcgot) && lcgot.tgid == 1001 && lcgot.src == 1234567, "LC %u", k);
			for (uint8_t v = 0; v < P25_VOICE_FRAMES; v++)
				if (p25_ldu_voice_get(got_air[k], v, out) < 0 || memcmp(out, c_voice[k - 1][v], P25_IMBE_BYTES))
					(*bad_voice)++;
		}
	}
	return ngot;
}

static void testCall(void)
{
	hal_baseband_t bb = tap_of(24000);
	uint8_t bad_voice, n;

	build_call();
	send(&p25_air_mode.profile, &bb);
	n = receive_call(true, &bad_voice);
	CHECK(n == 6, "%u units", n);
	CHECK(bad_voice == 0, "%u voice frames wrong", bad_voice);
	printf("  P25 call sent and received: %u units, %u voice frames wrong\n", n, bad_voice);
}

// NXDN 4800 at ten samples a symbol: random symbols with the frame sync every 192, every one found
static uint32_t nxdn_syncs(const hal_baseband_t *bb, bool path)
{
	const air_profile_t *p = &nxdn_air_mode.profile;
	const air_sync_t *s = &p->sync[0];

	air_n = 192 * 12;
	for (uint32_t k = 0; k < air_n; k++)
		air[k] = (uint8_t)(rng() & 3);
	for (uint32_t at = 100; at + s->nsyms <= air_n; at += 192)
		for (uint8_t i = 0; i < s->nsyms; i++)
			air[at + i] = air_sym_get(s->syms, i);
	send(p, bb);
	if (path)
		highpass(24000);
	nsyncs = 0;
	receive(p, 24000, 0.15 * CPK);
	return nsyncs;
}

static void testNxdn(void)
{
	hal_baseband_t bb = tap_of(24000);
	uint32_t n = nxdn_syncs(&bb, false);

	CHECK(n == 12, "NXDN 4800: %u syncs of 12", n);
	printf("  NXDN 4800 sent and received: %u syncs of 12\n", n);
}

// A low-end filter that is only a delay: the same samples, the lead longer by its look-ahead
static void testLfDelay(void)
{
	static int16_t flat[(SYMS_MAX + 64) * SPS_MAX];
	static int16_t lf[9];
	const air_profile_t *p = &p25_air_mode.profile;
	hal_baseband_t bb = tap_of(24000);
	uint32_t n, diff = 0;
	uint8_t lead0, lead, tail0;
	air_softmodem_tx_t m;

	air_n = 1500;
	for (uint32_t k = 0; k < air_n; k++)
		air[k] = (uint8_t)(rng() & 3);
	lead0 = send(p, &bb);
	n = wave_n;
	memcpy(flat, wave, n * sizeof flat[0]);
	air_softmodem_tx_open(&m, p, &bb);
	tail0 = air_softmodem_tx_tail(&m);
	memset(lf, 0, sizeof lf);
	lf[0] = 16384;		// the newest symbol: 8 ahead of the one it is for
	bb.tx_lf = lf;
	bb.tx_lf_n = 9;
	bb.tx_lf_at = 8;
	bb.tx_lf_rate = 4800;
	lead = send(p, &bb);
	CHECK(lead == lead0 + 8, "a low-end filter 8 symbols ahead: lead %u against %u", lead, lead0);
	for (uint32_t j = 0; j < n; j++)
		if (wave[j] != flat[j])
			diff++;
	CHECK(diff == 0, "a low-end filter 8 symbols ahead: %u samples differ from none", diff);
	air_softmodem_tx_open(&m, p, &bb);
	CHECK(air_softmodem_tx_tail(&m) == tail0 + 8, "its tail %u against %u", air_softmodem_tx_tail(&m), tail0);
}

// Through a path with two high-pass corners: the call and the NXDN stream, with the low-end filter for them (at 4800
// symbols/s; NXDN 4800 takes its taps in pairs) and without
static void testLfPath(void)
{
	hal_baseband_t bb = tap_of(24000);
	uint8_t bad, n, bad0, n0;
	uint32_t s, s0;

	build_call();
	send(&p25_air_mode.profile, &bb);
	highpass(24000);
	n0 = receive_call(false, &bad0);
	bb.tx_lf = lf_path;
	bb.tx_lf_n = LF_TAPS;
	bb.tx_lf_at = LF_AT;
	bb.tx_lf_rate = 4800;
	send(&p25_air_mode.profile, &bb);
	highpass(24000);
	n = receive_call(true, &bad);
	CHECK(n == 6 && bad == 0, "P25 through the corners with the low-end filter: %u units, %u voice frames wrong", n, bad);
	s = nxdn_syncs(&bb, true);
	CHECK(s == 12, "NXDN 4800 through the corners with the low-end filter: %u syncs of 12", s);
	bb.tx_lf = NULL;
	s0 = nxdn_syncs(&bb, true);
	printf("  through two high-pass corners: P25 %u units, %u voice frames wrong (without the low-end filter %u, %u); "
	       "NXDN 4800 %u syncs of 12 (without %u)\n", n, bad, n0, bad0, s, s0);
}

int main(void)
{
	testFit();
	testWave();
	testEq();
	testCall();
	testNxdn();
	testLfDelay();
	testLfPath();
	printf("airSoftmodemTxTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}

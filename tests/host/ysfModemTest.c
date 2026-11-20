// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for the System Fusion C4FM modem (core/ysf/ysf_c4fm.c, docs/spec/ysf-modem.md 2) and the engine on a
 * simulated radio link: the transmitter's dibits (ysf_frame.c) through the modulator, an FM modulator at 48 kHz,
 * white noise at a given Eb/N0, a channel filter, an FM discriminator, the receiver's audio path (a 3.6 kHz low-pass
 * and 8 kHz samples, as a radio's PCM path delivers them), the demodulator and the receiver. Voice frames carry known
 * pseudo-random bits, so the voice bits the receiver hands back give the bit error rate: over the 22 bits V/D mode 2
 * sends once (the channel's own BER) and over all 49 after the repetition vote (what the codec gets).
 *
 * Impairments: carrier offset, a carrier drifting through the transmission, the transmitter's symbol clock off by
 * some ppm, inverted levels, a first-order high-pass in the receive audio, the modulator run at 8 kHz as on the radio,
 * the deviation scaled (a DMR modem's levels, the narrow setting), the channel filter. The assertions are margins
 * under what the sweep measured; --sweep prints the table (docs/spec/ysf-modem.md 2.5).
 *
 *   make -C tests/host run-ysf          ysfModemTest --sweep    the full table (a few minutes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ysf/ysf_c4fm.h"
#include "ysf/ysf_frame.h"
#include "ysf/ysf_engine.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FS		48000.0	// the simulated air
#define DECIM		6	// to the receiver's 8 kHz
#define RX_RATE		8000u
#define DISC_SCALE	3000.0	// discriminator samples per 900 Hz (a +1 symbol)

// --- a deterministic noise source (xorshift64*, Box-Muller) ---

static uint64_t rng_s = 1;

static double urand(void)
{
	rng_s ^= rng_s >> 12;
	rng_s ^= rng_s << 25;
	rng_s ^= rng_s >> 27;
	return ((double)((rng_s * 0x2545F4914F6CDD1DULL) >> 11) + 0.5) / 9007199254740992.0;
}

static double gauss(void)
{
	return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand());
}

// --- FIR filters, windowed sinc ---

typedef struct {
	int n;
	double h[129];
	double xr[129], xi[129];
	int pos;
} fir_t;

static void fir_init(fir_t *f, int taps, double cutoff_hz, double fs)
{
	memset(f, 0, sizeof(*f));
	f->n = taps;
	double sum = 0;
	for (int i = 0; i < taps; i++) {
		double t = i - (taps - 1) / 2.0;
		double w = 0.42 - 0.5 * cos(2 * M_PI * i / (taps - 1)) + 0.08 * cos(4 * M_PI * i / (taps - 1));
		double x = 2 * cutoff_hz / fs;
		f->h[i] = (t == 0 ? x : sin(M_PI * x * t) / (M_PI * t)) * w;
		sum += f->h[i];
	}
	for (int i = 0; i < taps; i++) {
		f->h[i] /= sum;
	}
}

static void fir_c(fir_t *f, double *re, double *im)
{
	f->xr[f->pos] = *re;
	f->xi[f->pos] = *im;
	double ar = 0, ai = 0;
	for (int i = 0, p = f->pos; i < f->n; i++) {
		ar += f->h[i] * f->xr[p];
		ai += f->h[i] * f->xi[p];
		p = p ? p - 1 : f->n - 1;
	}
	f->pos = (f->pos + 1) % f->n;
	*re = ar;
	*im = ai;
}

// --- known voice ---

static void voice_bits(uint32_t n, uint32_t seed, uint8_t bits[YSF_VOICE_BITS])
{
	uint32_t x = n * 2654435761u + 12345u + seed * 0x9E3779B9u;
	for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		bits[i] = (uint8_t)(x & 1u);
	}
}

typedef struct {
	uint32_t sent, limit, seed;
} src_t;

static bool pull_voice(void *ctx, uint8_t bits[YSF_VOICE_BITS])
{
	src_t *s = ctx;
	if (s->sent >= s->limit) {
		return false;
	}
	voice_bits(s->sent++, s->seed, bits);
	return true;
}

// --- the link ---

typedef struct {
	double ebn0_db;		// 99 = no noise
	double offset_hz;
	double drift_hz;	// the carrier moves this much, evenly, from the start of the transmission to its end
	int32_t ppm;		// the transmitter's symbol clock error
	bool invert;
	double rx_hpf_hz;	// a first-order high-pass in the receive audio path, 0 = none
	uint16_t rx_lfr_hz;	// the restorer for it (0: none)
	double dev_scale;	// the levels against the standard's wide ones (0 = 1)
	double chan_hz;		// the channel filter's half width (0: 6250, a 12.5 kHz channel)
	bool tx_8k;		// run the modulator at 8 kHz and interpolate, as the radio does on an audio path
	uint32_t frames;	// voice frames to send (five a 100 ms frame)
	uint32_t seed;
	uint16_t preamble;	// the transmitter's preamble, symbols (0: none)
	uint8_t headers;	// its header frames (0: 2)
} link_t;

typedef struct {
	uint32_t firsts, ends, losts, voices, frames;
	uint32_t raw_errors, raw_bits;		// the 22 unprotected bits of each voice frame
	uint32_t prot_errors, prot_bits;	// the 27 bits sent three times, after the vote
	uint32_t next;
	bool counting;
	uint32_t fich_good, fich_bad, csd_good, csd_bad;
	bool callsign_ok;
	int32_t ppm, offset;
} result_t;

typedef struct {
	ysf_engine_t eng;
	result_t res;
	uint32_t seed;
} rx_side_t;

static void on_frame(void *ctx, const ysf_call_t *call, bool first)
{
	rx_side_t *r = ctx;
	r->res.frames++;
	if (first) {
		r->res.firsts++;
		// Voice lines up with what was sent only when the transmission was caught from its header
		r->res.counting = call->fich.fi == YSF_FI_HEADER;
		r->res.next = 0;
	}
}

static void on_call(void *ctx, const ysf_call_t *call)
{
	rx_side_t *r = ctx;
	char s[11];
	ysf_callsign_get(call->csd.src, s);
	r->res.callsign_ok = strcmp(s, "G4XYZ") == 0;
}

static void on_voice(void *ctx, const uint8_t bits[YSF_VOICE_BITS], uint8_t q)
{
	rx_side_t *r = ctx;
	r->res.voices++;
	if (!r->res.counting) {
		return;
	}
	uint8_t want[YSF_VOICE_BITS];
	voice_bits(r->res.next++, r->seed, want);
	for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
		uint32_t e = bits[i] != want[i];
		if (i < 27u) {
			r->res.prot_errors += e;
			r->res.prot_bits++;
		} else {
			r->res.raw_errors += e;
			r->res.raw_bits++;
		}
	}
}

static void on_end(void *ctx, ysf_end_t why)
{
	rx_side_t *r = ctx;
	r->res.ppm = ysf_c4fm_demod_ppm(&r->eng.u.rx.dm);
	r->res.offset = ysf_c4fm_demod_offset_hz(&r->eng.u.rx.dm);
	if (why == YSF_END_TERMINATOR) {
		r->res.ends++;
	} else {
		r->res.losts++;
	}
	r->res.counting = false;	// after a loss, voice by late entry can't be lined up with what was sent
}

static void tx_config(ysf_tx_config_t *c, src_t *src)
{
	memset(c, 0, sizeof(*c));
	ysf_csd_init(&c->csd, "G4XYZ", "ALL");
	c->headers = 1;
	c->order = YSF_WHITEN_DATA;
	c->pull = pull_voice;
	c->ctx = src;
}

static result_t run_link(const link_t *L)
{
	rng_s = 0x1234567ull + L->seed * 7919ull;
	src_t src = { 0, L->frames, L->seed };
	ysf_tx_config_t cfg;
	tx_config(&cfg, &src);
	cfg.headers = L->headers ? L->headers : 2;	// without a preamble the demodulator settles in the first header
	cfg.preamble = L->preamble;
	static ysf_tx_t tx;
	ysf_tx_start(&tx, &cfg);

	const int16_t amp = 3000;	// a +1 symbol; the shaped waveform peaks near 4.5 x that
	ysf_c4fm_mod_t mod;
	uint32_t mrate = L->tx_8k ? 8000u : (uint32_t)FS;
	ysf_c4fm_mod_init(&mod, mrate, amp, ysf_tx_dibit, &tx);
	ysf_c4fm_mod_ppm(&mod, L->ppm);

	static rx_side_t r;
	memset(&r, 0, sizeof(r));
	r.seed = L->seed;
	ysf_rx_cb_t cb = { on_frame, on_call, on_voice, NULL, on_end, &r };
	ysf_path_t path = { .rate = RX_RATE, .rx_hpf_hz = L->rx_lfr_hz };
	ysf_engine_rx_start(&r.eng, &path, &cb);

	// The transmitter's audio at 48 kHz, whole: straight from the modulator, or from it at 8 kHz held for six
	// samples and smoothed (the radio's PCM through its DAC's interpolation)
	size_t cap = (size_t)(L->frames / 5u + 8u) * YSF_FRAME_DIBITS * (size_t)(FS / YSF_BAUD) + 8192u, ntx = 0;
	double *txa = malloc(cap * sizeof(double));
	static fir_t chan, audio, interp;
	fir_init(&chan, 65, L->chan_hz > 0 ? L->chan_hz : 6250, FS);
	fir_init(&audio, 97, 3600, FS);
	fir_init(&interp, 97, 3600, FS);
	for (;;) {
		int16_t m[64];
		bool more = ysf_c4fm_mod(&mod, m, 64);
		// Stop the voice once it has all gone: the frame in progress finishes, then the terminator
		if (src.sent >= src.limit && !tx.stop) {
			ysf_tx_stop(&tx);
		}
		for (size_t i = 0; i < 64; i++) {
			if (L->tx_8k) {
				for (int j = 0; j < DECIM && ntx < cap; j++) {
					double re = j == 0 ? m[i] * (double)DECIM : 0, im = 0;	// zero-stuffed, the filter interpolates
					fir_c(&interp, &re, &im);
					txa[ntx++] = re / amp;
				}
			} else if (ntx < cap) {
				txa[ntx++] = (double)m[i] / amp;
			}
		}
		if (!more) {
			break;
		}
	}

	// Noise: signal power 1, Eb = Es / 2 = 1 / 9600 s; complex noise variance per sample N0 x fs
	double sigma = L->ebn0_db >= 99 ? 0 : sqrt(FS / (2.0 * YSF_BAUD * pow(10, L->ebn0_db / 10)) / 2);
	double dev = YSF_DEV_HZ * (L->dev_scale > 0 ? L->dev_scale : 1.0);
	double phase = 0, pre = 1, pim = 0, hp_x = 0, hp_y = 0;
	double a_rx = L->rx_hpf_hz > 0 ? 1 / (1 + 2 * M_PI * L->rx_hpf_hz / FS) : 1;
	int16_t out8[64];
	size_t n8 = 0;
	// Noise before the carrier (100 ms) and after it (300 ms), as a receiver hears
	size_t lead = (size_t)(FS / 10), total = lead + ntx + (size_t)(FS * 0.3);
	for (size_t k = 0; k < total; k++) {
		bool on = k >= lead && k < lead + ntx;
		double f = on ? txa[k - lead] : 0;
		double off = L->offset_hz + (on ? L->drift_hz * ((double)(k - lead) / ntx - 0.5) : 0);
		phase += 2 * M_PI * (dev * f + off) / FS;
		double carrier = on ? 1 : 0;
		double re = carrier * cos(phase) + sigma * gauss(), im = carrier * sin(phase) + sigma * gauss();
		if (!on && sigma == 0) {
			re = 1e-9 * gauss();
			im = 1e-9 * gauss();
		}
		fir_c(&chan, &re, &im);
		double dre = re * pre + im * pim, dim = im * pre - re * pim;
		pre = re;
		pim = im;
		double disc = atan2(dim, dre) * FS / (2 * M_PI) / YSF_DEV_HZ;	// 1.0 = 900 Hz
		if (L->invert) {
			disc = -disc;
		}
		if (L->rx_hpf_hz > 0) {
			hp_y = a_rx * (hp_y + disc - hp_x);
			hp_x = disc;
			disc = hp_y;
		}
		double ar = disc, ai = 0;
		fir_c(&audio, &ar, &ai);
		if (k % DECIM == 0) {
			double v = ar * DISC_SCALE;
			out8[n8++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			if (n8 == sizeof(out8) / sizeof(out8[0])) {
				ysf_engine_rx(&r.eng, out8, n8);
				n8 = 0;
			}
		}
	}
	free(txa);
	ysf_engine_rx(&r.eng, out8, n8);
	const ysf_rx_t *fr = ysf_engine_receiver(&r.eng);
	r.res.fich_good = fr->fich_good;
	r.res.fich_bad = fr->fich_bad;
	r.res.csd_good = fr->csd_good;
	r.res.csd_bad = fr->csd_bad;
	return r.res;
}

static double raw_ber(const result_t *r)
{
	return r->raw_bits ? (double)r->raw_errors / r->raw_bits : 1.0;
}

static double voice_ber(const result_t *r)
{
	uint32_t b = r->raw_bits + r->prot_bits;
	return b ? (double)(r->raw_errors + r->prot_errors) / b : 1.0;
}

// --- the pulse and the matched filter ---

static uint8_t iso_d[64];
static uint32_t iso_n, iso_i;

static int iso_pull(void *ctx)
{
	return iso_i < iso_n ? (int)iso_d[iso_i++] : -1;
}

static double rrc(double t)
{
	const double a = 0.2;
	if (fabs(t) < 1e-12) {
		return 1 - a + 4 * a / M_PI;
	}
	if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9) {
		return a / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
	}
	return (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
}

// The modulator's output (and so its table) against the root raised cosine formula: every sample of a pattern at
// 48 kHz equals the sum of the pulses of its symbols, cut at the span, within the table's resolution
static void test_mod_shape(void)
{
	static const uint8_t pat[] = { 0, 1, 2, 3, 3, 1, 0, 0, 2, 1, 3, 0, 1, 1, 2, 2, 3, 0, 1, 3 };
	iso_n = sizeof(pat);
	iso_i = 0;
	memcpy(iso_d, pat, sizeof(pat));
	ysf_c4fm_mod_t m;
	const uint32_t rate = 48000;
	CHECK(ysf_c4fm_mod_init(&m, rate, 6000, iso_pull, NULL), "init");
	static int16_t out[2000];
	ysf_c4fm_mod(&m, out, 2000);
	double worst = 0;
	uint32_t sps = rate / YSF_BAUD;
	for (uint32_t i = 0; i < (sizeof(pat) + 2u * YSF_RRC_SPAN) * sps; i++) {
		double t = (double)i / sps - YSF_RRC_SPAN;	// in symbols from the first symbol's center
		double want = 0;
		for (int k = 0; k < (int)sizeof(pat); k++) {
			if (t - k >= -YSF_RRC_SPAN && t - k < YSF_RRC_SPAN) {	// the modulator's window: from the span before to just under it after
				want += ysf_c4fm_level(pat[k]) * rrc(t - k);
			}
		}
		double d = fabs(out[i] / 6000.0 - want);
		worst = d > worst ? d : worst;
	}
	// 32 table points a symbol, interpolated: within 0.3 % of a +1 level
	CHECK(worst < 0.003 * 3, "modulator differs from the root raised cosine by %.4f of a +1 level", worst);
	CHECK(m.symbols == sizeof(pat), "symbols %u", m.symbols);
	// The table against the formula at its own points
	ysf_c4fm_mod_t one;
	uint8_t single[1] = { 0 };
	iso_n = 1;
	iso_i = 0;
	memcpy(iso_d, single, 1);
	ysf_c4fm_mod_init(&one, rate, 16384, iso_pull, NULL);
	ysf_c4fm_mod(&one, out, (2u * YSF_RRC_SPAN + 1u) * sps);
	double peak = out[YSF_RRC_SPAN * sps] / 16384.0;
	CHECK(fabs(peak - rrc(0)) < 0.001, "pulse peak %.4f, want %.4f", peak, rrc(0));
}

// Clean symbols straight from the modulator into the demodulator at 8 kHz (no FM, no noise): after the loops settle
// every symbol lands within a few percent of its level
static uint32_t wire_n, wire_bad, wire_seen;
static int wire_lv[4096];
static double wire_worst;

static void wire_push(void *ctx, int16_t soft, int16_t unit)
{
	wire_seen++;
	if (wire_seen == 600) {
		ysf_c4fm_demod_lock(ctx, true);	// as a deframer would once it found a sync
	}
	if (wire_seen > 900 && wire_seen < 2300) {
		double v = (double)soft / unit;
		double near = v > 2 ? 3 : v > 0 ? 1 : v > -2 ? -1 : -3;
		double d = fabs(v - near);
		wire_worst = d > wire_worst ? d : wire_worst;
		wire_bad += d > 0.5;
	}
}

static uint32_t wire_i;

static int wire_pull(void *ctx)
{
	(void)ctx;
	return wire_i < wire_n ? (int)((uint32_t)wire_lv[wire_i++] & 3u) : -1;
}

static void test_wire(void)
{
	wire_n = 2400;
	uint32_t x = 12345;
	for (uint32_t i = 0; i < wire_n; i++) {
		x = x * 1103515245u + 12345u;
		wire_lv[i] = (int)(x >> 16);
	}
	ysf_c4fm_mod_t m;
	ysf_c4fm_demod_t d;
	ysf_c4fm_mod_init(&m, 8000, 4000, wire_pull, NULL);
	ysf_c4fm_demod_init(&d, 8000, wire_push, &d);
	for (;;) {
		int16_t s[80];
		bool more = ysf_c4fm_mod(&m, s, 80);
		ysf_c4fm_demod(&d, s, 80);
		if (!more) {
			break;
		}
	}
	CHECK(wire_bad == 0 && wire_worst < 0.2, "wire: %u symbols off, worst %.3f of a level", wire_bad, wire_worst);
}

// --- the link ---

static void test_link(void)
{
	link_t L = { .ebn0_db = 99, .frames = 200, .seed = 1 };
	result_t r = run_link(&L);
	CHECK(r.firsts == 1 && r.voices == 200 && r.raw_errors + r.prot_errors == 0 && r.ends == 1 && r.callsign_ok,
	      "clean: tx %u voices %u errors %u ends %u call %d", r.firsts, r.voices, r.raw_errors + r.prot_errors, r.ends,
	      r.callsign_ok);

	// The margins below are over what --sweep measured (docs/spec/ysf-modem.md 2.5)
	struct {
		const char *name;
		link_t L;
		double max_raw;
	} cases[] = {
		{ "Eb/N0 14 dB", { .ebn0_db = 14, .frames = 300, .seed = 2 }, 4e-3 },
		{ "offset +1 kHz", { .ebn0_db = 14, .offset_hz = 1000, .frames = 300, .seed = 3 }, 6e-3 },
		{ "offset -1 kHz", { .ebn0_db = 14, .offset_hz = -1000, .frames = 300, .seed = 4 }, 6e-3 },
		{ "drift 500 Hz", { .ebn0_db = 14, .drift_hz = 500, .frames = 300, .seed = 5 }, 6e-3 },
		{ "clock +200 ppm", { .ebn0_db = 14, .ppm = 200, .frames = 300, .seed = 6 }, 4e-3 },
		{ "clock -200 ppm", { .ebn0_db = 14, .ppm = -200, .frames = 300, .seed = 7 }, 4e-3 },
		{ "inverted", { .ebn0_db = 14, .invert = true, .frames = 300, .seed = 8 }, 4e-3 },
		{ "8 kHz modulator", { .ebn0_db = 14, .tx_8k = true, .frames = 300, .seed = 9 }, 6e-3 },
		{ "DMR modem's levels", { .ebn0_db = 14, .dev_scale = 0.72, .frames = 300, .seed = 10 }, 1.5e-2 },
		{ "RX 300 Hz high-pass, restored", { .ebn0_db = 14, .rx_hpf_hz = 300, .rx_lfr_hz = 300, .frames = 300, .seed = 11 }, 2e-2 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		r = run_link(&cases[i].L);
		CHECK(r.firsts == 1 && r.ends == 1 && r.losts == 0 && r.voices == cases[i].L.frames &&
		      raw_ber(&r) <= cases[i].max_raw && r.callsign_ok,
		      "%s: tx %u ends %u losts %u voices %u raw BER %.2e call %d", cases[i].name, r.firsts, r.ends, r.losts,
		      r.voices, raw_ber(&r), r.callsign_ok);
	}
	// A Yaesu radio's start, the 50 ms preamble and one header, and ours, the preamble and two: the over is caught from
	// its header (its voice counted), not joined late. At 10 dB, 40 overs each: 39 with the preamble and one header, 40
	// with two; 9 with one header and no preamble (the demodulator settles in the header and loses it)
	struct {
		const char *name;
		uint16_t preamble;
		uint8_t headers;
	} starts[] = { { "preamble, one header", 240, 1 }, { "preamble, two headers", 240, 2 } };
	for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
		uint32_t caught = 0;
		for (uint32_t k = 0; k < 8; k++) {
			link_t S = { .ebn0_db = 12, .frames = 30, .seed = 40 + k, .preamble = starts[i].preamble,
				     .headers = starts[i].headers };
			r = run_link(&S);
			caught += (uint32_t)(r.firsts == 1 && r.raw_bits == 22u * r.voices && r.voices == 30);
		}
		CHECK(caught == 8, "%s at 12 dB: %u of 8 overs caught from their header", starts[i].name, caught);
	}
	// The clock loop reads the transmitter's error, the DC loop its offset
	link_t P = { .ebn0_db = 20, .ppm = 300, .offset_hz = 600, .frames = 300, .seed = 12 };
	r = run_link(&P);
	CHECK(r.ppm > 150 && r.ppm < 450, "clock read as %d ppm, sent +300", r.ppm);
	printf("clock read as %d ppm (sent +300), offset %d Hz (sent +600)\n", r.ppm, r.offset);
	CHECK(r.offset > 450 && r.offset < 750, "offset read as %d Hz, sent +600", r.offset);
}

// The engine end to end on a symbol path: the transmitter's PHY blocks into the receiver as hard dibits, as a modem
// chip's physical layer would carry them
static void test_phy_blocks(void)
{
	src_t src = { 0, 60, 3 };
	ysf_tx_config_t cfg;
	tx_config(&cfg, &src);
	static ysf_engine_t tx;
	static rx_side_t r;
	memset(&r, 0, sizeof(r));
	r.seed = 3;
	ysf_path_t path = { 0 };
	CHECK(ysf_engine_tx_start(&tx, &path, &cfg), "phy tx start");
	ysf_rx_cb_t cb = { on_frame, on_call, on_voice, NULL, on_end, &r };
	CHECK(ysf_engine_rx_start(&r.eng, &path, &cb), "phy rx start");
	uint8_t raw[YSF_PHY_BLOCK_BYTES];
	uint32_t blocks = 0;
	while (ysf_engine_tx_block(&tx, raw) && blocks < 1000) {
		if (src.sent >= src.limit) {
			ysf_engine_tx_stop(&tx);
		}
		uint8_t d[YSF_PHY_BLOCK_SYMS];
		for (uint32_t i = 0; i < YSF_PHY_BLOCK_SYMS; i++) {
			d[i] = (uint8_t)((raw[i >> 2] >> (6u - 2u * (i & 3u))) & 3u);
		}
		ysf_engine_rx_dibits(&r.eng, d, YSF_PHY_BLOCK_SYMS);
		blocks++;
	}
	// 1 header, 12 V/D 2 frames, the terminator: 14 x 480 symbols in 144-symbol blocks
	CHECK(blocks == (14u * YSF_FRAME_DIBITS + YSF_PHY_BLOCK_SYMS - 1u) / YSF_PHY_BLOCK_SYMS, "phy: %u blocks", blocks);
	CHECK(r.res.voices == 60 && r.res.raw_errors + r.res.prot_errors == 0 && r.res.ends == 1 && r.res.callsign_ok,
	      "phy: voices %u errors %u ends %u", r.res.voices, r.res.raw_errors + r.res.prot_errors, r.res.ends);
}

// The engine on an 8 kHz audio path, as a board's flat taps carry it: the transmitter's samples straight into the
// receiver
static void test_engine_audio(void)
{
	src_t src = { 0, 40, 4 };
	ysf_tx_config_t cfg;
	tx_config(&cfg, &src);
	static ysf_engine_t tx;
	static rx_side_t r;
	memset(&r, 0, sizeof(r));
	r.seed = 4;
	ysf_path_t path = { .rate = 8000, .tx_amp = 3000 };
	CHECK(ysf_engine_tx_start(&tx, &path, &cfg), "audio tx start");
	ysf_rx_cb_t cb = { on_frame, on_call, on_voice, NULL, on_end, &r };
	CHECK(ysf_engine_rx_start(&r.eng, &path, &cb), "audio rx start");
	int16_t pcm[256];
	uint32_t blocks = 0;
	while (ysf_engine_tx(&tx, pcm, 256) && blocks < 2000) {
		if (src.sent >= src.limit) {
			ysf_engine_tx_stop(&tx);
		}
		ysf_engine_rx(&r.eng, pcm, 256);
		blocks++;
	}
	memset(pcm, 0, sizeof(pcm));
	for (int i = 0; i < 10; i++) {
		ysf_engine_rx(&r.eng, pcm, 256);
	}
	CHECK(r.res.voices == 40 && r.res.raw_errors + r.res.prot_errors == 0 && r.res.ends == 1,
	      "audio: voices %u errors %u ends %u", r.res.voices, r.res.raw_errors + r.res.prot_errors, r.res.ends);
}

static void sweep(void)
{
	static const double ebn0[] = { 6, 8, 10, 12, 14, 16, 18 };
	struct {
		const char *name;
		link_t L;
	} rows[] = {
		{ "Clean", { .ebn0_db = 0 } },
		{ "+1 kHz carrier offset", { .offset_hz = 1000 } },
		{ "500 Hz drift over the over", { .drift_hz = 500 } },
		{ "+200 ppm symbol clock", { .ppm = 200 } },
		{ "Modulator at 8 kHz", { .tx_8k = true } },
		{ "DMR modem's levels (0.72)", { .dev_scale = 0.72 } },
		{ "Narrow deviation (0.5)", { .dev_scale = 0.5 } },
		{ "RX 30 Hz high-pass", { .rx_hpf_hz = 30 } },
		{ "RX 100 Hz high-pass", { .rx_hpf_hz = 100 } },
		{ "RX 300 Hz high-pass, restored", { .rx_hpf_hz = 300, .rx_lfr_hz = 300 } },
		{ "16 kHz channel filter", { .chan_hz = 8000 } },
	};
	printf("BER over the voice frames of transmissions caught from their header (4 x 250 voice frames a point):\n");
	printf("raw = the 22 bits sent once, voice = all 49 after the vote; (h/4) headers caught when under 4\n\n");
	printf("| Path |");
	for (size_t j = 0; j < sizeof(ebn0) / sizeof(ebn0[0]); j++) {
		printf(" %g dB |", ebn0[j]);
	}
	printf("\n|---|");
	for (size_t j = 0; j < sizeof(ebn0) / sizeof(ebn0[0]); j++) {
		printf("---|");
	}
	printf("\n");
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		printf("| %s |", rows[i].name);
		for (size_t j = 0; j < sizeof(ebn0) / sizeof(ebn0[0]); j++) {
			result_t sum = { 0 };
			uint32_t caught = 0;
			for (uint32_t k = 0; k < 4; k++) {
				link_t L = rows[i].L;
				L.ebn0_db = ebn0[j];
				L.frames = 250;
				L.seed = 100 + 10 * (uint32_t)j + k;
				result_t r = run_link(&L);
				caught += r.counting || r.raw_bits ? 1u : 0u;
				sum.raw_errors += r.raw_errors;
				sum.raw_bits += r.raw_bits;
				sum.prot_errors += r.prot_errors;
				sum.prot_bits += r.prot_bits;
			}
			if (sum.raw_bits == 0) {
				printf(" (0/4) |");
			} else {
				printf(" %.1e / %.1e", raw_ber(&sum), voice_ber(&sum));
				if (caught < 4) {
					printf(" (%u/4)", caught);
				}
				printf(" |");
			}
			fflush(stdout);
		}
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--sweep") == 0) {
		sweep();
		return 0;
	}
	test_mod_shape();
	test_wire();
	test_link();
	test_phy_blocks();
	test_engine_audio();
	printf("%s ysfModemTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}

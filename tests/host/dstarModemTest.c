// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for the D-STAR GMSK modem (core/dstar/dstar_gmsk.c, docs/spec/dstar-modem.md 2) on a simulated radio
 * link: the transmitter's bit stream (dstar_frame.c) through the modulator, an FM modulator at 48 kHz, white noise at
 * a given Eb/N0, a 12.5 kHz channel filter, an FM discriminator, the receiver's audio path (a 3.4 kHz low-pass and 8
 * kHz samples, as a radio's PCM path delivers them), the demodulator and the deframer. Voice frames carry known
 * pseudo-random bits, so the voice bits the deframer hands back give the bit error rate.
 *
 * Impairments: carrier offset, the transmitter's bit clock off by some ppm, an inverted discriminator, a first-order
 * high-pass in the transmit or receive audio path (with and without the low-frequency restorer), the modulator run
 * at 8 kHz as on the radio. The assertions are margins under what the sweep measured; --sweep prints the BER table
 * (docs/spec/dstar-modem.md 2.6).
 *
 *   make -C tests/host run-dstar          dstarModemTest --sweep    the full table (a minute)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dstar/dstar_gmsk.h"
#include "dstar/dstar_frame.h"
#include "dstar/dstar_engine.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FS		48000.0	// the simulated air
#define DECIM		6	// to the receiver's 8 kHz
#define RX_RATE		8000u
#define DISC_SCALE	8000.0	// discriminator samples per 1200 Hz (a radio's RX path gives some such level)

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

// --- the link ---

typedef struct {
	double ebn0_db;		// 99 = no noise
	double offset_hz;
	int32_t ppm;		// the transmitter's bit clock error
	bool invert;
	double tx_hpf_hz;	// a first-order high-pass in the transmit audio path, 0 = none
	double rx_hpf_hz;	// and in the receive audio path
	uint16_t tx_lfr_hz;	// the modulator's restorer for it
	uint16_t rx_lfr_hz;	// the demodulator's
	uint16_t lfr_floor_hz;	// the restorers' floor (0: 60 Hz)
	bool tx_8k;		// run the modulator at 8 kHz and interpolate, as the radio does
	uint32_t frames;	// voice frames to send
	uint32_t seed;
} link_t;

typedef struct {
	uint32_t headers, slow_headers, voices, bit_errors, bits, ends, losts, relocks;
	uint32_t next;		// the next voice frame expected
	int32_t ppm;
	bool counting;
} result_t;

static uint8_t voice_byte(uint32_t frame, uint32_t i, uint32_t seed)
{
	uint32_t x = (frame * 9u + i) * 2654435761u ^ seed * 0x9E3779B9u;
	x ^= x >> 15;
	x *= 0x85EBCA6Bu;
	x ^= x >> 13;
	return (uint8_t)x;
}

typedef struct {
	uint32_t sent, limit, seed;
} src_t;

static bool pull_voice(void *ctx, uint8_t v[DSTAR_VOICE_BYTES])
{
	src_t *s = ctx;
	if (s->sent >= s->limit) {
		return false;
	}
	for (uint32_t i = 0; i < DSTAR_VOICE_BYTES; i++) {
		v[i] = voice_byte(s->sent, i, s->seed);
	}
	s->sent++;
	return true;
}

typedef struct {
	dstar_rx_t rx;
	dstar_gmsk_demod_t dm;
	result_t res;
	uint32_t seed;
} rx_side_t;

static void on_header(void *ctx, const dstar_header_t *h, bool from_slow)
{
	rx_side_t *r = ctx;
	if (from_slow) {
		r->res.slow_headers++;
		return;
	}
	r->res.headers++;
	r->res.counting = true;
	r->res.next = 0;
}

static void on_voice(void *ctx, const uint8_t v[DSTAR_VOICE_BYTES], uint8_t frame)
{
	rx_side_t *r = ctx;
	r->res.voices++;
	if (!r->res.counting) {
		return;
	}
	for (uint32_t i = 0; i < DSTAR_VOICE_BYTES; i++) {
		r->res.bit_errors += (uint32_t)__builtin_popcount((unsigned)(v[i] ^ voice_byte(r->res.next, i, r->seed)));
	}
	r->res.bits += DSTAR_VOICE_BITS;
	r->res.next++;
}

static void on_end(void *ctx, dstar_end_t why)
{
	rx_side_t *r = ctx;
	r->res.ppm = dstar_gmsk_demod_ppm(&r->dm);
	if (why == DSTAR_END_PATTERN) {
		r->res.ends++;
	} else {
		r->res.losts++;
	}
	// After a lost lock, frames that come by late entry can't be lined up with what was sent: stop counting
	r->res.counting = false;
}

static void push_bit(void *ctx, int16_t soft, int16_t unit)
{
	rx_side_t *r = ctx;
	bool was = dstar_rx_locked(&r->rx);
	dstar_rx_bit(&r->rx, soft, unit);
	bool now = dstar_rx_locked(&r->rx);
	if (now != was) {
		dstar_gmsk_demod_lock(&r->dm, now);
		if (now && r->rx.state == DSTAR_RX_VOICE && !r->res.counting && r->res.voices) {
			r->res.relocks++;
		}
	}
}

static int tx_bit(void *ctx)
{
	return dstar_tx_bit(ctx);
}

static result_t run_link(const link_t *L)
{
	rng_s = 0x1234567ull + L->seed * 7919ull;
	dstar_header_t h;
	dstar_header_set(&h, 0, "DIRECT", "DIRECT", "CQCQCQ", "N0CALL", "TEST");
	src_t src = { 0, L->frames, L->seed };
	static dstar_tx_t tx;
	dstar_tx_init(&tx, &h, "Modem test", 240, pull_voice, &src);

	// The modulator's full deviation, with headroom for a restorer's low-frequency gain (a radio's flat path has far
	// more: for APRS 2750 counts gave 3.1 kHz, and it reached 5.1 kHz)
	const int16_t amp = 4000;
	dstar_gmsk_mod_t mod;
	uint32_t mrate = L->tx_8k ? 8000u : (uint32_t)FS;
	dstar_gmsk_mod_init(&mod, mrate, amp, tx_bit, &tx);
	dstar_gmsk_mod_trim(&mod, L->ppm);
	if (L->tx_lfr_hz) {
		dstar_lfr_init(&mod.lfr, mrate, L->tx_lfr_hz, L->lfr_floor_hz ? L->lfr_floor_hz : 60);
	}

	static rx_side_t r;
	memset(&r, 0, sizeof(r));
	r.seed = L->seed;
	dstar_rx_cb_t cb = { on_header, on_voice, NULL, on_end, &r };
	dstar_rx_init(&r.rx, &cb);
	dstar_gmsk_demod_init(&r.dm, RX_RATE, push_bit, &r);
	r.dm.invert = false;
	if (L->rx_lfr_hz) {
		dstar_lfr_init(&r.dm.lfr, RX_RATE, L->rx_lfr_hz, L->lfr_floor_hz ? L->lfr_floor_hz : 60);
	}

	// The transmitter's audio at 48 kHz, whole: straight from the modulator, or from it at 8 kHz held for six
	// samples and smoothed (the radio's PCM through its DAC's interpolation)
	size_t cap = (size_t)(L->frames + 40u) * DSTAR_FRAME_BITS * (size_t)(FS / DSTAR_BAUD) + 4096u, ntx = 0;
	double *txa = malloc(cap * sizeof(double));
	static fir_t chan, audio, interp;
	fir_init(&chan, 65, 6000, FS);
	fir_init(&audio, 97, 3400, FS);
	fir_init(&interp, 97, 3600, FS);
	for (;;) {
		int16_t m[64];
		size_t got = dstar_gmsk_mod(&mod, m, 64);
		for (size_t i = 0; i < got; i++) {
			if (L->tx_8k) {
				for (int j = 0; j < DECIM && ntx < cap; j++) {
					double re = m[i], im = 0;
					fir_c(&interp, &re, &im);
					txa[ntx++] = re / amp;
				}
			} else if (ntx < cap) {
				txa[ntx++] = (double)m[i] / amp;
			}
		}
		if (got < 64) {
			break;
		}
	}

	// Noise: signal power 1, Eb = 1 / 4800 s; complex noise variance per sample N0 x fs
	double sigma = L->ebn0_db >= 99 ? 0 : sqrt(FS / (DSTAR_BAUD * pow(10, L->ebn0_db / 10)) / 2);
	double phase = 0, pre = 1, pim = 0, hp_tx_x = 0, hp_tx_y = 0, hp_rx_x = 0, hp_rx_y = 0;
	double a_tx = L->tx_hpf_hz > 0 ? 1 / (1 + 2 * M_PI * L->tx_hpf_hz / FS) : 1;
	double a_rx = L->rx_hpf_hz > 0 ? 1 / (1 + 2 * M_PI * L->rx_hpf_hz / FS) : 1;
	int16_t out8[64];
	size_t n8 = 0;
	// Noise before the carrier (100 ms) and after it (200 ms), as a receiver hears
	size_t lead = (size_t)(FS / 10), total = lead + ntx + (size_t)(FS / 5);
	for (size_t k = 0; k < total; k++) {
		bool on = k >= lead && k < lead + ntx;
		double f = on ? txa[k - lead] : 0;
		if (L->tx_hpf_hz > 0) {
			hp_tx_y = a_tx * (hp_tx_y + f - hp_tx_x);
			hp_tx_x = f;
			f = hp_tx_y;
		}
		phase += 2 * M_PI * (DSTAR_DEV_HZ * f + L->offset_hz) / FS;
		double carrier = on ? 1 : 0;
		double re = carrier * cos(phase) + sigma * gauss(), im = carrier * sin(phase) + sigma * gauss();
		if (!on && sigma == 0) {
			re = 1e-9 * gauss();	// a receiver with no signal and no noise still hears something
			im = 1e-9 * gauss();
		}
		fir_c(&chan, &re, &im);
		double dre = re * pre + im * pim, dim = im * pre - re * pim;
		pre = re;
		pim = im;
		double disc = atan2(dim, dre) * FS / (2 * M_PI) / DSTAR_DEV_HZ;	// 1.0 = 1200 Hz
		if (L->invert) {
			disc = -disc;
		}
		if (L->rx_hpf_hz > 0) {
			hp_rx_y = a_rx * (hp_rx_y + disc - hp_rx_x);
			hp_rx_x = disc;
			disc = hp_rx_y;
		}
		double ar = disc, ai = 0;
		fir_c(&audio, &ar, &ai);
		if (k % DECIM == 0) {
			double v = ar * DISC_SCALE;
			out8[n8++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			if (n8 == sizeof(out8) / sizeof(out8[0])) {
				dstar_gmsk_demod(&r.dm, out8, n8);
				n8 = 0;
			}
		}
	}
	free(txa);
	dstar_gmsk_demod(&r.dm, out8, n8);
	return r.res;
}

static double ber(const result_t *r)
{
	return r->bits ? (double)r->bit_errors / r->bits : 1.0;
}

static uint8_t iso_bits[32];
static uint32_t iso_n, iso_i;

static int iso_pull(void *ctx)
{
	return iso_i < iso_n ? (int)iso_bits[iso_i++] : -1;
}

// The modulator's output (and so its pulse table) against the definition, libm's erf: every sample of a pattern at
// 96 kHz equals the sum of the Gaussian-filtered rectangles of its bits, within the table's resolution
static void test_mod_shape(void)
{
	static const uint8_t pat[] = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0 };
	iso_n = sizeof(pat);
	iso_i = 0;
	memcpy(iso_bits, pat, sizeof(pat));
	dstar_gmsk_mod_t m;
	const uint32_t rate = 96000;
	CHECK(dstar_gmsk_mod_init(&m, rate, 30000, iso_pull, NULL), "init");
	int16_t out[600];
	size_t n = dstar_gmsk_mod(&m, out, 600);
	CHECK(n > 24u * rate / DSTAR_BAUD && n < 600, "length %zu", n);
	double sigma = sqrt(log(2.0)) / (2 * M_PI * 0.5);
	double worst = 0;
	for (size_t i = 0; i < 24u * rate / DSTAR_BAUD; i++) {
		double t = (double)i * DSTAR_BAUD / rate;	// in bits from the start of bit 0
		double want = 0;
		for (int b = 0; b < (int)sizeof(pat); b++) {
			double tau = t - (b + 0.5);
			double g = 0.5 * (erf((tau + 0.5) / (sqrt(2) * sigma)) - erf((tau - 0.5) / (sqrt(2) * sigma)));
			want += (pat[b] ? 1 : -1) * g;
		}
		double d = fabs(out[i] / 30000.0 - want);
		worst = d > worst ? d : worst;
	}
	// 64 table points a bit, interpolated: within 0.1 % of full deviation, plus the step's rounding
	CHECK(worst < 0.002, "modulator differs from the Gaussian pulse by %.4f", worst);
	CHECK(m.bits == sizeof(pat), "bits %u", m.bits);
}

// The restorer: a step through a first-order high-pass at 300 Hz and the restorer for it (floor 20 Hz) comes out as
// the floor's own high-pass would leave it, 10000 e^(-2 pi 20 t): the 300 Hz droop is gone
static void test_lfr(void)
{
	dstar_lfr_t f;
	dstar_lfr_init(&f, 8000, 300, 20);
	double a = 1 / (1 + 2 * M_PI * 300 / 8000.0), x0 = 0, y = 0, worst = 0, plain = 0;
	for (int i = 0; i < 400; i++) {	// 50 ms of a constant level
		double x = 10000;
		y = a * (y + x - x0);
		x0 = x;
		int16_t r = dstar_lfr(&f, (int32_t)lrint(y));
		double want = 10000 * exp(-2 * M_PI * 20 * i / 8000.0);
		double d = fabs(r - want);
		worst = d > worst ? d : worst;
		if (i == 40) {
			plain = y;	// 5 ms in, unrestored
		}
	}
	CHECK(worst < 300, "restored step off by %.0f", worst);
	CHECK(plain < 1000, "the high-pass alone left %.0f at 5 ms", plain);
	dstar_lfr_init(&f, 8000, 0, 0);
	CHECK(dstar_lfr(&f, 1234) == 1234, "off is a wire");
}

static void test_link(void)
{
	// Clean: everything through
	link_t L = { .ebn0_db = 99, .frames = 200, .seed = 1 };
	result_t r = run_link(&L);
	CHECK(r.headers == 1 && r.voices == 200 && r.bit_errors == 0 && r.ends == 1, "clean: h %u v %u e %u ends %u",
	      r.headers, r.voices, r.bit_errors, r.ends);

	// The margins below are under what --sweep measured (docs/spec/dstar-modem.md 2.6)
	struct {
		const char *name;
		link_t L;
		double max_ber;
	} cases[] = {
		{ "Eb/N0 14 dB", { .ebn0_db = 14, .frames = 300, .seed = 2 }, 2e-3 },
		{ "offset +700 Hz", { .ebn0_db = 14, .offset_hz = 700, .frames = 300, .seed = 3 }, 3e-3 },
		{ "offset -700 Hz", { .ebn0_db = 14, .offset_hz = -700, .frames = 300, .seed = 4 }, 3e-3 },
		{ "clock +300 ppm", { .ebn0_db = 14, .ppm = 300, .frames = 300, .seed = 5 }, 2e-3 },
		{ "clock -300 ppm", { .ebn0_db = 14, .ppm = -300, .frames = 300, .seed = 6 }, 2e-3 },
		{ "inverted", { .ebn0_db = 14, .invert = true, .frames = 300, .seed = 7 }, 2e-3 },
		{ "8 kHz modulator", { .ebn0_db = 14, .tx_8k = true, .frames = 300, .seed = 8 }, 3e-3 },
		{ "TX 290 Hz high-pass, restored", { .ebn0_db = 14, .tx_hpf_hz = 290, .tx_lfr_hz = 290, .frames = 300, .seed = 9 }, 3e-3 },
		{ "RX 300 Hz high-pass, restored", { .ebn0_db = 14, .rx_hpf_hz = 300, .rx_lfr_hz = 300, .frames = 300, .seed = 10 }, 3e-3 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		r = run_link(&cases[i].L);
		CHECK(r.headers == 1 && r.ends == 1 && r.losts == 0 && r.voices == cases[i].L.frames && ber(&r) <= cases[i].max_ber,
		      "%s: headers %u ends %u losts %u voices %u BER %.2e", cases[i].name, r.headers, r.ends, r.losts, r.voices,
		      ber(&r));
	}
	// The clock loop reads the transmitter's error
	link_t P = { .ebn0_db = 20, .ppm = 400, .frames = 300, .seed = 11 };
	r = run_link(&P);
	CHECK(r.ppm > 300 && r.ppm < 500, "clock read %d ppm for 400", r.ppm);
}

// The engine end to end at 8 kHz, as a board would run it: both paths inverted (they cancel), a first-order high-pass
// in each path (290 Hz out, 300 Hz in) with the restorer for it, no voice source (the silence frame until stopped)
typedef struct {
	int headers, voices, ends;
} eng_log_t;

static void eng_header(void *ctx, const dstar_header_t *h, bool from_slow)
{
	eng_log_t *l = ctx;
	l->headers += !from_slow && memcmp(h->my, "N0CALL  ", 8) == 0;
}

static void eng_voice(void *ctx, const uint8_t v[DSTAR_VOICE_BYTES], uint8_t frame)
{
	eng_log_t *l = ctx;
	l->voices += memcmp(v, dstar_voice_silence, DSTAR_VOICE_BYTES) == 0;
}

static void eng_end(void *ctx, dstar_end_t why)
{
	eng_log_t *l = ctx;
	l->ends += why == DSTAR_END_PATTERN;
}

static void test_engine(void)
{
	static dstar_engine_t tx, rx;
	dstar_path_t path = { .rate = 8000, .tx_amp = 1100, .tx_invert = true, .rx_invert = true, .tx_hpf_hz = 290,
			      .rx_hpf_hz = 300 };
	dstar_header_t h;
	dstar_header_set(&h, 0, "DIRECT", "DIRECT", "CQCQCQ", "N0CALL", "");
	eng_log_t l = { 0 };
	dstar_rx_cb_t cb = { eng_header, eng_voice, NULL, eng_end, &l };
	CHECK(dstar_engine_tx_start(&tx, &path, &h, NULL, 480, NULL, NULL), "tx start");
	CHECK(dstar_engine_rx_start(&rx, &path, &cb), "rx start");
	int16_t pcm[256];
	uint32_t blocks = 0;
	bool more = true;
	double a1 = 1 / (1 + 2 * M_PI * 290 / 8000.0), a2 = 1 / (1 + 2 * M_PI * 300 / 8000.0);
	double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	while (more && blocks < 1000) {
		more = dstar_engine_tx(&tx, pcm, 256);
		for (int i = 0; i < 256; i++) {
			y1 = a1 * (y1 + pcm[i] - x1);
			x1 = pcm[i];
			y2 = a2 * (y2 + y1 - x2);
			x2 = y1;
			pcm[i] = (int16_t)lrint(y2 * 4);	// and some gain on the way in
		}
		// 32 ms a block (the radio's RX RAM halves): stop after about two seconds
		if (++blocks == 60) {
			dstar_engine_tx_stop(&tx);
		}
		dstar_engine_rx(&rx, pcm, 256);
	}
	CHECK(!more && blocks < 100, "transmission ended after %u blocks", blocks);
	// 60 blocks of 256 samples are 1.92 s: about 96 frames less the preamble and header (0.24 s)
	CHECK(l.headers == 1 && l.voices >= 80 && l.voices <= 100 && l.ends == 1, "engine: headers %d voices %d ends %d",
	      l.headers, l.voices, l.ends);
	printf("# dstar engine state %zu bytes (rx %zu + %zu, tx %zu + %zu, shared)\n", sizeof(dstar_engine_t),
	       sizeof(dstar_gmsk_demod_t), sizeof(dstar_rx_t), sizeof(dstar_gmsk_mod_t), sizeof(dstar_tx_t));
}

static void sweep(void)
{
	static const double snr[] = { 6, 8, 9, 10, 11, 12, 13, 14, 16, 18 };
	struct {
		const char *name;
		link_t L;
	} rows[] = {
		{ "clean path", { .ebn0_db = 0 } },
		{ "+700 Hz offset", { .offset_hz = 700 } },
		{ "+500 ppm clock", { .ppm = 500 } },
		{ "8 kHz modulator", { .tx_8k = true } },
		{ "TX HPF 290 Hz", { .tx_hpf_hz = 290 } },
		{ "TX HPF 290 Hz + restorer", { .tx_hpf_hz = 290, .tx_lfr_hz = 290 } },
		{ "RX HPF 300 Hz", { .rx_hpf_hz = 300 } },
		{ "RX HPF 300 Hz + restorer", { .rx_hpf_hz = 300, .rx_lfr_hz = 300 } },
	};
	printf("| Path | ");
	for (size_t j = 0; j < sizeof(snr) / sizeof(snr[0]); j++) {
		printf("%.0f dB | ", snr[j]);
	}
	printf("\n");
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		printf("| %s | ", rows[i].name);
		for (size_t j = 0; j < sizeof(snr) / sizeof(snr[0]); j++) {
			result_t tot = { 0 };
			uint32_t hdr = 0, runs = 4;
			for (uint32_t s = 0; s < runs; s++) {
				link_t L = rows[i].L;
				L.ebn0_db = snr[j];
				L.frames = 500;
				L.seed = 100 + s;
				result_t r = run_link(&L);
				tot.bits += r.bits;
				tot.bit_errors += r.bit_errors;
				hdr += r.headers;
			}
			if (tot.bits) {
				printf("%.1e (%u/%u) | ", ber(&tot), hdr, runs);
			} else {
				printf("- (%u/%u) | ", hdr, runs);
			}
			fflush(stdout);
		}
		printf("\n");
	}
}

// A demodulator whose init refused the rate is fed: nothing comes out, and nothing divides by its boxcar of 0 samples
static unsigned guard_bits;
static void guard_push(void *ctx, int16_t soft, int16_t unit)
{
	(void)ctx;
	(void)soft;
	(void)unit;
	guard_bits++;
}

static void test_demod_guard(void)
{
	dstar_gmsk_demod_t d;
	CHECK(!dstar_gmsk_demod_init(&d, DSTAR_RATE_MIN / 2u, guard_push, NULL), "4 kHz accepted");
	d.push = guard_push;
	int16_t s[256];
	for (int i = 0; i < 256; i++) {
		s[i] = (int16_t)((i * 7919) % 6001 - 3000);
	}
	guard_bits = 0;
	dstar_gmsk_demod(&d, s, 256);
	CHECK(guard_bits == 0, "%u bits from a demodulator never started", guard_bits);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--sweep") == 0) {
		sweep();
		return 0;
	}
	test_mod_shape();
	test_lfr();
	test_link();
	test_engine();
	test_demod_guard();
	printf("%s dstarModemTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}

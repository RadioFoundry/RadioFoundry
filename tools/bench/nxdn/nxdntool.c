// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN bench tool on the firmware's own code: the frame codec and call layer of core/nxdn, the framer of core/air and
// the soft modem's receiver, built for the host. tools/bench/nxdntool.py is its front end (options, comparisons, the
// self-test); this program writes JSON lines on stdout. Voice is opaque: 72-bit VCH frames as on air, never speech.
//
//   nxdntool gen    OUT.dibits EXPECT.json [options]	a conventional voice transmission as the transmit path builds it
//   nxdntool decode IN.dibits [options]			symbols through the framer and the call layer
//   nxdntool disc   IN.raw [options]			discriminator samples through the soft modem, then as decode
//   nxdntool mod    IN.dibits OUT.raw [options]		a 4-level FSK model: symbols to discriminator samples (for tests)
//
// --9600: the 9600 bit/s mode (4800 symbols/s), else 4800 bit/s (2400 symbols/s).
// Dibit files: four symbols to a byte, the first in bits 7-6; 01 = +3, 00 = +1, 10 = -1, 11 = -3 (hal/hal_air.h).
// --unpacked: one symbol a byte, in its low two bits.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "nxdn/nxdn.h"

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "nxdntool: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
	exit(2);
}

// ── Small helpers ──
static uint64_t rng_s = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void)
{
	rng_s ^= rng_s << 13;
	rng_s ^= rng_s >> 7;
	rng_s ^= rng_s << 17;
	return rng_s;
}

static void rng_seed(uint64_t s)
{
	rng_s = s ? s * 0x9E3779B97F4A7C15ull : 0x9E3779B97F4A7C15ull;
	for (int i = 0; i < 8; i++)
		rng();
}

static double rng_unit(void)
{
	return (double)(rng() >> 11) / 9007199254740992.0;	// [0, 1)
}

static double gauss(void)
{
	double u = 1.0 - rng_unit(), v = rng_unit();

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void parse_hex(const char *s, uint8_t *out, size_t n, const char *what)
{
	if (strlen(s) != 2 * n)
		die("wrong length of hex", what);
	for (size_t i = 0; i < n; i++) {
		int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);

		if (a < 0 || b < 0)
			die("not hex", what);
		out[i] = (uint8_t)(a << 4 | b);
	}
}

static void put_hex(FILE *f, const uint8_t *p, size_t n)
{
	fputc('"', f);
	for (size_t i = 0; i < n; i++)
		fprintf(f, "%02x", p[i]);
	fputc('"', f);
}

static uint8_t *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p = NULL;
	size_t cap = 0, len = 0;

	if (!f)
		die("can't open", path);
	for (;;) {
		if (len == cap) {
			cap = cap ? 2 * cap : 1 << 16;
			p = realloc(p, cap);
			if (!p)
				die("out of memory", NULL);
		}
		size_t r = fread(p + len, 1, cap - len, f);

		if (!r)
			break;
		len += r;
	}
	fclose(f);
	*n = len;
	return p;
}

static uint8_t *read_syms(const char *path, int unpacked, size_t *nsyms)
{
	size_t n;
	uint8_t *raw = slurp(path, &n), *s;

	if (unpacked) {
		for (size_t i = 0; i < n; i++)
			raw[i] &= 3;
		*nsyms = n;
		return raw;
	}
	s = malloc(4 * n + 1);
	if (!s)
		die("out of memory", NULL);
	for (size_t i = 0; i < 4 * n; i++)
		s[i] = (uint8_t)((raw[i / 4] >> (6 - 2 * (i & 3))) & 3);
	free(raw);
	*nsyms = 4 * n;
	return s;
}

static void write_syms(const char *path, const uint8_t *s, size_t n, int unpacked, uint8_t idle)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		die("can't write", path);
	if (unpacked) {
		fwrite(s, 1, n, f);
	} else {
		for (size_t i = 0; i < n; i += 4) {
			uint8_t b = 0;

			for (size_t k = 0; k < 4; k++)
				b = (uint8_t)(b << 2 | (i + k < n ? s[i + k] : (idle >> (6 - 2 * k)) & 3));
			fputc(b, f);
		}
	}
	fclose(f);
}

typedef struct {
	int argc;
	char **argv;
} opts_t;

static const char *opt(const opts_t *o, const char *name)
{
	for (int i = 0; i < o->argc; i++)
		if (!strcmp(o->argv[i], name))
			return i + 1 < o->argc ? o->argv[i + 1] : "";
	return NULL;
}

static int flag(const opts_t *o, const char *name)
{
	for (int i = 0; i < o->argc; i++)
		if (!strcmp(o->argv[i], name))
			return 1;
	return 0;
}

static long opt_long(const opts_t *o, const char *name, long dflt)
{
	const char *v = opt(o, name);
	char *end;
	long r;

	if (!v)
		return dflt;
	r = strtol(v, &end, 10);
	if (!*v || *end)
		die("bad number for", name);
	return r;
}

static double opt_double(const opts_t *o, const char *name, double dflt)
{
	const char *v = opt(o, name);
	char *end;
	double r;

	if (!v)
		return dflt;
	r = strtod(v, &end);
	if (!*v || *end)
		die("bad number for", name);
	return r;
}

static const air_mode_t *mode_of(const opts_t *o)
{
	return flag(o, "--9600") ? &nxdn_air_mode_9600 : &nxdn_air_mode;
}

static void vcall_json(FILE *f, const nxdn_vcall_t *v)
{
	fprintf(f, "{\"msg\":%u,\"cc_opt\":%u,\"call_type\":%u,\"voice_opt\":%u,\"src\":%u,\"dst\":%u,\"cipher\":%u,\"key_id\":%u}",
		v->msg, v->cc_opt, v->call_type, v->voice_opt, v->src, v->dst, v->cipher, v->key_id);
}

// The VCH of a frame that carry voice, by its LICH's steal option
static bool vch_voice(uint8_t lich, uint8_t n)
{
	uint8_t o = NXDN_LICH_OPT(lich);

	if (NXDN_LICH_USC(lich) == NXDN_USC_UDCH)
		return false;
	return o == NXDN_STEAL_NONE || (o == NXDN_STEAL_FIRST && n >= 2) || (o == NXDN_STEAL_SECOND && n < 2);
}

// ── gen: the transmit path of nxdn_call.c, frame by frame ──
#define GEN_MAX_SYMS	(1u << 22)

static int cmd_gen(const char *out_path, const char *exp_path, const opts_t *o)
{
	static nxdn_call_tx_t t;
	static uint8_t fixed[NXDN_EHR_BYTES] = { 0xA5, 0xC3, 0xF0, 0x0F, 0x5A, 0x3C, 0x96, 0x69, 0x0F };
	const air_mode_t *m = mode_of(o);
	bool r9600 = m == &nxdn_air_mode_9600;
	long frames = opt_long(o, "--voice-frames", 48), lead = opt_long(o, "--lead", 0), tail = opt_long(o, "--tail", 0);
	long ran = opt_long(o, "--ran", 1), seed_voice = opt_long(o, "--random-voice", -1);
	double ber = opt_double(o, "--ber", 0);
	uint8_t idle = m->profile.idle, frame[NXDN_EHR_BYTES];
	nxdn_vcall_t v = { NXDN_MSG_VCALL, 0, NXDN_CALL_GROUP, r9600 ? NXDN_VOICE_9600_EHR : NXDN_VOICE_4800_EHR, 1, 1,
			   NXDN_CIPHER_NONE, 0 };
	uint8_t *syms = malloc(GEN_MAX_SYMS);
	size_t n = 0, frame_at = 0;
	long given = 0, nflip = 0, nframes = 0;
	FILE *e = fopen(exp_path, "w"), *vf = NULL;
	int ended = 0;

	if (!syms || !e)
		die("can't write", exp_path);
	if (ran < 0 || ran > 63 || frames < 0 || frames > 100000 || lead < 0 || tail < 0 || ber < 0 || ber > 0.5)
		die("an option is out of range", NULL);
	v.src = (uint16_t)opt_long(o, "--src", 1);
	if (opt(o, "--unit")) {
		v.call_type = NXDN_CALL_INDIVIDUAL;
		v.dst = (uint16_t)opt_long(o, "--unit", 1);
	} else {
		v.dst = (uint16_t)opt_long(o, "--group", 1);
	}
	if (flag(o, "--emergency"))
		v.cc_opt = NXDN_CC_EMERGENCY;
	if (opt(o, "--voice"))
		parse_hex(opt(o, "--voice"), fixed, sizeof fixed, "--voice");
	if (seed_voice >= 0)
		rng_seed((uint64_t)seed_voice);
	// --voice-file: 72-bit frames as on air, 9 bytes each, in order (still opaque here); the last one repeats
	if (opt(o, "--voice-file")) {
		vf = fopen(opt(o, "--voice-file"), "rb");
		if (!vf)
			die("can't read", opt(o, "--voice-file"));
	}

	fprintf(e, "{\"format\":\"nxdn-expect/1\",\"rate\":%u,\"ran\":%ld,\"vcall\":", r9600 ? 9600 : 4800, ran);
	vcall_json(e, &v);
	fprintf(e, ",\"lead_syms\":%ld,\"frames\":[", lead);
	for (long i = 0; i < lead; i++)
		syms[n++] = (uint8_t)((idle >> (6 - 2 * (i & 3))) & 3);
	nxdn_call_tx_begin(&t, &v, (uint8_t)ran, NXDN_DIR_INBOUND, r9600);
	while (!nxdn_call_tx_done(&t)) {
		uint16_t first, k;

		// The last voice frame given, the key lets go at once
		if (given == frames && !ended) {
			nxdn_call_tx_end(&t, fixed);
			ended = 1;
		}
		k = nxdn_call_tx_ready(&t, &first);
		if (k) {
			if (n + k > GEN_MAX_SYMS)
				die("too long", NULL);
			if (t.sent == 0)
				frame_at = n;
			if (t.sent + k == t.nsyms && t.nsyms == NXDN_FRAME_SYMS) {
				// A frame is whole (it goes out in parts as its voice comes): what the checker needs from it
				int lich = nxdn_lich_get(t.frame, NULL);

				fprintf(e, "%s{\"at\":%zu,\"lich\":%d,\"voice\":[", nframes ? "," : "", frame_at, lich);
				for (uint8_t vn = 0, c = 0; vn < NXDN_VOICE_FRAMES; vn++)
					if (lich >= 0 && vch_voice((uint8_t)lich, vn)) {
						nxdn_vch_get(t.frame, vn, frame);
						fputs(c++ ? "," : "", e);
						put_hex(e, frame, sizeof frame);
					}
				fputs("],\"syms\":", e);
				put_hex(e, t.frame, NXDN_FRAME_BYTES);
				fputc('}', e);
				nframes++;
			}
			for (uint16_t i = 0; i < k; i++)
				syms[n++] = air_sym_get(nxdn_call_tx_syms(&t), (uint16_t)(first + i));
			nxdn_call_tx_sent(&t, k);
			continue;
		}
		if (given < frames) {
			if (seed_voice >= 0)
				for (int i = 0; i < NXDN_EHR_BYTES; i++)
					frame[i] = (uint8_t)rng();
			else if (vf && fread(frame, 1, sizeof frame, vf) == sizeof frame)
				memcpy(fixed, frame, sizeof fixed);
			else
				memcpy(frame, fixed, sizeof frame);
			nxdn_call_tx_voice(&t, frame);
			given++;
		} else if (!ended) {
			nxdn_call_tx_end(&t, fixed);
			ended = 1;
		} else {
			die("the transmit path stalled", NULL);
		}
	}
	for (long i = 0; i < tail; i++)
		syms[n++] = (uint8_t)((idle >> (6 - 2 * (i & 3))) & 3);
	if (ber > 0) {
		rng_seed((uint64_t)opt_long(o, "--seed", 1) + 0x5EED);
		for (size_t i = 0; i < n; i++)
			for (int b = 0; b < 2; b++)
				if (rng_unit() < ber) {
					syms[i] ^= (uint8_t)(1 << b);
					nflip++;
				}
	}
	fprintf(e, "],\"voice_given\":%ld,\"total_syms\":%zu,\"ber\":%g,\"bit_errors_applied\":%ld}\n", given, n, ber, nflip);
	fclose(e);
	write_syms(out_path, syms, n, flag(o, "--unpacked"), idle);
	printf("{\"ev\":\"gen\",\"syms\":%zu,\"frames\":%ld,\"voice_given\":%ld,\"bit_errors_applied\":%ld}\n", n, nframes,
	       given, nflip);
	free(syms);
	return 0;
}

// ── decode and disc: the receive path, every frame printed ──
static nxdn_call_rx_t g_call;
static air_framer_t g_fr;
static const air_mode_t *g_mode;
static void (*g_call_frame)(const nxdn_rx_frame_t *f, void *ctx);
static uint32_t g_frames, g_voice_out;
static int g_disc;			// times are a capture's: no symbol positions

static void frame_hook(const nxdn_rx_frame_t *f, void *ctx)
{
	static nxdn_fec_work_t w;
	uint8_t msg[NXDN_FACCH1_BYTES], ehr[NXDN_EHR_BYTES];

	printf("{\"ev\":\"frame\",\"i\":%u,\"t_us\":%u", g_frames++, f->t_us);
	if (!g_disc)
		printf(",\"at\":%u", (uint32_t)(((uint64_t)f->t_us * g_mode->profile.sym_rate + 500000) / 1000000));
	printf(",\"lich\":%d,\"no_sync\":%s,\"sacch_errs\":%d", f->lich, (f->flags & AIR_FRAME_NO_SYNC) ? "true" : "false",
	       f->sacch_errs);
	if (f->sacch_errs >= 0) {
		printf(",\"sacch\":");
		put_hex(stdout, f->sacch, NXDN_SACCH_BYTES);
	}
	if (f->lich >= 0) {
		uint8_t o = NXDN_LICH_OPT(f->lich);

		printf(",\"facch\":[");
		for (uint8_t h = 0, c = 0; h < 2; h++) {
			int r;

			if (o == NXDN_STEAL_NONE || (o == NXDN_STEAL_FIRST && h == 1) || (o == NXDN_STEAL_SECOND && h == 0))
				continue;
			r = nxdn_facch1_get(f->hard, f->soft, h, &w, msg);
			printf("%s{\"half\":%u,\"errs\":%d", c++ ? "," : "", h, r);
			if (r >= 0) {
				printf(",\"msg\":");
				put_hex(stdout, msg, sizeof msg);
				printf(",\"type\":%u", nxdn_msg_type(msg));
			}
			printf("}");
		}
		printf("]");
	}
	// The VCH as on air: what the voice path would take, opaque here
	printf(",\"vch\":[");
	for (uint8_t n = 0; n < NXDN_VOICE_FRAMES; n++) {
		nxdn_vch_get(f->hard, n, ehr);
		printf("%s", n ? "," : "");
		put_hex(stdout, ehr, sizeof ehr);
	}
	printf("],\"syms\":");
	put_hex(stdout, f->hard, NXDN_FRAME_BYTES);
	printf("}\n");
	g_call_frame(f, ctx);
}

static void call_json(const char *ev, const nxdn_call_t *c)
{
	printf("{\"ev\":\"%s\",\"ran\":%u,\"late\":%s,\"have_vcall\":%s,\"iv\":%s,\"vcall\":", ev, c->ran,
	       c->late ? "true" : "false", c->have_vcall ? "true" : "false", c->iv ? "true" : "false");
	vcall_json(stdout, &c->vcall);
	printf("}\n");
}

static void on_start(const nxdn_call_t *c, void *ctx) { (void)ctx; call_json("call_start", c); }
static void on_update(const nxdn_call_t *c, void *ctx) { (void)ctx; call_json("call_update", c); }
static void on_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	(void)ctx;
	g_voice_out++;
	printf("{\"ev\":\"voice\",\"vch\":");
	put_hex(stdout, ehr, NXDN_EHR_BYTES);
	printf("}\n");
}
static void on_end(void *ctx) { (void)ctx; printf("{\"ev\":\"call_end\"}\n"); }

static const nxdn_call_sink_t sink = { on_start, on_update, on_voice, on_end };

static void rx_setup(const opts_t *o)
{
	static uint8_t hard[NXDN_FRAME_BYTES];
	static int8_t soft[NXDN_FRAME_SYMS];

	g_mode = mode_of(o);
	nxdn_call_rx_init(&g_call, (uint8_t)opt_long(o, "--ran", NXDN_RAN_ANY), &sink, NULL);
	g_call_frame = g_call.rx.frame;
	g_call.rx.frame = frame_hook;
	air_framer_init(&g_fr, g_mode, &g_call.rx, hard, soft, NXDN_FRAME_SYMS);
}

static void rx_end(void)
{
	air_chunk_t c;
	uint8_t z = 0;

	// The end of the input is the end of the signal
	memset(&c, 0, sizeof c);
	c.nsyms = 1;
	c.sync_at = -1;
	c.flags = AIR_CHUNK_GAP | AIR_CHUNK_NO_SIGNAL;
	c.hard = &z;
	air_framer_rx(&g_fr, &c);
}

static void rx_stats(void)
{
	const air_framer_stats_t *s = &g_fr.stats;
	const nxdn_call_stats_t *c = &g_call.stats;

	printf("{\"ev\":\"stats\",\"framer\":{\"syncs\":%u,\"misses\":%u,\"locks\":%u,\"rejects\":%u,\"frames\":%u},"
	       "\"call\":{\"calls\":%u,\"frames\":%u,\"lich_fail\":%u,\"sacch_fail\":%u,\"facch_fail\":%u,\"voice\":%u,"
	       "\"voice_held\":%u,\"fec_bits\":%u},\"ran_rejects\":%u,\"voice_out\":%u}\n", s->syncs, s->misses, s->locks,
	       s->rejects, s->frames, c->calls, c->frames, c->lich_fail, c->sacch_fail, c->facch_fail, c->voice,
	       c->voice_held, c->fec_bits, g_call.rx.ran_rejects, g_voice_out);
}

static int cmd_decode(const char *in, const opts_t *o)
{
	size_t n;
	uint8_t *s = read_syms(in, flag(o, "--unpacked"), &n);
	uint8_t hard[AIR_CHUNK_SYMS_MAX / 4];

	rx_setup(o);
	for (size_t at = 0; at < n; at += AIR_CHUNK_SYMS_MAX) {
		air_chunk_t c;
		uint16_t k = (uint16_t)(n - at < AIR_CHUNK_SYMS_MAX ? n - at : AIR_CHUNK_SYMS_MAX);

		memset(&c, 0, sizeof c);
		for (uint16_t i = 0; i < k; i++)
			air_sym_put(hard, i, s[at + i]);
		c.t_us = (uint32_t)(((uint64_t)at * 1000000u) / g_mode->profile.sym_rate);
		c.nsyms = k;
		c.sync_at = -1;
		c.flags = at ? 0 : AIR_CHUNK_GAP;
		c.hard = hard;
		air_framer_rx(&g_fr, &c);
	}
	rx_end();
	printf("{\"ev\":\"input\",\"syms\":%zu}\n", n);
	rx_stats();
	free(s);
	return 0;
}

static air_softmodem_t g_m;
static uint32_t g_sm_syncs, g_sm_chunks;

static void sm_out(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	g_sm_chunks++;
	if (c->sync_at >= 0) {
		g_sm_syncs++;
		printf("{\"ev\":\"sync\",\"t_ms\":%.3f}\n", (c->t_us + c->sync_at * 1e6 / g_mode->profile.sym_rate) / 1000.0);
	}
	air_framer_rx(&g_fr, c);
}

static int cmd_disc(const char *in, const opts_t *o)
{
	size_t nbytes, n;
	uint8_t *raw = slurp(in, &nbytes);
	long rate = opt_long(o, "--rate", 24000);
	double full = opt_double(o, "--full", 0), cpk = opt_double(o, "--counts-per-khz", 0);
	int inv = flag(o, "--invert");
	int16_t *x;

	g_disc = 1;
	n = nbytes / 2;
	x = malloc((n + 1) * sizeof *x);
	if (!x)
		die("out of memory", NULL);
	for (size_t i = 0; i < n; i++) {
		int32_t v = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8);

		if (inv)
			v = -v;
		x[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	free(raw);
	if (cpk <= 0 && full > 0)
		cpk = 32768.0 / (full / 1000.0);
	if (cpk > 65535)
		die("the scale is past what the soft modem takes (counts per kHz under 65536)", NULL);
	rx_setup(o);
	if (!air_softmodem_rx_open(&g_m, &g_mode->profile, (uint32_t)rate, (uint16_t)lround(cpk), sm_out, NULL))
		die("the soft modem does not take this rate for this mode (8 kHz up to 10 samples a symbol)", NULL);
	for (size_t at = 0; at < n; at += 256) {
		uint16_t k = (uint16_t)(n - at < 256 ? n - at : 256);

		air_softmodem_rx(&g_m, x + at, k, (uint32_t)(((uint64_t)at * 1000000u) / (uint64_t)rate));
	}
	rx_end();
	printf("{\"ev\":\"input\",\"samples\":%zu,\"rate\":%ld,\"seconds\":%.3f,\"counts_per_khz\":%.0f}\n", n, rate,
	       (double)n / rate, cpk);
	printf("{\"ev\":\"softmodem\",\"syncs\":%u,\"chunks\":%u}\n", g_sm_syncs, g_sm_chunks);
	rx_stats();
	free(x);
	return 0;
}

// ── mod: a 4-level FSK transmitter and an FM discriminator, modeled at 240 kHz ──
#define FS	240000
#define SPAN	8

// The transmit pulse (TS 1-A 3.4): root raised cosine, roll-off 0.2, times the shaper sin(pi f T) / (pi f T); DC gain 1
static void nxdn_pulse(double *h, int osr)
{
	int n = 2 * SPAN * osr + 1;
	double rate = (double)FS / osr, sum = 0, alpha = 0.2, f1 = rate * (1 - alpha) / 2, f2 = rate * (1 + alpha) / 2;

	for (int i = 0; i < n; i++) {
		double t = (double)(i - SPAN * osr) / FS, v = 0;

		for (double f = 1.25; f < f2; f += 2.5) {
			double hf = f < f1 ? 1 : sqrt(0.5 + 0.5 * cos(M_PI * (f - f1) / (f2 - f1))), xx = M_PI * f / rate;

			v += hf * sin(xx) / xx * cos(2 * M_PI * f * t);
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
}

static int cmd_mod(const char *in, const char *out_path, const opts_t *o)
{
	size_t n;
	uint8_t *s = read_syms(in, flag(o, "--unpacked"), &n);
	const air_mode_t *m = mode_of(o);
	long rate = opt_long(o, "--rate", 24000);
	double full = opt_double(o, "--full", 5000), dev = m->profile.dev_outer_hz * opt_double(o, "--dev-scale", 100) / 100;
	double off = opt_double(o, "--offset", 0), noise = opt_double(o, "--noise", 0);
	double gap = opt_double(o, "--carrier-gap", 0.2), hi = opt_double(o, "--lpf", 0);
	int osr = FS / m->profile.sym_rate, inv = flag(o, "--invert");
	size_t lead = (size_t)(gap * FS), wn, dec;
	double *w, *h, lpf[241], lsum = 0;
	FILE *f;

	if (rate < 8000 || FS % rate)
		die("--rate must divide 240000 (8000, 12000, 16000, 24000, 48000 ...)", NULL);
	dec = (size_t)(FS / rate);
	if (hi <= 0)
		hi = rate * 0.45 < 6000 ? rate * 0.45 : 6000;
	rng_seed((uint64_t)opt_long(o, "--seed", 7));
	wn = lead + n * (size_t)osr + lead + 2 * 120;
	w = calloc(wn, sizeof *w);
	h = malloc((size_t)(2 * SPAN * osr + 1) * sizeof *h);
	if (!w || !h)
		die("out of memory", NULL);
	nxdn_pulse(h, osr);
	for (size_t k = 0; k < n; k++) {
		long at = (long)lead + 120 + (long)k * osr - SPAN * osr;
		double lv = air_sym_level(s[k]) * dev / 3.0;

		for (int i = 0; i <= 2 * SPAN * osr; i++)
			if (at + i >= 0 && at + i < (long)wn)
				w[at + i] += lv * h[i];
	}
	for (int i = -120; i <= 120; i++) {
		double xx = 2 * M_PI * hi / FS * i, win = 0.54 + 0.46 * cos(M_PI * i / 120);

		lpf[i + 120] = (i ? sin(xx) / xx : 1) * win;
		lsum += lpf[i + 120];
	}
	f = fopen(out_path, "wb");
	if (!f)
		die("can't write", out_path);
	for (size_t j = 120; j + 120 < wn; j += dec) {
		double v = 0;
		int carrier = j >= lead && j < wn - lead;

		for (int i = -120; i <= 120; i++)
			v += w[j + (size_t)i] * lpf[i + 120];
		v = v / lsum + off;
		// No carrier: discriminator noise, well past the signal's deviation
		v += carrier ? noise * gauss() : 2.5 * dev * gauss();
		v = v / full * 32768;
		if (inv)
			v = -v;
		v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
		int16_t q = (int16_t)lround(v);

		fputc(q & 0xFF, f);
		fputc((q >> 8) & 0xFF, f);
	}
	fclose(f);
	printf("{\"ev\":\"mod\",\"syms\":%zu,\"rate\":%ld,\"full_hz\":%.0f,\"dev_hz\":%.1f,\"lpf_hz\":%.0f}\n", n, rate, full,
	       dev, hi);
	free(h);
	free(w);
	free(s);
	return 0;
}

int main(int argc, char **argv)
{
	opts_t o;

	if (argc < 3) {
		fprintf(stderr, "usage: nxdntool gen OUT EXPECT | decode IN | disc IN | mod IN OUT [options]  "
			"(tools/bench/nxdntool.py)\n");
		return 2;
	}
	if ((!strcmp(argv[1], "gen") || !strcmp(argv[1], "mod")) && argc >= 4) {
		o.argc = argc - 4;
		o.argv = argv + 4;
		return argv[1][0] == 'g' ? cmd_gen(argv[2], argv[3], &o) : cmd_mod(argv[2], argv[3], &o);
	}
	o.argc = argc - 3;
	o.argv = argv + 3;
	if (!strcmp(argv[1], "decode"))
		return cmd_decode(argv[2], &o);
	if (!strcmp(argv[1], "disc"))
		return cmd_disc(argv[2], &o);
	die("unknown command", argv[1]);
	return 2;
}
